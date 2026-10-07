// Machine: factory CMOS, run_cycles, PIT -> PIC -> CPU interrupt round trip, KBC reset

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
    // INT 11h equipment bit 2 (PS/2 mouse); CuteMouse gives up if clear
    EXPECT_EQ(m.chipset.cmos.peek(0x14) & 0x04, 0x04);
    EXPECT_EQ(m.chipset.cmos.peek(0x15), 0x80);  // base memory low byte
    EXPECT_EQ(m.chipset.cmos.peek(0x16), 0x02);  // base memory high byte -> 640KB
    EXPECT_EQ(m.chipset.cmos.peek(0x17), 0x00);  // extended memory low byte
    EXPECT_EQ(m.chipset.cmos.peek(0x18), 0x7C);  // extended memory high byte -> 31744KB
    // 0x30/0x31 is the only extended-memory figure rombios.c reads (INT 15h AH=88h, E801)
    EXPECT_EQ(m.chipset.cmos.peek(0x30), 0x00);
    EXPECT_EQ(m.chipset.cmos.peek(0x31), 0x7C);
    EXPECT_EQ(m.chipset.cmos.peek(0x17), m.chipset.cmos.peek(0x30)) << "the two copies must agree";
    EXPECT_EQ(m.chipset.cmos.peek(0x18), m.chipset.cmos.peek(0x31));
    // memory above 16MB in 64KB units: 16384KB / 64 = 0x0100
    EXPECT_EQ(m.chipset.cmos.peek(0x34), 0x00);
    EXPECT_EQ(m.chipset.cmos.peek(0x35), 0x01);
    EXPECT_EQ(m.chipset.cmos.peek(0x3D) & 0x0F, 0x01);         // 1st boot device = floppy
    EXPECT_EQ((m.chipset.cmos.peek(0x3D) >> 4) & 0x0F, 0x02);  // 2nd = hard disk fallback
    // Type 47 fixed-disk geometry: the WD Caviar AC2250's 1010/9/55.
    EXPECT_EQ(m.chipset.cmos.peek(0x19), 47);
    EXPECT_EQ(m.chipset.cmos.peek(0x1B) | (m.chipset.cmos.peek(0x1C) << 8), 1010);
    EXPECT_EQ(m.chipset.cmos.peek(0x1D), 9);
    EXPECT_EQ(m.chipset.cmos.peek(0x23), 55);
    // checksum covers 0x10-0x2D
    uint16_t sum = 0;
    for (uint16_t reg = 0x10; reg <= 0x2D; ++reg) sum = uint16_t(sum + m.chipset.cmos.peek(uint8_t(reg)));
    EXPECT_EQ(m.chipset.cmos.peek(0x2E), uint8_t(sum >> 8));
    EXPECT_EQ(m.chipset.cmos.peek(0x2F), uint8_t(sum & 0xFF));
}

TEST(MachineTest, FactoryCmosSurvivesAnExplicitResetCall) {
    // CMOS is battery-backed and survives reset
    Machine m;
    m.reset();
    EXPECT_EQ(m.chipset.cmos.peek(0x3D) & 0x0F, 0x01);
    EXPECT_EQ(m.chipset.cmos.peek(0x10), 0x40);
}

TEST(MachineTest, RunCyclesAdvancesAtLeastTheRequestedAmount) {
    Machine m;
    m.reset();
    for (int i = 0; i < 0x2000; ++i) m.chipset.mem[i] = 0x90;
    m.cpu.cs = 0;
    m.cpu.eip = 0;
    uint64_t before = m.total_cycles();
    m.run_cycles(1000);
    EXPECT_GE(m.total_cycles(), before + 1000);
}

TEST(MachineTest, TimerInterruptWakesHaltedCpuAndRunsHandler) {
    Machine m;
    m.reset();

    // master PIC: base 0x08, cascaded, 8086 mode
    m.chipset.pic_master.out(0x20, 0x11);
    m.chipset.pic_master.out(0x21, 0x08);
    m.chipset.pic_master.out(0x21, 0x04);
    m.chipset.pic_master.out(0x21, 0x01);
    m.chipset.pic_master.out(0x21, 0xFE);

    // PIT ch0, mode 3, small reload
    m.chipset.pit.out(0x43, 0x36);
    m.chipset.pit.out(0x40, 4);
    m.chipset.pit.out(0x40, 0);

    auto &mem = m.chipset.mem;
    mem[8 * 4 + 0] = 0x00; mem[8 * 4 + 1] = 0x50;
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

    // CPU restarts at the reset vector F000:FFF0
    EXPECT_EQ(m.cpu.cs, 0xF000);
    EXPECT_GE(m.cpu.eip, 0xFFF0u);
    EXPECT_GE(m.total_cycles(), before);  // pacing counter not zeroed by reset
}

