#include "machine.h"

namespace ibmpcat {

void Machine::configure_factory_cmos() {
    auto &c = chipset.cmos;
    // Floppy drive types (0x10): high nibble = drive A:, low nibble =
    // drive B:. 1=360KB 5.25", 2=1.2MB 5.25" -- this system's two drives.
    c.poke(0x10, 0x21);
    // Equipment byte (0x14): bit0 = at least one floppy drive installed.
    // The floppy-count (bits 6-7) and video-type (bits 4-5) sub-fields
    // aren't asserted here -- not empirically verified against this
    // specific BIOS build yet, see IBM_PCAT_REVIEW.md §8.
    c.poke(0x14, 0x01);
    // Base memory size, 640KB, little-endian word (0x15 low, 0x16 high) --
    // matches the real POST "640 KB OK" message this system's spec calls for.
    c.poke(0x15, 0x80);
    c.poke(0x16, 0x02);
    // Boot device sequence (BX_ELTORITO_BOOT convention, the actual BIOS
    // source read to find this): low nibble of 0x3D selects the 1st boot
    // device, high nibble the 2nd -- 0x01=floppy, 0x02=hard disk, per
    // rombios.c's own boot-device-code table. Without a 1st device the
    // BIOS panics with "No bootable device" regardless of what's actually
    // mounted -- it never auto-probes drives for bootability (see
    // IBM_PCAT_REVIEW.md §8). 0x21 (floppy first, falling back to the
    // fixed disk) is this BIOS's literal spelling of the standard real-AT
    // default sequence (A: then C:) for a machine that has a hard disk
    // installed -- confirmed missing when a genuine HDD-only boot (no
    // floppy present) hit that exact panic instead of falling through.
    // See IBM_PCAT_REVIEW.md.
    c.poke(0x3D, 0x21);

    // Fixed-disk "Type 47 user-definable" geometry (0x12, 0x19, 0x1B-0x23):
    // this BIOS's hard_drive_post reads these into a legacy EBDA parameter
    // table completely separately from ata_detect()'s IDENTIFY-based path
    // (wd1003.h) -- both describe the identical ST-4038 geometry (733
    // cyl / 5 heads / 17 sec/track, no write precomp, landing zone 733),
    // so the two paths agree. Found by reading hard_drive_post itself:
    // 0x12 high nibble must be 0xF ("use extended type") or this whole
    // block is skipped; 0x19 must then read exactly 47 or POST halts.
    // See IBM_PCAT_REVIEW.md.
    c.poke(0x12, 0xF0);  // drive C: = extended type; no drive D:
    c.poke(0x19, 47);
    c.poke(0x1B, 0xDD);  // cylinders low  (733 = 0x2DD)
    c.poke(0x1C, 0x02);  // cylinders high
    c.poke(0x1D, 5);     // heads
    c.poke(0x1E, 0xFF);  // write precomp low  -- 0xFFFF = "none", this drive doesn't need it
    c.poke(0x1F, 0xFF);  // write precomp high
    c.poke(0x20, 0x00);  // control byte (heads <= 8, no special flags)
    c.poke(0x21, 0xDD);  // landing zone low  (733, same as max cylinder)
    c.poke(0x22, 0x02);  // landing zone high
    c.poke(0x23, 17);    // sectors per track

    // CMOS checksum over bytes 0x10-0x2D, stored big-endian at 0x2E/0x2F --
    // kept internally consistent even though this BIOS build hasn't been
    // observed to actually enforce it.
    uint16_t sum = 0;
    for (uint16_t reg = 0x10; reg <= 0x2D; ++reg) sum = uint16_t(sum + c.peek(uint8_t(reg)));
    c.poke(0x2E, uint8_t(sum >> 8));
    c.poke(0x2F, uint8_t(sum & 0xFF));
}

void Machine::run_cycles(int64_t cycles) {
    int64_t target = int64_t(total_cycles_) + cycles;
    while (int64_t(total_cycles_) < target) {
        if (chipset.kbc.reset_requested()) {
            chipset.kbc.clear_reset_request();
            cpu.reset();
        }
        int spent = cpu.step();
        total_cycles_ += uint64_t(spent);
        chipset.tick(total_cycles_, kCpuHz);
        // Real hardware only begins an INTA cycle if the CPU's IF flag
        // permits it to respond to INTR -- polling (and thus acknowledging)
        // the PIC while IF is clear would wrongly consume a pending IRQ the
        // CPU never actually served, e.g. losing timer ticks during a
        // cli-protected critical section.
        if (cpu.flag(cpu80286::FLAG_IF)) {
            int vec = chipset.poll_interrupt();
            if (vec >= 0) cpu.interrupt(uint8_t(vec));
        }
    }
}

}  // namespace ibmpcat
