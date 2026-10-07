// Glue logic: owns every chipset device and the flat 32MB memory window, and
// builds the cpu80486::Bus the CPU talks through.
// 0x00000-0x9FFFF RAM, 0xA0000-0xBFFFF VGA window, 0xC0000-0xC7FFF video BIOS,
// 0xF0000-0xFFFFF system BIOS, 0x100000-0x1FFFFFF extended RAM (behind A20).
// Unpopulated space reads 0xFF and discards writes.
#ifndef PC486_CHIPSET_H
#define PC486_CHIPSET_H

#include "atapi_cdrom.h"
#include "cmos_rtc.h"
#include "cpu80486.h"
#include "dma8237.h"
#include "ega.h"
#include "fdc765.h"
#include "i8042.h"
#include "pcspeaker.h"
#include "pic8259.h"
#include "pit8253.h"
#include "mpu401.h"
#include "soundblaster.h"
#include "wd1003.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pc486 {

class Chipset {
public:
    Chipset() { reset(); }

    void reset();

    // Builds the Cpu callbacks; the returned Bus holds `this`, so the Chipset must
    // outlive the Cpu.
    cpu80486::Bus make_bus();

    // Public because Bus::For's thunks are free functions.
    uint8_t io_in(uint16_t port);
    void io_out(uint16_t port, uint8_t v);
    // Composes two 8-bit accesses, except the IDE data registers (0x1F0, 0x170),
    // which are atomic 16-bit.
    uint16_t io_in16(uint16_t port);
    void io_out16(uint16_t port, uint16_t v);
    uint8_t mem_read(uint32_t addr);
    void mem_write(uint32_t addr, uint8_t v);

    // Resolves a 4KB physical page to a host pointer, or nullptr for the VGA
    // window, unpopulated space, or (for writes) a page holding any ROM byte.
    // Applies A20 like mem_read/mem_write.
    uint8_t *page_host(uint32_t page_base, bool write);
    // Bumped whenever a resolved page could now resolve differently.
    const uint32_t *map_epoch() const { return &map_epoch_; }

    // 32MB: 4x8MB SIMMs. Heap-backed; a stack-allocated Chipset would overflow the stack.
    static constexpr std::size_t kRamSize = 32u * 1024u * 1024u;
    std::vector<uint8_t> mem = std::vector<uint8_t>(kRamSize, 0);
    // Marks [addr, addr+len) read-only and copies `data` in.
    void load_rom(uint32_t addr, const uint8_t *data, std::size_t len);

    Pic8259 pic_master{0x20};
    Pic8259 pic_slave{0xA0};
    Pit8253 pit;
    I8042 kbc;
    CmosRtc cmos;
    Dma8237 dma1{0x00, 1};
    Dma8237 dma2{0xC0, 2};
    Fdc765 fdc;
    // VGA card; the class is still named Ega.
    Ega vga;
    // Primary IDE channel (0x1F0, IRQ14).
    Wd1003 hdd;
    // Secondary IDE channel (0x170, IRQ15): the ATAPI CD-ROM.
    AtapiCdrom cdrom;
    PcSpeaker speaker;
    // ISA 0x220, IRQ5, DMA1/DMA5 (SET BLASTER=A220 I5 D1 H5 T6).
    SoundBlaster sb;
    // The SB16's MPU-401, at its own jumper-selected base.
    Mpu401 mpu;

    // Relays a mouse event to the 8042 AUX port. +dy is away from the user, so a
    // browser movementY must be negated.
    void inject_mouse_event(int dx, int dy, uint8_t buttons) {
        kbc.inject_mouse_event(dx, dy, buttons);
        next_service_ = 0;  // the packet's IRQ12 is picked up by the next tick()
    }

    // Port 0x61: bit0 gates PIT ch2, bit1 enables the speaker, bit4 is the refresh
    // toggle, bit5 reads ch2 output.
    uint8_t port61() const;
    void set_port61(uint8_t v);

