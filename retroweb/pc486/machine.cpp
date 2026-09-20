#include "machine.h"

namespace pc486 {

void Machine::configure_factory_cmos() {
    auto &c = chipset.cmos;
    // Floppy drive types (0x10): high nibble = drive A:, low nibble =
    // drive B:. Standard AT CMOS floppy-type encoding: 4 = 1.44MB 3.5".
    // Low nibble 0 = no B: drive -- this machine has a single floppy bay.
    c.poke(0x10, 0x40);
    // Equipment byte (0x14): bit0 = at least one floppy drive installed,
    // bit2 = a pointing device is installed. The BIOS copies this byte into
    // the BDA's INT 11h equipment word at 0x410, where bit 2 is the standard
    // "PS/2 mouse installed" flag -- and that flag is what a period mouse
    // driver gates its whole PS/2 path on: CuteMouse 2.1 (FreeDOS's CTMOUSE)
    // opens with `INT 11h / TEST AL,4 / JZ give-up` before it will even try
    // INT 15h AH=C2h. This machine has a mouse on the 8042's AUX port
    // (i8042.h), so its CMOS says so, exactly as a real board's Setup/POST
    // records the port. See PC486_REVIEW.md §13.
    // Floppy-count (bits 6-7) and video-type (bits 4-5) sub-fields aren't
    // asserted here, same hedge ibmpc-at/machine.cpp takes for the same
    // reason: not empirically verified against this specific BIOS build.
    c.poke(0x14, 0x05);
    // Base memory size, 640KB, little-endian word (0x15 low, 0x16 high) --
    // unchanged from the AT; conventional memory is always 640KB
    // regardless of how much extended RAM is installed.
    c.poke(0x15, 0x80);
    c.poke(0x16, 0x02);
    // Extended memory size above 1MB, little-endian word in KB, at the
    // standard AT CMOS Map location (0x17/0x18) -- 31MB = 31744KB =
    // 0x7C00. This is the "Setup-configured" copy.
    c.poke(0x17, 0x00);
    c.poke(0x18, 0x7C);
    // The "POST-verified" copy of the same figure, at 0x30/0x31. Real AT
    // CMOS maps carry extended memory size in both places and they hold
    // the same value; this BIOS reads *only* 0x30/0x31 -- `rombios.c`'s
    // INT 15h AH=88h ("Get the amount of extended memory") and AX=E801
    // both do `inb_cmos(0x30)`/`inb_cmos(0x31)` and never look at
    // 0x17/0x18 at all. Leaving these zero made every extended-memory
    // query report none, so HimemX refused to install ("Extended memory
    // is too small or not available") and this 32MB machine ran with no
    // XMS whatsoever. See PC486_REVIEW.md §5.3.
    c.poke(0x30, 0x00);
    c.poke(0x31, 0x7C);
    // Memory above 16MB, in 64KB units (0x34/0x35) -- the other half of
    // the same INT 15h AX=E801 answer, which caps its 0x30/0x31-derived
    // figure at 15MB and reports the remainder from here. 32MB total =
    // 16MB above the 16MB line = 16384KB / 64 = 256 = 0x0100.
    c.poke(0x34, 0x00);
    c.poke(0x35, 0x01);
    // Boot device sequence (BX_ELTORITO_BOOT convention, same as the AT):
    // low nibble of 0x3D selects the 1st boot device, high nibble the
    // 2nd -- 0x01=floppy, 0x02=hard disk. 0x21 = floppy first, falling
    // back to the fixed disk, the standard sequence for a machine with a
    // hard disk installed. CD-ROM booting (El-Torito) is out of scope for
    // Milestone 1 -- the CD-ROM only needs to be reachable from DOS via
    // MSCDEX, not bootable.
    c.poke(0x3D, 0x21);

    // Fixed-disk "Type 47 user-definable" geometry (0x12, 0x19, 0x1B-0x23):
    // 1024 cylinders / 16 heads / 63 sectors/track -- the genuine pre-EIDE
    // INT13h CHS addressing ceiling (504MB / 528,482,304 bytes), the
    // period-correct "maxed out" geometry this machine's HDD uses. See
    // wd1003.h and PC486_REVIEW.md.
    c.poke(0x12, 0xF0);  // drive C: = extended type; no drive D:
    c.poke(0x19, 47);
    c.poke(0x1B, 0x00);  // cylinders low  (1024 = 0x400)
    c.poke(0x1C, 0x04);  // cylinders high
    c.poke(0x1D, 16);    // heads
    c.poke(0x1E, 0xFF);  // write precomp low  -- 0xFFFF = "none"
    c.poke(0x1F, 0xFF);  // write precomp high
    // Control byte bit3 is the standard Phoenix/AMI CMOS-map "more than 8
    // heads" flag; this drive's 16 heads needs it set, unlike the AT's
    // ST-4038 (5 heads, control byte 0x00). NOT yet verified against an
    // actual boot -- same hedge as the extended-memory fields above.
    c.poke(0x20, 0x08);
    c.poke(0x21, 0x00);  // landing zone low  (1024, same as max cylinder)
    c.poke(0x22, 0x04);  // landing zone high
    c.poke(0x23, 63);    // sectors per track

    // CMOS checksum over bytes 0x10-0x2D, stored big-endian at 0x2E/0x2F --
    // kept internally consistent even though this BIOS build hasn't been
    // observed to actually enforce it (same as ibmpc-at).
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
        int spent = cpu.step();
        total_cycles_ += uint64_t(spent);
        // The 8042 drives the CPU's RESET line from its own output port, and
        // only a write to the controller can pulse it -- which re-opens the
        // chipset's service gate (see Chipset::tick), so asking on the
        // serviced pass catches the pulse on exactly the instruction boundary
        // a per-instruction check did.
        if (chipset.tick(total_cycles_, kCpuHz)) service_kbc_reset();
        // Real hardware only begins an INTA cycle if the CPU's IF flag
        // permits it to respond to INTR -- see ibmpc-at/machine.cpp's
        // identical comment; unchanged reasoning on this CPU.
        // Chipset::has_interrupt() is an inline pair of mask tests; the full
        // poll_interrupt() (cascade-line update + INTA acknowledge) is an
        // out-of-line call that overwhelmingly finds nothing to do. Guarding
        // it is exact, not an approximation: when neither PIC has an
        // unmasked pending line, poll_interrupt()'s only side effect is
        // lower(2) on an already-clear master IR2 -- if IR2 *were* set, the
        // master would report a pending interrupt and this guard would let
        // the call through. See PC486_REVIEW.md §8.
        if (cpu.flag(cpu80486::FLAG_IF) && chipset.has_interrupt()) {
            int vec = chipset.poll_interrupt();
            if (vec >= 0) cpu.interrupt(uint8_t(vec));
        }
    }
}

}  // namespace pc486
