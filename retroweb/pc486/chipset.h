// This machine's "glue logic": owns every chipset device plus the flat
// 32MB memory window, and builds the cpu80486::Bus the CPU core talks
// through. Plays the same role ibmpc-at/chipset.h plays for the AT, scaled
// up to this machine's period-correct "maxed out gamer 486" RAM size and a
// second IDE channel for the ATAPI CD-ROM.
//
// Memory map, as populated on this system:
//   0x0000000-0x009FFFF  640KB conventional RAM
//   0x00A0000-0x00BFFFF  VGA video RAM window (routed to `vga`, not `mem`)
//   0x00C0000-0x00C7FFF  VGA video BIOS extension ROM window
//   0x00F0000-0x00FFFFF  system BIOS ROM
//   0x0100000-0x1FFFFFF  31MB extended RAM -- genuinely present (this is a
//     "maxed out" 32MB-RAM period gaming build), counted and reported by a
//     real BIOS POST ("Extended memory test: 31744K OK" or similar), and
//     genuinely reachable two ways: FreeDOS's HimemX addresses it from real
//     mode with the "unreal mode" trick (PC486_REVIEW.md §5.4), and a
//     protected-mode program addresses it directly through a flat descriptor
//     (§6). Note the A20 gate below still applies to both -- software has to
//     open it before any of this memory is reachable at all, which is the
//     chipset's job, not the CPU's.
// Anything not populated reads as 0xFF (open bus) and discards writes,
// matching real hardware with nothing wired to that address range.
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

    // Builds the callbacks cpu80486::Cpu is constructed with. The returned
    // Bus holds `this` as its ctx pointer -- the Chipset must outlive any
    // Cpu built from it.
    cpu80486::Bus make_bus();

    // The six bus operations make_bus() binds. Public because Bus::For's
    // thunks are free functions rather than Chipset members (see
    // cpu80486.h) -- and because the Bus hands every one of them to the CPU
    // regardless, so there was never any encapsulation here to lose.
    uint8_t io_in(uint16_t port);
    void io_out(uint16_t port, uint8_t v);
    // Default: compose two 8-bit accesses (port, port+1) -- correct for
    // every device except the two IDE channels' inherently-16-bit data
    // registers (0x1F0 for the HDD, 0x170 for the CD-ROM), which special-
    // case atomically. See cpu80486.h's Bus::in16/out16 comment.
    uint16_t io_in16(uint16_t port);
    void io_out16(uint16_t port, uint16_t v);
    uint8_t mem_read(uint32_t addr);
    void mem_write(uint32_t addr, uint8_t v);

    // Resolves a whole 4KB physical page to a host pointer, or nullptr when
    // the page is not plain memory: the VGA window (the card answers, not
    // RAM), unpopulated space above `mem`, or -- on the write side -- a page
    // holding any ROM byte. Which backing store an address belongs to is a
    // property of the page, not of the byte, so the CPU resolves it once and
    // reads/writes every byte of that page directly (PC486_REVIEW.md §15).
    // The A20 gate is applied here exactly as mem_read/mem_write apply it.
    uint8_t *page_host(uint32_t page_base, bool write);
    // Generation counter for the mapping page_host() returns: bumped
    // whenever an already-resolved page could now resolve differently.
    const uint32_t *map_epoch() const { return &map_epoch_; }

    // 32MB: 4x8MB SIMMs, a genuinely high-end 486 configuration in 1993-94
    // (64MB+ was server/workstation territory, not a "gamer's dream" box).
    // Heap-backed (not std::array like ibmpc-at's 1MB map): at this size, a
    // stack-allocated Chipset/Machine -- which every test constructs as a
    // plain local -- would overflow the default thread stack outright.
    static constexpr std::size_t kRamSize = 32u * 1024u * 1024u;
    std::vector<uint8_t> mem = std::vector<uint8_t>(kRamSize, 0);
    // Marks [addr, addr+len) read-only (a ROM image) and copies `data` in.
    void load_rom(uint32_t addr, const uint8_t *data, std::size_t len);

    Pic8259 pic_master{0x20};
    Pic8259 pic_slave{0xA0};
    Pit8253 pit;
    I8042 kbc;
    CmosRtc cmos;
    Dma8237 dma1{0x00, 1};
    Dma8237 dma2{0xC0, 2};
    Fdc765 fdc;
    // This machine's video adapter is a VGA card, not a genuine EGA board --
    // the class is still named `Ega` for continuity with Milestone 1 (see
    // ega.h's own header comment), but it now implements the genuinely-VGA
    // parts too: Chain-4 mode 13h, the 256-entry DAC, and the SVGA
    // extension registers carrying the VESA BIOS Extensions (§7). Named
    // `vga` here to reflect what's actually installed in this machine.
    Ega vga;
    // Primary IDE channel (0x1F0-0x1F7/0x3F6, IRQ14): the hard disk.
    Wd1003 hdd;
    // Secondary IDE channel (0x170-0x177/0x376, IRQ15): the ATAPI CD-ROM --
    // the standard secondary-channel address/IRQ convention, a separate
    // cable from the primary channel on real period IDE hardware.
    AtapiCdrom cdrom;
    PcSpeaker speaker;
    // ISA base 0x220, IRQ5, 8-bit DMA1/16-bit DMA5 -- the period-standard
    // `SET BLASTER=A220 I5 D1 H5 T6` a period driver finds in its
    // environment (§11).
    SoundBlaster sb;

    // Host-facing: relay a mouse movement/button event into the 8042's
    // AUX port. `dy` follows the mouse's own axis convention (+Y is away
    // from the user) -- a caller passing a browser `movementY` must negate
    // it first (§10).
    void inject_mouse_event(int dx, int dy, uint8_t buttons) {
        kbc.inject_mouse_event(dx, dy, buttons);
        next_service_ = 0;  // the packet's IRQ12 is picked up by the next tick()
    }

    // Port 0x61 ("PPI port B" equivalent): bit0 gates PIT channel 2
    // (speaker), bit1 enables the speaker data path, bit4 is a refresh-
    // activity toggle a BIOS's memory-refresh POST test polls for
    // liveness, bit5 reads channel 2's current output level back.
    uint8_t port61() const;
    void set_port61(uint8_t v);

    // Advances the PIT against the CPU's running cycle count and pulses
    // PIC IRQ0 for every channel-0 rising edge it reports; flips the
    // refresh-activity toggle; recomputes the speaker's AND-gate signal;
    // ticks the floppy, VGA, HDD, and CD-ROM devices and cascades their
    // IRQs. Machine::run_cycles() calls this once per CPU instruction.
    //
    // The service pass itself runs on the chipset's own clock, not the CPU's
    // instruction stream. Not one device here is clocked by the CPU: the
    // 8253 runs at a fixed 1.193182 MHz off the 14.31818 MHz crystal (one
    // count every ~55 CPU clocks at 66 MHz, see pit8253.h), the drives and
    // the Sound Blaster's DAC pace themselves in milliseconds, and the
    // card's retrace runs off the pixel clock. So nothing any of them does
    // can become observable between two counts of the fastest of those
    // clocks, and `next_service_` skips the pass until then. Any port access
    // re-opens the gate (see io_in/io_out), so a guest polling a device is
    // still serviced on every poll, and A20 is still checked on every call.
    // Returns true when the full pass actually ran, so a caller with work of
    // its own that hangs off a device signal (Machine::run_cycles and the
    // 8042's CPU RESET line) can hang it off the same gate.
    bool tick(uint64_t cpu_cycles, double cpu_hz) {
        note_a20();
        if (cpu_cycles < next_service_) return false;
        service(cpu_cycles, cpu_hz);
        return true;
    }

    // Services one INTA cycle: cascades through the slave when the
    // master's highest-pending line is IR2, exactly like real AT-derived
    // wiring (unchanged on a period 486 board's Super I/O chipset).
    // Returns -1 if nothing is pending.
    int poll_interrupt();
    bool has_interrupt() const { return pic_master.has_interrupt() || pic_slave.has_interrupt(); }

    // Port 0x80: the classic BIOS POST-diagnostic-code sink.
    uint8_t last_post_code() const { return last_post_code_; }

    // Port 0xE9: the Bochs/QEMU "debug console" convention, same as
    // ibmpc-at -- this machine's BIOS substitute is the same Bochs-legacy
    // build and opportunistically writes progress/panic text there.
    const std::string &debug_console() const { return debug_console_; }

