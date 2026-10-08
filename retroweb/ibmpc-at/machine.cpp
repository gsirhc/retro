#include "machine.h"

namespace ibmpcat {

void Machine::configure_factory_cmos() {
    auto &c = chipset.cmos;
    // Floppy types (0x10): high nibble A:, low nibble B:. 1=360KB, 2=1.2MB.
    c.poke(0x10, 0x21);
    // Equipment byte (0x14): bit0 = floppy installed. Other sub-fields are unset.
    c.poke(0x14, 0x01);
    // Base memory 640KB, word at 0x15/0x16.
    c.poke(0x15, 0x80);
    c.poke(0x16, 0x02);
    // Boot sequence (BX_ELTORITO_BOOT style, rombios.c): low nibble of 0x3D is
    // the 1st device, high nibble the 2nd; 1=floppy, 2=hard disk. The BIOS panics
    // "No bootable device" without a 1st device. 0x21 = A: then C:
    // (IBM_PCAT_REVIEW.md §8).
    c.poke(0x3D, 0x21);

    // Fixed-disk Type 47 geometry (0x12, 0x19, 0x1B-0x23), read by hard_drive_post
    // separately from ata_detect() (wd1003.h), same ST-4038 geometry. 0x12 high
    // nibble must be 0xF and 0x19 must be 47, or POST skips the block or halts.
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

    // CMOS checksum over 0x10-0x2D, big-endian at 0x2E/0x2F.
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
        uint32_t from = (uint32_t(cpu.cs) << 4) + cpu.ip;
        int spent = cpu.step();
        // IBM's POST closes A20 before booting; the stand-in BIOS never does, so
        // the machine closes it when ROM code enters a boot sector at 0000:7C00.
        if ((uint32_t(cpu.cs) << 4) + cpu.ip == 0x7C00 && chipset.is_rom(from)) chipset.kbc.set_a20(false);
        total_cycles_ += uint64_t(spent);
        chipset.tick(total_cycles_, kCpuHz);
        total_cycles_ += uint64_t(chipset.take_held_clocks());
        // An INTA cycle only starts when IF is set; polling earlier would consume IRQs the CPU never served.
        if (cpu.flag(cpu80286::FLAG_IF) && !cpu.interrupt_shadow()) {
            int vec = chipset.poll_interrupt();
            if (vec >= 0) total_cycles_ += uint64_t(cpu.hardware_interrupt(uint8_t(vec)));
        }
    }
}

}  // namespace ibmpcat