    // Advances the PIT and pulses IRQ0, flips the refresh toggle, and ticks the
    // floppy, VGA, HDD and CD-ROM. Called once per CPU instruction.
    // The service pass runs on the chipset's own slow clock (PIT 1.193182 MHz),
    // so next_service_ skips it until then. Any port access reopens the gate; A20
    // is checked on every call. Returns true when the full pass ran.
    bool tick(uint64_t cpu_cycles, double cpu_hz) {
        note_a20();
        if (cpu_cycles < next_service_) return false;
        PC486_PERF_BUMP(perf_services);
        service(cpu_cycles, cpu_hz);
        return true;
    }
#ifdef PC486_PERF
    uint64_t perf_services = 0;
#endif

    // One INTA cycle, cascading through the slave on IR2. Returns -1 if none pending.
    int poll_interrupt();
    bool has_interrupt() const { return pic_master.has_interrupt() || pic_slave.has_interrupt(); }

    // Port 0x80: BIOS POST code.
    uint8_t last_post_code() const { return last_post_code_; }

    // Port 0xE9: Bochs debug console, written by the Bochs-legacy BIOS.
    const std::string &debug_console() const { return debug_console_; }

    // Passes the CPU clock to devices that convert seconds into CPU cycles.
    void set_cpu_hz(double hz);

    // Board bus timing, told about each DMA transfer. Null for a bare chipset.
    Cache486 *timing = nullptr;
    const uint64_t *timing_clock = nullptr;

private:
    void dma_timing(uint32_t phys, int size, bool to_mem) {
        if (timing) timing->dma(phys, size, to_mem, *timing_clock);
    }
    // vector<bool> is bit-packed (4MB, not 32MB).
    std::vector<bool> rom_ = std::vector<bool>(kRamSize, false);
    // Set for any page holding a ROM byte, so page_host() can refuse write pointers.
    std::vector<bool> rom_page_ = std::vector<bool>(kRamSize / 4096u, false);
    uint32_t map_epoch_ = 1;
    // A20 comes from the 8042 output port. Checked after controller writes and on
    // every tick().
    bool a20_prev_ = false;
    // VGA mapping changes invalidate cached VRAM pointers like an A20 change does.
    void note_vga_mapping() {
        if (vga.mapping_epoch() != vga_map_prev_) {
            vga_map_prev_ = vga.mapping_epoch();
            ++map_epoch_;
        }
    }

    void note_a20() {
        if (kbc.a20_enabled() != a20_prev_) { a20_prev_ = kbc.a20_enabled(); ++map_epoch_; }
    }
    // The BIOS ROM also appears in the top 2MB of the 4GB space (reset vector
    // FFFFFFF0h), mirrored every 128KB.
    static uint32_t rom_alias(uint32_t addr) {
        return addr >= 0xFFE00000u ? (0xE0000u | (addr & 0x1FFFFu)) : addr;
    }
    // Cycle count at which tick() next runs the full service pass; 0 means now.
    uint64_t next_service_ = 0;
    void service(uint64_t cpu_cycles, double cpu_hz);
    uint8_t port61_ = 0x00;
    bool refresh_toggle_ = false;
    uint8_t last_post_code_ = 0x00;
    std::string debug_console_;
    // Edge-triggered ISA IRQs; re-raising every tick causes an interrupt storm.
    bool fdc_irq_prev_ = false;
    bool hdd_irq_prev_ = false;   // IRQ14 (hard disk, slave PIC line 6)
    bool cdrom_irq_prev_ = false; // IRQ15 (CD-ROM, slave PIC line 7)
    uint32_t vga_map_prev_ = 0xFFFFFFFFu;
    bool sb_irq_prev_ = false;    // Sound Blaster, line picked by mixer 80h (IRQ5 default)
    bool rtc_irq_prev_ = false;   // RTC, IRQ8

    // Moves one Sound Blaster DMA block (8-bit ch1 or 16-bit ch5) per completed paced wait.
    void io_out_impl(uint16_t port, uint8_t v);
    void service_sb_dma();
};

}  // namespace pc486

#endif  // PC486_CHIPSET_H
