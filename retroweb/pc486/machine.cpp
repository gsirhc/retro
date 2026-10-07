#include "machine.h"

namespace pc486 {

void Machine::set_cpu_hz(double hz) {
    if (!(hz > 0.0) || hz == cpu_hz_) return;
    cpu_hz_ = hz;
    chipset.set_cpu_hz(hz);
    set_rep_yield();
}

void Machine::configure_factory_cmos() {
    auto &c = chipset.cmos;
    // Floppy types (0x10): high nibble A:, low nibble B:. 4 = 1.44MB 3.5", no B:
    c.poke(0x10, 0x40);
    // Equipment byte (0x14): bit0 = floppy installed, bit2 = pointing device. The
    // BIOS copies it to the INT 11h equipment word, and CuteMouse tests bit 2
    // before trying INT 15h AH=C2h (PC486_REVIEW.md §13).
    // Floppy-count and video-type sub-fields are not set, unverified against this BIOS.
    c.poke(0x14, 0x05);
    // Base memory, 640KB, word at 0x15/0x16
    c.poke(0x15, 0x80);
    c.poke(0x16, 0x02);
    // Extended memory above 1MB in KB, word at 0x17/0x18 (31MB = 0x7C00), the Setup copy
    c.poke(0x17, 0x00);
    c.poke(0x18, 0x7C);
    // POST copy at 0x30/0x31. This BIOS reads only these (rombios.c INT 15h AH=88h
    // and AX=E801); zero here made HimemX refuse to load (PC486_REVIEW.md §5.3).
    c.poke(0x30, 0x00);
    c.poke(0x31, 0x7C);
    // Memory above 16MB in 64KB units (0x34/0x35), the other half of the E801
    // answer: 32MB gives 16MB / 64KB = 0x0100
    c.poke(0x34, 0x00);
    c.poke(0x35, 0x01);
    // Boot sequence (BX_ELTORITO_BOOT): low nibble of 0x3D is 1st device, high
    // nibble 2nd; 1 = floppy, 2 = hard disk. 0x21 = floppy then disk. No CD boot.
    c.poke(0x3D, 0x21);

    // Type 47 user-defined geometry (0x12, 0x19, 0x1B-0x23): WD Caviar AC2250,
    // 1010 cyl / 9 heads / 55 spt, matching wd1003.cpp's IDENTIFY
    c.poke(0x12, 0xF0);  // drive C: = extended type; no drive D:
    c.poke(0x19, 47);
    c.poke(0x1B, uint8_t(Wd1003::kCylinders & 0xFF));  // cylinders low  (1010 = 0x3F2)
    c.poke(0x1C, uint8_t(Wd1003::kCylinders >> 8));    // cylinders high
    c.poke(0x1D, Wd1003::kHeads);
    c.poke(0x1E, 0xFF);  // write precomp low  -- 0xFFFF = "none"
    c.poke(0x1F, 0xFF);  // write precomp high
    // Control byte bit 3 is the Phoenix/AMI "more than 8 heads" flag; 9 heads
    // needs it. Unverified against a boot.
    c.poke(0x20, 0x08);
    c.poke(0x21, uint8_t(Wd1003::kCylinders & 0xFF));  // landing zone low  (same as max cylinder)
    c.poke(0x22, uint8_t(Wd1003::kCylinders >> 8));    // landing zone high
    c.poke(0x23, Wd1003::kSectorsPerTrack);

    // Checksum over 0x10-0x2D, big-endian at 0x2E/0x2F; kept consistent though
    // this BIOS doesn't seem to enforce it
    uint16_t sum = 0;
    for (uint16_t reg = 0x10; reg <= 0x2D; ++reg) sum = uint16_t(sum + c.peek(uint8_t(reg)));
    c.poke(0x2E, uint8_t(sum >> 8));
    c.poke(0x2F, uint8_t(sum & 0xFF));
}

void Machine::run_cycles(int64_t cycles) {
    int64_t target = int64_t(total_cycles_) + cycles;
    service_kbc_reset();
    while (int64_t(total_cycles_) < target) {
        if (on_instruction) on_instruction(on_instruction_ctx, *this);
        PC486_PERF_BUMP(cpu.perf.instrs);
        int spent = cpu.step();
        total_cycles_ += uint64_t(spent);
        // The 8042 pulses CPU RESET from its output port, which re-opens the chipset
        // service gate (Chipset::tick), so asking on the serviced pass catches it
        if (chipset.tick(total_cycles_, cpu_hz_)) service_kbc_reset();
        // AT boards decode the shutdown cycle and pulse RESET (IBM AT Technical
        // Reference, "Shutdown"); memory and CMOS survive.
        if (cpu.shutdown()) reset_cpu();
        // INTA only starts if IF permits. Guarding poll_interrupt() with the inline
        // has_interrupt() is exact: with no unmasked pending line its only effect is
        // lower(2) on a clear IR2 (PC486_REVIEW.md §8).
        if (cpu.flag(cpu80486::FLAG_IF) && !cpu.interrupt_shadow() && chipset.has_interrupt()) {
            int vec = chipset.poll_interrupt();
            if (vec >= 0) cpu.interrupt(uint8_t(vec));
        }
    }
}

}  // namespace pc486