TEST(MachineTest, AnInterruptPendingAtStiWaitsOneInstruction) {
    Machine m;
    m.reset();
    m.chipset.pic_master.out(0x20, 0x11);
    m.chipset.pic_master.out(0x21, 0x08);
    m.chipset.pic_master.out(0x21, 0x04);
    m.chipset.pic_master.out(0x21, 0x01);
    m.chipset.pic_master.out(0x21, 0xFD);  // unmask IRQ1 only
    m.chipset.pic_master.raise(1);         // already pending when STI runs

    auto &mem = m.chipset.mem;
    mem[9 * 4 + 0] = 0x00; mem[9 * 4 + 1] = 0x50;  // IVT[9] -> 0000:5000
    mem[9 * 4 + 2] = 0x00; mem[9 * 4 + 3] = 0x00;
    // Handler: MOV AL,[1235h] ; MOV [1236h],AL ; MOV byte [1234h],1 ; IRET
    const uint8_t handler[] = {0xA0, 0x35, 0x12, 0xA2, 0x36, 0x12, 0xC6, 0x06, 0x34, 0x12, 0x01, 0xCF};
    for (std::size_t i = 0; i < sizeof handler; ++i) mem[0x5000 + i] = handler[i];
    // STI ; INC byte [1235h] ; HLT
    const uint8_t prog[] = {0xFB, 0xFE, 0x06, 0x35, 0x12, 0xF4};
    for (std::size_t i = 0; i < sizeof prog; ++i) mem[i] = prog[i];
    mem[0x1234] = mem[0x1235] = mem[0x1236] = 0;
    m.cpu.cs = m.cpu.ds = 0;
    m.cpu.eip = 0;

    for (int i = 0; i < 200 && mem[0x1234] == 0; ++i) m.run_cycles(10);
    ASSERT_EQ(mem[0x1234], 1);
    EXPECT_EQ(mem[0x1236], 1) << "the instruction after STI ran before the interrupt";
}

TEST(MachineTest, ShutdownResetsTheCpuAndKeepsMemory) {
    Machine m;
    m.reset();
    auto &mem = m.chipset.mem;
    // LIDT [0600h] with a zero limit, then INT3: #GP, #DF, shutdown.
    const uint8_t prog[] = {0x0F, 0x01, 0x1E, 0x00, 0x06, 0xCC};
    for (std::size_t i = 0; i < sizeof prog; ++i) mem[0x400 + i] = prog[i];
    for (int i = 0; i < 6; ++i) mem[0x600 + i] = 0;
    mem[0x1234] = 0x5A;
    m.cpu.cs = m.cpu.ds = 0;
    m.cpu.eip = 0x400;
    m.run_cycles(200);
    EXPECT_EQ(m.cpu.cs, 0xF000) << "the board turned the shutdown cycle into RESET";
    EXPECT_FALSE(m.cpu.shutdown());
    EXPECT_EQ(mem[0x1234], 0x5A) << "a CPU reset, not a power cycle";
}

// --- CPU page-resolution and prefetch fast paths ---

namespace {
// loads code at 0000:0400, runs until HLT or budget
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
    // the second store trusts the cached page resolution
    RunRealMode(m, {0xB8, 0x00, 0xF0, 0x8E, 0xD8,
                    0xC6, 0x06, 0x00, 0x00, 0xFF,
                    0xC6, 0x06, 0x01, 0x00, 0xFE, 0xF4});
    EXPECT_EQ(m.chipset.mem[0xF0000], 0x11);
    EXPECT_EQ(m.chipset.mem[0xF0001], 0x22);
}

TEST(MachineTest, CpuAccessesInTheVgaWindowStillGoToTheCardAndNotRam) {
    Machine m;
    m.reset();
    m.chipset.mem[0xA0000] = 0x99;
    // store must not reach RAM, load must come from the card
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
    // gate closed: store aliases to 000100; after the 8042 A20 sequence it lands at 100100
    RunRealMode(m, {0xB8, 0xFF, 0xFF, 0x8E, 0xD8,
                    0xC6, 0x06, 0x10, 0x01, 0xAA,
                    0xB0, 0xD1, 0xE6, 0x64,   // MOV AL,D1 / OUT 64,AL
                    0xB0, 0xDF, 0xE6, 0x60,  // A20 on, reset line high
                    0xC6, 0x06, 0x10, 0x01, 0xBB, 0xF4});
    ASSERT_TRUE(m.chipset.kbc.a20_enabled());
    EXPECT_EQ(m.chipset.mem[0x000100], 0xAA);
    EXPECT_EQ(m.chipset.mem[0x100100], 0xBB);
}

TEST(MachineTest, CodeThatPatchesItselfAheadOfEipExecutesThePatchedByte) {
    Machine m;
    // store and fetch share a page: prefetch must see live memory
    RunRealMode(m, {0xC6, 0x06, 0x0A, 0x04, 0x40,
                    0x31, 0xC0,
                    0x90, 0x90, 0x90,
                    0x90,
                    0xF4});
    EXPECT_EQ(m.cpu.eax & 0xFFFFu, 1u);
}

