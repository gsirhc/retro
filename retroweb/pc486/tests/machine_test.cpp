// GoogleTest suite for the top-level Machine: factory CMOS seeding for
// this machine's own geometry (1.44MB floppy, 504MB HDD, 32MB RAM),
// run_cycles() progress, a full PIT-channel-0 -> PIC IRQ0 -> CPU interrupt
// -> handler round trip (including waking a HLTed CPU), and the keyboard-
// controller reset trick resetting the CPU without corrupting the
// decoupled total_cycles_ pacing counter. Adapted from
// ibmpc-at/tests/machine_test.cpp for the 80486 core's 32-bit-native
// register fields (eax/eip, not ax/ip).

#include <gtest/gtest.h>

#include "machine.h"

#include <cstddef>
#include <vector>

namespace {

using pc486::Machine;

TEST(MachineTest, ConstructorSeedsFactoryCmosConfiguration) {
    Machine m;
    EXPECT_EQ(m.chipset.cmos.peek(0x10), 0x40);  // drive A: 1.44MB 3.5", no B:
    EXPECT_EQ(m.chipset.cmos.peek(0x14) & 0x01, 0x01);  // a floppy is installed
    // Bit 2 of the equipment byte reaches software as bit 2 of the INT 11h
    // equipment word, the standard "PS/2 mouse installed" flag -- and a
    // period mouse driver gates its entire PS/2 path on it: CuteMouse 2.1
    // (FreeDOS's CTMOUSE) opens with INT 11h / TEST AL,4 and gives up with
    // "device not found" if it is clear. This machine has a mouse on the
    // 8042's AUX port, so its CMOS says so. See PC486_REVIEW.md §13.
    EXPECT_EQ(m.chipset.cmos.peek(0x14) & 0x04, 0x04);
    EXPECT_EQ(m.chipset.cmos.peek(0x15), 0x80);  // base memory low byte
    EXPECT_EQ(m.chipset.cmos.peek(0x16), 0x02);  // base memory high byte -> 640KB
    EXPECT_EQ(m.chipset.cmos.peek(0x17), 0x00);  // extended memory low byte
    EXPECT_EQ(m.chipset.cmos.peek(0x18), 0x7C);  // extended memory high byte -> 31744KB
    // 0x30/0x31 is the "POST-verified" copy of the same figure, and is the
    // ONLY place this BIOS actually looks: rombios.c's INT 15h AH=88h and
    // AX=E801 both read 0x30/0x31 and never touch 0x17/0x18. Leaving these
    // zero reported no extended memory at all on a 32MB machine, so HimemX
    // refused to install and the XMS-dependent CD-ROM driver could not load.
    // See PC486_REVIEW.md §5.3.
    EXPECT_EQ(m.chipset.cmos.peek(0x30), 0x00);
    EXPECT_EQ(m.chipset.cmos.peek(0x31), 0x7C);
    EXPECT_EQ(m.chipset.cmos.peek(0x17), m.chipset.cmos.peek(0x30)) << "the two copies must agree";
    EXPECT_EQ(m.chipset.cmos.peek(0x18), m.chipset.cmos.peek(0x31));
    // Memory above the 16MB line, in 64KB units -- the other half of E801's
    // answer, which caps its 0x30/0x31 figure at 15MB. 32MB total means 16MB
    // above 16MB = 16384KB / 64 = 256 = 0x0100.
    EXPECT_EQ(m.chipset.cmos.peek(0x34), 0x00);
    EXPECT_EQ(m.chipset.cmos.peek(0x35), 0x01);
    EXPECT_EQ(m.chipset.cmos.peek(0x3D) & 0x0F, 0x01);         // 1st boot device = floppy
    EXPECT_EQ((m.chipset.cmos.peek(0x3D) >> 4) & 0x0F, 0x02);  // 2nd = hard disk fallback
    EXPECT_EQ(m.chipset.cmos.peek(0x1D), 16);  // fixed-disk heads -- see wd1003.h's 504MB geometry
    // Checksum (0x2E/0x2F) covers 0x10-0x2D and must stay internally
    // consistent with whatever's actually in that range.
    uint16_t sum = 0;
    for (uint16_t reg = 0x10; reg <= 0x2D; ++reg) sum = uint16_t(sum + m.chipset.cmos.peek(uint8_t(reg)));
    EXPECT_EQ(m.chipset.cmos.peek(0x2E), uint8_t(sum >> 8));
    EXPECT_EQ(m.chipset.cmos.peek(0x2F), uint8_t(sum & 0xFF));
}

TEST(MachineTest, FactoryCmosSurvivesAnExplicitResetCall) {
    // Regression test, same as ibmpc-at's: constructing a Machine and
    // calling reset() must not wipe the factory CMOS configuration -- real
    // CMOS is battery-backed and survives any reset.
    Machine m;
    m.reset();
    EXPECT_EQ(m.chipset.cmos.peek(0x3D) & 0x0F, 0x01);
    EXPECT_EQ(m.chipset.cmos.peek(0x10), 0x40);
}

TEST(MachineTest, RunCyclesAdvancesAtLeastTheRequestedAmount) {
    Machine m;
    m.reset();
    for (int i = 0; i < 0x2000; ++i) m.chipset.mem[i] = 0x90;  // NOP sled
    m.cpu.cs = 0;
    m.cpu.eip = 0;
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
    m.cpu.eip = 0;

    for (int i = 0; i < 2000 && mem[0x1234] == 0; ++i) m.run_cycles(10);
    EXPECT_EQ(mem[0x1234], 1);
    EXPECT_FALSE(m.cpu.halted);  // IRET returned to the byte right after HLT
}

TEST(MachineTest, KeyboardControllerResetTrickResetsCpuWithoutLosingPacing) {
    Machine m;
    m.reset();
    for (int i = 0; i < 0x100; ++i) m.chipset.mem[i] = 0x90;  // harmless NOP sled everywhere
    m.cpu.cs = 0;
    m.cpu.eip = 0;
    m.run_cycles(100);
    uint64_t before = m.total_cycles();

    m.chipset.kbc.out(0x64, 0xFE);  // pulse output line 0 -> CPU reset
    m.run_cycles(10);

    // The CPU landed at the real-mode reset vector (F000:FFF0) and only
    // ran forward from there -- it did NOT keep executing from wherever it
    // was in the NOP sled before the reset.
    EXPECT_EQ(m.cpu.cs, 0xF000);
    EXPECT_GE(m.cpu.eip, 0xFFF0u);
    EXPECT_GE(m.total_cycles(), before);  // pacing counter kept advancing, not zeroed by the reset
}

// --- the CPU's page-resolution and prefetch fast paths (§15) -------------
// The CPU now resolves a physical page to a host pointer once and reads and
// writes every byte of it directly, and fetches a run of instruction bytes
// out of the code page the same way. These run real guest code through a
// real Machine to pin the properties that must survive that.

namespace {
// Loads `code` at 0000:0400 and runs until the guest halts (or the budget
// runs out). Interrupts are off out of reset, so nothing else executes.
void RunRealMode(Machine &m, const std::vector<uint8_t> &code) {
    m.reset();
    for (std::size_t i = 0; i < code.size(); ++i) m.chipset.mem[0x400 + i] = code[i];
    m.cpu.cs = 0;
    m.cpu.ds = 0;
    m.cpu.eip = 0x400;
    for (int i = 0; i < 200 && !m.cpu.halted; ++i) m.run_cycles(100);
}
}  // namespace

TEST(MachineTest, CpuWritesStillCannotAlterRomThroughTheResolvedPage) {
    Machine m;
    uint8_t rom[2] = {0x11, 0x22};
    m.chipset.load_rom(0xF0000, rom, 2);
    // MOV AX,F000 / MOV DS,AX / MOV BYTE [0],FF / MOV BYTE [1],FE / HLT.
    // The second store is the one that matters: by then the page has been
    // resolved once already, so it is the cached answer being trusted.
    RunRealMode(m, {0xB8, 0x00, 0xF0, 0x8E, 0xD8,
                    0xC6, 0x06, 0x00, 0x00, 0xFF,
                    0xC6, 0x06, 0x01, 0x00, 0xFE, 0xF4});
    EXPECT_EQ(m.chipset.mem[0xF0000], 0x11);
    EXPECT_EQ(m.chipset.mem[0xF0001], 0x22);
}

TEST(MachineTest, CpuAccessesInTheVgaWindowStillGoToTheCardAndNotRam) {
    Machine m;
    m.reset();
    m.chipset.mem[0xA0000] = 0x99;  // marker in the RAM sitting behind the window
    // MOV AX,A000 / MOV DS,AX / MOV BYTE [0],5A / MOV AL,[0] / XOR BX,BX /
    // MOV ES,BX / MOV ES:[0300],AL / HLT. The store must not reach RAM and
    // the load must come back from the card, not from the marker.
    const std::vector<uint8_t> code = {0xB8, 0x00, 0xA0, 0x8E, 0xD8,
                                       0xC6, 0x06, 0x00, 0x00, 0x5A,
                                       0xA0, 0x00, 0x00,
                                       0x31, 0xDB, 0x8E, 0xC3,
                                       0x26, 0xA2, 0x00, 0x03, 0xF4};
    for (std::size_t i = 0; i < code.size(); ++i) m.chipset.mem[0x400 + i] = code[i];
    m.cpu.cs = 0;
    m.cpu.ds = 0;
    m.cpu.eip = 0x400;
    for (int i = 0; i < 200 && !m.cpu.halted; ++i) m.run_cycles(100);

    EXPECT_EQ(m.chipset.mem[0xA0000], 0x99);  // never touched the RAM behind the window
    EXPECT_EQ(m.chipset.mem[0x300], m.chipset.vga.mem_read(0xA0000));
    EXPECT_NE(m.chipset.mem[0x300], 0x99);
}

TEST(MachineTest, OpeningA20MidRunRetargetsAnAlreadyResolvedPage) {
    Machine m;
    // MOV AX,FFFF / MOV DS,AX / MOV BYTE [0110],AA   -- gate closed, so this
    // aliases down to physical 000100 -- then the 8042 dance that opens A20,
    // then the same store again, which must now land at 100100.
    RunRealMode(m, {0xB8, 0xFF, 0xFF, 0x8E, 0xD8,
                    0xC6, 0x06, 0x10, 0x01, 0xAA,
                    0xB0, 0xD1, 0xE6, 0x64,   // MOV AL,D1 / OUT 64,AL
                    0xB0, 0xDF, 0xE6, 0x60,   // MOV AL,DF / OUT 60,AL -- A20 on,
                                              // reset line left high (bit 0 low resets the CPU)
                    0xC6, 0x06, 0x10, 0x01, 0xBB, 0xF4});
    ASSERT_TRUE(m.chipset.kbc.a20_enabled());
    EXPECT_EQ(m.chipset.mem[0x000100], 0xAA);
    EXPECT_EQ(m.chipset.mem[0x100100], 0xBB);
}

TEST(MachineTest, CodeThatPatchesItselfAheadOfEipExecutesThePatchedByte) {
    Machine m;
    // MOV BYTE [040A],40 (patch the NOP at 040A into INC AX) / XOR AX,AX /
    // three NOPs / the patched byte / HLT. The store and the fetch are on
    // the same page, so this is the prefetch window reading live memory.
    RunRealMode(m, {0xC6, 0x06, 0x0A, 0x04, 0x40,
                    0x31, 0xC0,
                    0x90, 0x90, 0x90,
                    0x90,
                    0xF4});
    EXPECT_EQ(m.cpu.eax & 0xFFFFu, 1u);
}

}  // namespace