private:
    // vector<bool> is bit-packed (4MB, not 32MB) -- same stack-overflow
    // reasoning as `mem` above, plus it'd otherwise double the footprint.
    std::vector<bool> rom_ = std::vector<bool>(kRamSize, false);
    // Page-granular view of rom_: set for any page holding at least one ROM
    // byte, so page_host() can refuse a write pointer to a partly-ROM page
    // without walking 4096 bits.
    std::vector<bool> rom_page_ = std::vector<bool>(kRamSize / 4096u, false);
    uint32_t map_epoch_ = 1;
    // A20 is driven by the 8042's output port and by nothing else on this
    // machine. Checked after every write to the controller (where a guest
    // moves it) and once per tick() (so a host poking kbc directly is
    // noticed too), since moving the gate changes what every page above 1MB
    // resolves to.
    bool a20_prev_ = false;
    void note_a20() {
        if (kbc.a20_enabled() != a20_prev_) { a20_prev_ = kbc.a20_enabled(); ++map_epoch_; }
    }
    // The cycle count at or after which tick() runs its full service pass
    // again (see tick()). 0 means "on the next call".
    uint64_t next_service_ = 0;
    void service(uint64_t cpu_cycles, double cpu_hz);
    uint8_t port61_ = 0x00;
    bool refresh_toggle_ = false;
    uint8_t last_post_code_ = 0x00;
    std::string debug_console_;
    // Edge-triggered ISA IRQs -- see ibmpc-at/chipset.h's fdc_irq_prev_
    // comment for why re-raising on every tick (rather than only on the
    // 0->1 transition) causes a real interrupt-storm bug.
    bool fdc_irq_prev_ = false;
    bool kbc_irq_prev_ = false;   // IRQ1 (keyboard)
    bool hdd_irq_prev_ = false;   // IRQ14 (hard disk, slave PIC line 6)
    bool cdrom_irq_prev_ = false; // IRQ15 (CD-ROM, slave PIC line 7)
    bool sb_irq_prev_ = false;    // IRQ5 (Sound Blaster, master PIC line 5)

    // Moves one Sound Blaster DMA block (8-bit channel 1 or 16-bit channel
    // 5) once its paced transfer is ready -- the same "one bulk copy per
    // completed real-time wait" shape as the floppy's handoff above, with
    // the two SB16-specific corrections `soundblaster.h`'s header
    // documents (16-bit channel counts words, and auto-init needs the DMA
    // controller to reload at terminal count -- now real, see dma8237.h).
    void service_sb_dma();
};

}  // namespace pc486

#endif  // PC486_CHIPSET_H
