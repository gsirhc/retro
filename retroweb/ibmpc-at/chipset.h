// The AT's "glue logic": owns every chipset device plus the flat 1MB
// memory window, and builds the cpu80286::Bus the CPU core talks through.
// Plays the same role cg-oac-6502's bus.h plays for its 74HC00 address
// decode, scaled up to an ISA-bus machine's separate port space plus its
// own memory decode (including the A20 gate, which is a motherboard-level
// concern per cpu80286.h's own header comment -- the CPU never masks
// addresses itself).
//
// Memory map, as populated on this system (640KB conventional, no
// extended memory -- see IBM_PCAT_REVIEW.md and the approved plan):
//   0x00000-0x9FFFF  640KB conventional RAM
//   0xA0000-0xBFFFF  EGA video RAM window (routed to `ega`, not `mem`)
//   0xC0000-0xC7FFF  EGA video BIOS extension ROM window (vgabios)
//   0xF0000-0xFFFFF  system BIOS ROM
// Anything not populated reads as 0xFF (open bus) and discards writes,
// matching a real AT with nothing wired to that address range.
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

    // Builds the callbacks cpu80286::Cpu is constructed with. The returned
    // Bus's std::functions capture `this` by reference -- the Chipset must
    // outlive any Cpu built from it.
    cpu80286::Bus make_bus();

    std::array<uint8_t, 0x100000> mem{};
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
    Ega ega;
    Wd1003 hdd;
    PcSpeaker speaker;

    // Port 0x61 ("PPI port B" equivalent): bit0 gates PIT channel 2
    // (speaker), bit1 enables the speaker data path, bit4 is a refresh-
    // activity toggle a BIOS's memory-refresh POST test polls for
    // liveness, bit5 reads channel 2's current output level back.
    uint8_t port61() const;
    void set_port61(uint8_t v);

    // Advances the PIT against the CPU's running cycle count and pulses
    // PIC IRQ0 for every channel-0 rising edge it reports; flips the
    // refresh-activity toggle once per call; recomputes the speaker's
    // AND-gate signal from the current Port 0x61 Speaker Data Enable bit
    // and PIT channel 2's output. Machine::run_cycles() calls this once
    // per CPU instruction (not literally once per video frame, despite the
    // "call once per frame" phrasing disk88/cassette's tick() convention
    // elsewhere in this codebase uses) -- fine enough granularity for the
    // speaker's direct-toggle digitized-playback technique to be captured
    // accurately.
    void tick(uint64_t cpu_cycles, double cpu_hz);

    // Services one INTA cycle: cascades through the slave when the
    // master's highest-pending line is IR2, exactly like real AT wiring.
    // Returns -1 if nothing is pending.
    int poll_interrupt();
    bool has_interrupt() const { return pic_master.has_interrupt() || pic_slave.has_interrupt(); }

    // Port 0x80 is the classic BIOS POST-diagnostic-code sink -- real
    // hardware has nothing listening on it beyond an (optional) LED
    // display; this core just remembers the last byte written so a native
    // test/diagnostic harness can observe POST progress the way a real
    // debug card would.
    uint8_t last_post_code() const { return last_post_code_; }

    // Port 0xE9: the well-known Bochs/QEMU "debug console" convention --
    // bytes written here have no real hardware meaning at all (no genuine
    // AT has anything wired to 0xE9) but the Bochs BIOS this machine boots
    // (and many others) opportunistically writes ASCII progress/panic text
    // there if a debug console might be listening. Captured verbatim so a
    // native diagnostic harness can read out what the BIOS was trying to
    // report, e.g. around a POST failure -- not exposed to the CPU as
    // anything it can read back, matching real hardware having nothing
    // there to read.
    const std::string &debug_console() const { return debug_console_; }

private:
    std::array<bool, 0x100000> rom_{};
    uint8_t port61_ = 0x00;
    bool refresh_toggle_ = false;
    uint8_t last_post_code_ = 0x00;
    std::string debug_console_;
    // IRQ6 (the FDC's interrupt line) is genuine ISA edge-triggered, like
    // every other ISA IRQ on a real AT -- it should be raised once, on the
    // 0->1 transition of fdc.irq_pending(), not re-raised every tick for
    // as long as the condition merely remains true. Getting this wrong
    // was a real bug this session hit: some real BIOS interrupt handlers
    // legitimately return without draining the FDC's result bytes
    // themselves (leaving that to foreground code that polls a status
    // flag later) -- re-raising IRQ6 on every subsequent tick while it
    // waited turned that into an infinite interrupt storm that starved
    // the foreground code of any chance to run. See IBM_PCAT_REVIEW.md §8.
    bool fdc_irq_prev_ = false;
    bool kbc_irq_prev_ = false;  // same edge-triggering discipline, for IRQ1 (keyboard)
    bool hdd_irq_prev_ = false;  // ...and for IRQ14 (hard disk)

    uint8_t io_in(uint16_t port);
    void io_out(uint16_t port, uint8_t v);
    // Default: compose two 8-bit accesses (port, port+1) -- correct for
    // every device except the hard disk controller's inherently-16-bit
    // data register, which special-cases port 0x1F0. See cpu80286.h's
    // Bus::in16/out16 comment.
    uint16_t io_in16(uint16_t port);
    void io_out16(uint16_t port, uint16_t v);
    uint8_t mem_read(uint32_t addr);
    void mem_write(uint32_t addr, uint8_t v);
};

}  // namespace ibmpcat

#endif  // IBMPCAT_CHIPSET_H
