// GoogleTest suite for the top-level Machine: run_cycles() progress, a full
// PIT-channel-0 -> PIC IRQ0 -> CPU interrupt -> handler round trip
// (including waking a HLTed CPU), and the keyboard-controller reset trick
// resetting the CPU without corrupting the decoupled total_cycles_ pacing
// counter -- the exact class of bug cg-oac-6502's review doc (§10)
// documents for its own Machine.

#include <gtest/gtest.h>

#include "machine.h"

namespace {

using ibmpcat::Machine;

TEST(MachineTest, ConstructorSeedsFactoryCmosConfiguration) {
    // See IBM_PCAT_REVIEW.md §8: without these bytes, the real BIOS this
    // machine boots panics with "No bootable device" regardless of what's
    // mounted -- it reads the boot sequence from CMOS 0x3D rather than
    // auto-probing drives.
    Machine m;
    EXPECT_EQ(m.chipset.cmos.peek(0x10), 0x21);  // drive A: 1.2MB, B: 360KB
    EXPECT_EQ(m.chipset.cmos.peek(0x14) & 0x01, 0x01);  // a floppy is installed
    EXPECT_EQ(m.chipset.cmos.peek(0x15), 0x80);  // base memory low byte
    EXPECT_EQ(m.chipset.cmos.peek(0x16), 0x02);  // base memory high byte -> 640KB
    EXPECT_EQ(m.chipset.cmos.peek(0x3D) & 0x0F, 0x01);         // 1st boot device = floppy
    EXPECT_EQ((m.chipset.cmos.peek(0x3D) >> 4) & 0x0F, 0x02);  // 2nd = hard disk fallback
    // Checksum (0x2E/0x2F) covers 0x10-0x2D and must stay internally
    // consistent with whatever's actually in that range.
    uint16_t sum = 0;
    for (uint16_t reg = 0x10; reg <= 0x2D; ++reg) sum = uint16_t(sum + m.chipset.cmos.peek(uint8_t(reg)));
    EXPECT_EQ(m.chipset.cmos.peek(0x2E), uint8_t(sum >> 8));
    EXPECT_EQ(m.chipset.cmos.peek(0x2F), uint8_t(sum & 0xFF));
}

TEST(MachineTest, FactoryCmosSurvivesAnExplicitResetCall) {
    // Regression test: chipset.reset() used to unconditionally reset the
    // CMOS/RTC too, silently wiping the factory configuration the moment
    // any caller did the natural thing and called m.reset() after
    // constructing a Machine -- real CMOS is battery-backed and survives
    // any reset. See IBM_PCAT_REVIEW.md §8.
    Machine m;
    m.reset();
    EXPECT_EQ(m.chipset.cmos.peek(0x3D) & 0x0F, 0x01);
    EXPECT_EQ(m.chipset.cmos.peek(0x10), 0x21);
}

TEST(MachineTest, RunCyclesAdvancesAtLeastTheRequestedAmount) {
    Machine m;
    m.reset();
    for (int i = 0; i < 0x2000; ++i) m.chipset.mem[i] = 0x90;  // NOP sled
    m.cpu.cs = 0;
    m.cpu.ip = 0;
    uint64_t before = m.total_cycles();
    m.run_cycles(1000);
    EXPECT_GE(m.total_cycles(), before + 1000);
}

TEST(MachineTest, TimerInterruptWakesHaltedCpuAndRunsHandler) {
    Machine m;
    m.reset();

    // Master PIC: vector base 0x08, cascaded, 8086 mode, unmask IRQ0 only.
    m.chipset.pic_master.out(0x20, 0x11);
    m.chipset.pic_master.out(0x21, 0x08);
    m.chipset.pic_master.out(0x21, 0x04);
    m.chipset.pic_master.out(0x21, 0x01);
    m.chipset.pic_master.out(0x21, 0xFE);  // unmask IRQ0 only

    // PIT channel 0, mode 3, a small reload so it fires almost immediately.
    m.chipset.pit.out(0x43, 0x36);
    m.chipset.pit.out(0x40, 4);
    m.chipset.pit.out(0x40, 0);

    auto &mem = m.chipset.mem;
    mem[8 * 4 + 0] = 0x00; mem[8 * 4 + 1] = 0x50;  // IVT[8] -> 0000:5000 (physical 0x5000)
    mem[8 * 4 + 2] = 0x00; mem[8 * 4 + 3] = 0x00;
    // Handler: INC byte ptr [1234h] ; IRET
    mem[0x5000 + 0] = 0xFE; mem[0x5000 + 1] = 0x06;
    mem[0x5000 + 2] = 0x34; mem[0x5000 + 3] = 0x12;
    mem[0x5000 + 4] = 0xCF;
    mem[0x1234] = 0;

    // Main program at CS:IP = 0:0 -- STI ; HLT.
    mem[0] = 0xFB;
    mem[1] = 0xF4;
    m.cpu.cs = 0;
    m.cpu.ip = 0;

    for (int i = 0; i < 2000 && mem[0x1234] == 0; ++i) m.run_cycles(10);
    EXPECT_EQ(mem[0x1234], 1);
    EXPECT_FALSE(m.cpu.halted);  // IRET returned to the byte right after HLT
}

TEST(MachineTest, KeyboardControllerResetTrickResetsCpuWithoutLosingPacing) {
    Machine m;
    m.reset();
    for (int i = 0; i < 0x100; ++i) m.chipset.mem[i] = 0x90;  // harmless NOP sled everywhere
    m.cpu.cs = 0;
    m.cpu.ip = 0;
    m.run_cycles(100);
    uint64_t before = m.total_cycles();

    m.chipset.kbc.out(0x64, 0xFE);  // pulse output line 0 -> CPU reset
    m.run_cycles(10);

    // The CPU landed at the real-mode reset vector (F000:FFF0) and only
    // ran forward from there -- it did NOT keep executing from wherever it
    // was in the NOP sled before the reset.
    EXPECT_EQ(m.cpu.cs, 0xF000);
    EXPECT_GE(m.cpu.ip, 0xFFF0);
    EXPECT_GE(m.total_cycles(), before);  // pacing counter kept advancing, not zeroed by the reset
}

}  // namespace
