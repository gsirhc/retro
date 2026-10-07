// AT glue logic: owns the chipset devices and the 1MB memory window, and
// builds the cpu80286::Bus. The A20 gate lives here, not in the CPU.
//
// Memory map (640KB conventional, no extended memory):
//   0x00000-0x9FFFF  640KB conventional RAM
//   0xA0000-0xBFFFF  EGA video RAM window (routed to `ega`, not `mem`)
//   0xC0000-0xC7FFF  EGA video BIOS extension ROM window (vgabios)
//   0xF0000-0xFFFFF  system BIOS ROM
// Unpopulated addresses read 0xFF and discard writes.
#ifndef IBMPCAT_CHIPSET_H
#define IBMPCAT_CHIPSET_H

#include "cmos_rtc.h"
#include "cpu80286.h"
#include "dma8237.h"
#include "ega.h"
#include "fdc765.h"
#include "i8042.h"
#include "pcspeaker.h"
#include "pic8259.h"
#include "pit8253.h"
#include "wd1003.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ibmpcat {

class Chipset {
public:
    Chipset() { reset(); }

    void reset();

    // The returned Bus captures `this`; the Chipset must outlive any Cpu built from it.
    cpu80286::Bus make_bus();

    std::array<uint8_t, 0x100000> mem{};
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
    Ega ega;
    Wd1003 hdd;
    PcSpeaker speaker;

    // Port 0x61: bit0 gates PIT ch2, bit1 enables the speaker, bit4 is the
    // refresh toggle, bit5 reads back ch2 output.
    uint8_t port61() const;
    void set_port61(uint8_t v);

    // Advances the PIT and pulses IRQ0 per ch0 rising edge, flips the refresh
    // toggle, updates the speaker. Called once per CPU instruction.
    void tick(uint64_t cpu_cycles, double cpu_hz);

    // One INTA cycle, cascading through the slave when the master's top line is IR2.
    // Returns -1 if nothing is pending.
    int poll_interrupt();
    bool has_interrupt() const { return pic_master.has_interrupt() || pic_slave.has_interrupt(); }

    // Port 0x80: last BIOS POST code written.
    uint8_t last_post_code() const { return last_post_code_; }

    // Port 0xE9: Bochs/QEMU debug console. The Bochs BIOS writes progress text here.
    const std::string &debug_console() const { return debug_console_; }

private:
    std::array<bool, 0x100000> rom_{};
    uint8_t port61_ = 0x00;
    bool refresh_toggle_ = false;
    uint8_t last_post_code_ = 0x00;
    std::string debug_console_;
    // IRQ6 is edge-triggered: raised on the 0->1 transition only. Re-raising every tick
    // storms the CPU when a BIOS handler leaves FDC result bytes undrained.
    bool fdc_irq_prev_ = false;
    bool kbc_irq_prev_ = false;
    bool hdd_irq_prev_ = false;

    uint8_t io_in(uint16_t port);
    void io_out(uint16_t port, uint8_t v);
    // Composes two 8-bit accesses, except the HDD data register at 0x1F0.
    uint16_t io_in16(uint16_t port);
    void io_out16(uint16_t port, uint16_t v);
    uint8_t mem_read(uint32_t addr);
    void mem_write(uint32_t addr, uint8_t v);
};

}  // namespace ibmpcat

#endif  // IBMPCAT_CHIPSET_H
