#include <gtest/gtest.h>

#include "machine.h"

#include <vector>

namespace {

using ibmpcat::Machine;

TEST(MachineTest, ConstructorSeedsFactoryCmosConfiguration) {
    // Without the factory CMOS bytes the BIOS panics "No bootable device" (IBM_PCAT_REVIEW.md §8).
    Machine m;
    EXPECT_EQ(m.chipset.cmos.peek(0x10), 0x21);  // drive A: 1.2MB, B: 360KB
    EXPECT_EQ(m.chipset.cmos.peek(0x14), 0x41);  // two diskette drives, EGA
    EXPECT_EQ(m.chipset.cmos.peek(0x15), 0x80);  // base memory low byte
    EXPECT_EQ(m.chipset.cmos.peek(0x16), 0x02);  // base memory high byte -> 640KB
    EXPECT_EQ(m.chipset.cmos.peek(0x3D) & 0x0F, 0x01);         // 1st boot device = floppy
    EXPECT_EQ((m.chipset.cmos.peek(0x3D) >> 4) & 0x0F, 0x02);  // 2nd = hard disk fallback
    // Checksum 0x2E/0x2F covers 0x10-0x2D.
    uint16_t sum = 0;
    for (uint16_t reg = 0x10; reg <= 0x2D; ++reg) sum = uint16_t(sum + m.chipset.cmos.peek(uint8_t(reg)));
    EXPECT_EQ(m.chipset.cmos.peek(0x2E), uint8_t(sum >> 8));
    EXPECT_EQ(m.chipset.cmos.peek(0x2F), uint8_t(sum & 0xFF));
}

TEST(MachineTest, FactoryCmosSurvivesAnExplicitResetCall) {
    // CMOS is battery-backed and must survive reset (IBM_PCAT_REVIEW.md §8).
    Machine m;
    m.reset();
    EXPECT_EQ(m.chipset.cmos.peek(0x3D) & 0x0F, 0x01);
    EXPECT_EQ(m.chipset.cmos.peek(0x10), 0x21);
}

TEST(MachineTest, OnlyTheSystemRomGetsThe386Concession) {
    Machine m;
    std::vector<uint8_t> rom(0x4000, 0x90);
    m.chipset.load_rom(0xC0000, rom.data(), rom.size());
    m.chipset.load_rom(0xFC000, rom.data(), rom.size());
    EXPECT_FALSE(m.cpu.firmware_at(0xC0003));
    EXPECT_TRUE(m.cpu.firmware_at(0xFC003));
    EXPECT_TRUE(m.cpu.firmware_at(0xFFC003));
    EXPECT_FALSE(m.cpu.firmware_at(0x7C00));
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
    std::vector<uint8_t> rom(16, 0x90);
    m.chipset.load_rom(0xFFFF0, rom.data(), rom.size());
    m.cpu.cs = 0;
    m.cpu.ip = 0;
    m.run_cycles(100);
    uint64_t before = m.total_cycles();

    m.chipset.kbc.out(0x64, 0xFE);  // pulse output line 0 -> CPU reset
    m.run_cycles(10);

    // Lands at the reset vector F000:FFF0, not in the NOP sled.
    EXPECT_EQ(m.cpu.cs, 0xF000);
    EXPECT_GE(m.cpu.ip, 0xFFF0);
    EXPECT_GE(m.total_cycles(), before);  // pacing counter kept advancing, not zeroed by the reset
}

TEST(MachineTest, FirstFetchIsFromTheTopOf16MBUntilAFarJump) {
    Machine m;
    m.reset();
    const uint8_t stub[] = {0xEA, 0x00, 0xE0, 0x00, 0xF0};  // JMP F000:E000
    m.chipset.load_rom(0xFFFF0, stub, sizeof stub);
    const uint8_t target[] = {0x90};
    m.chipset.load_rom(0xFE000, target, sizeof target);
    m.chipset.kbc.set_a20(false);  // the low 1MB copy would still answer, the alias wouldn't
    m.cpu.reset();
    m.run_cycles(1);
    EXPECT_NE(m.cpu.ip, 0xE000) << "with A20 held low the fetch at FFFFF0h finds no ROM";

    m.chipset.kbc.set_a20(true);
    m.cpu.reset();
    m.run_cycles(1);
    EXPECT_EQ(m.cpu.cs, 0xF000);
    EXPECT_EQ(m.cpu.ip, 0xE000);
    m.chipset.mem[0xFE000 + 1] = 0x90;
    m.run_cycles(1);
    EXPECT_EQ(m.cpu.ip, 0xE001) << "after the far jump CS's base is F0000h again";
}

TEST(MachineTest, A20ClosesWhenRomCodeEntersABootSector) {
    Machine m;
    m.reset();
    ASSERT_TRUE(m.chipset.kbc.a20_enabled());
    auto &mem = m.chipset.mem;
    mem[0x0600] = 0xEA; mem[0x0601] = 0x00; mem[0x0602] = 0x7C; mem[0x0603] = 0x00; mem[0x0604] = 0x00;
    m.cpu.cs = 0; m.cpu.ip = 0x0600;
    m.run_cycles(1);  // JMP 0000:7C00 from RAM, as an MBR handing to a VBR does
    EXPECT_EQ(m.cpu.ip, 0x7C00);
    EXPECT_TRUE(m.chipset.kbc.a20_enabled());

    const uint8_t jmp[] = {0xEA, 0x00, 0x7C, 0x00, 0x00};
    m.chipset.load_rom(0xF8000, jmp, sizeof jmp);
    m.cpu.cs = 0xF800; m.cpu.ip = 0;
    m.run_cycles(1);
    EXPECT_EQ(m.cpu.ip, 0x7C00);
    EXPECT_FALSE(m.chipset.kbc.a20_enabled()) << "IBM's POST closes A20 before booting";
}

TEST(MachineTest, Opcodes386RunFromRomButNotFromRam) {
    Machine m;
    m.reset();
    const uint8_t movzx[] = {0x0F, 0xB6, 0xC3, 0xF4};  // MOVZX AX, BL ; HLT
    m.chipset.load_rom(0xE0000, movzx, sizeof movzx);
    auto &mem = m.chipset.mem;
    for (int i = 0; i < 4; ++i) mem[0x0600 + i] = movzx[i];
    mem[6 * 4 + 0] = 0x00; mem[6 * 4 + 1] = 0x50;  // IVT[6] -> 0000:5000
    mem[6 * 4 + 2] = 0x00; mem[6 * 4 + 3] = 0x00;
    m.cpu.ss = 0; m.cpu.sp = 0x8000;
    m.cpu.bx = 0x80;

    m.cpu.cs = 0xE000; m.cpu.ip = 0;
    m.run_cycles(1);
    EXPECT_EQ(m.cpu.ax & 0xFFFF, 0x80u);
    EXPECT_EQ(m.cpu.ip, 3);

    m.cpu.ax = 0;
    m.cpu.cs = 0; m.cpu.ip = 0x0600;
    m.run_cycles(1);
    EXPECT_EQ(m.cpu.ax, 0u);
    EXPECT_EQ(m.cpu.ip, 0x5000) << "a 286 raises #UD outside the firmware";
}

TEST(MachineTest, MovSsHoldsOffAPendingIrqForOneInstruction) {
    Machine m;
    m.reset();
    m.chipset.pic_master.out(0x20, 0x11);
    m.chipset.pic_master.out(0x21, 0x08);
    m.chipset.pic_master.out(0x21, 0x04);
    m.chipset.pic_master.out(0x21, 0x01);
    m.chipset.pic_master.out(0x21, 0xFD);  // unmask IRQ1 only

    auto &mem = m.chipset.mem;
    mem[9 * 4 + 0] = 0x00; mem[9 * 4 + 1] = 0x50;  // IVT[9] -> 0000:5000
    mem[9 * 4 + 2] = 0x00; mem[9 * 4 + 3] = 0x00;
    mem[0x0600] = 0x8E; mem[0x0601] = 0xD0;  // MOV SS, AX
    mem[0x0602] = 0x90;                      // NOP
    m.cpu.cs = 0; m.cpu.ip = 0x0600;
    m.cpu.ax = 0; m.cpu.sp = 0x8000;
    m.cpu.set_flag(cpu80286::FLAG_IF, true);
    m.chipset.kbc.out(0x64, 0x60); m.chipset.kbc.out(0x60, 0x01);  // enable IRQ1
    m.chipset.kbc.inject_scancode(0x1E);

    m.run_cycles(1);
    EXPECT_EQ(m.cpu.ip, 0x0602) << "no INTR between MOV SS and the next instruction";
    m.run_cycles(1);
    EXPECT_EQ(m.cpu.ip, 0x5000);
}

TEST(MachineTest, LongRepYieldsSoTheTimerStillTicks) {
    Machine m;
    m.reset();
    auto &mem = m.chipset.mem;
    mem[0x0600] = 0xF3; mem[0x0601] = 0xAB;  // REP STOSW
    m.cpu.cs = 0; m.cpu.ip = 0x0600;
    m.cpu.es = 0x2000; m.cpu.di = 0;
    m.cpu.cx = 0x8000;
    m.run_cycles(1);
    EXPECT_EQ(m.cpu.ip, 0x0600);
    EXPECT_GT(m.cpu.cx, 0x7F00u) << "one PIT clock's worth of iterations, not the whole 64KB";
}

}  // namespace