TEST(MachineTest, TurboOffHoldsTheBusAndLeavesTheClockAlone) {
    // 471 de-turbo: periodic HOLD, 4us of every 12us; PIT pacing unaffected
    Machine m;
    EXPECT_TRUE(m.turbo());
    m.set_turbo(false);
    EXPECT_FALSE(m.turbo());
    EXPECT_DOUBLE_EQ(m.cpu_hz(), Machine::kCpuHz);
    EXPECT_EQ(m.cpu.rep_yield_cycles, 55u);

    // cold read at hold start waits out the 264-clock hold
    m.cache.reset();
    EXPECT_EQ(m.cache.read(0x40000, 4, true, 792 * 10), 264 + 2 * (4 + 5));
    m.set_turbo(true);
    m.cache.reset();
    EXPECT_EQ(m.cache.read(0x40000, 4, true, 792 * 10), 2 * (4 + 5));
}

// --- cache and bus timing (cache486.h) ---

TEST(MachineTest, TheBoardTurnsTheL1OnAfterEveryReset) {
    // CPU leaves RESET with CR0.CD and NW set; the board stands in for POST clearing them
    constexpr uint32_t kCdNw = 0x60000000u;
    Machine m;
    EXPECT_EQ(m.cpu.cr(0) & kCdNw, 0u);
    m.reset();
    EXPECT_EQ(m.cpu.cr(0) & kCdNw, 0u);
    // shutdown reset: LIDT zero limit, INT3
    const uint8_t prog[] = {0x0F, 0x01, 0x1E, 0x00, 0x06, 0xCC};
    for (std::size_t i = 0; i < sizeof prog; ++i) m.chipset.mem[0x400 + i] = prog[i];
    for (int i = 0; i < 6; ++i) m.chipset.mem[0x600 + i] = 0;
    m.cpu.cs = m.cpu.ds = 0;
    m.cpu.eip = 0x400;
    m.run_cycles(200);
    ASSERT_EQ(m.cpu.cs, 0xF000);
    EXPECT_EQ(m.cpu.cr(0) & kCdNw, 0u);
}

TEST(MachineTest, ACacheMissAndAnIsaPortCostTheirBusCycles) {
    Machine m;
    m.reset();
    const uint8_t prog[] = {0xA0, 0x00, 0x20,   // mov al,[2000h]
                            0xA0, 0x00, 0x20,   // mov al,[2000h]
                            0xE6, 0x80,         // out 80h,al
                            0xF4};
    for (std::size_t i = 0; i < sizeof prog; ++i) m.chipset.mem[0x400 + i] = prog[i];
    m.cpu.cs = m.cpu.ds = 0;
    m.cpu.eip = 0x400;
    // code fill 9 bus clocks; data fill waits for the burst (18) then 9 more; core clocks double on the DX2
    EXPECT_EQ(m.cpu.step(), 1 + 18 + 36);
    // data burst ends at core clock 72, this read is at 55
    EXPECT_EQ(m.cpu.step(), 1 + 17);
    // published 16 plus an 8-bit ISA cycle (26) less the 2 already counted
    EXPECT_EQ(m.cpu.step(), 16 + 52 - 2);
}

// pipeline penalties missing from the clock tables (Intel486 Developer's Manual 27302101, 12.3.1)
TEST(MachineTest, ThePipelineChargesAgiMisalignmentAndDisplacementWithImmediate) {
    Machine m;
    m.reset();
    const uint8_t prog[] = {0xBB, 0x00, 0x20,         // mov bx,2000h
                            0x8A, 0x07,               // mov al,[bx]
                            0x90,                     // nop
                            0x8A, 0x07,               // mov al,[bx]
                            0xA1, 0x01, 0x20,         // mov ax,[2001h]
                            0xA1, 0x00, 0x20,         // mov ax,[2000h]
                            0x83, 0x47, 0x02, 0x05,   // add word [bx+2],5
                            0x83, 0x07, 0x05};        // add word [bx],5
    for (std::size_t i = 0; i < sizeof prog; ++i) m.chipset.mem[0x400 + i] = prog[i];
    m.cpu.cs = m.cpu.ds = 0;
    // two warm passes: first leaves its last code line filling
    for (int pass = 0; pass < 2; ++pass) {
        m.cpu.ebx = 0;
        m.cpu.eip = 0x400;
        for (int i = 0; i < 8; ++i) m.cpu.step();
    }
    m.cpu.ebx = 0;
    m.cpu.eip = 0x400;
    int cost[8];
    for (int &c : cost) c = m.cpu.step();
    EXPECT_EQ(cost[1] - cost[3], 1) << "rule 4: BX was written by the instruction before";
    EXPECT_EQ(cost[4] - cost[5], 3) << "rule 2: a word at an odd address";
    EXPECT_EQ(cost[6] - cost[7], 1) << "rule 8: a displacement and an immediate";
}

TEST(MachineTest, ALongRepYieldsEveryPitCount) {
    pc486::Machine m;
    EXPECT_EQ(m.cpu.rep_yield_cycles, 55u);
    m.set_cpu_hz(33000000.0);
    EXPECT_EQ(m.cpu.rep_yield_cycles, 27u);
}

}  // namespace
