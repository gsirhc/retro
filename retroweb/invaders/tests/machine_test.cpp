#include <gtest/gtest.h>

#include "machine.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>

namespace {

constexpr uint8_t HLT = 0x76, EI = 0xFB, RET = 0xC9, JMP = 0xC3;

invaders::RomSet rom(std::initializer_list<uint8_t> at0) {
    invaders::RomSet s;
    std::copy(at0.begin(), at0.end(), s.program.begin());
    return s;
}

// EI; HLT; JMP 0 at reset; RST 1 and RST 2 just EI; RET.
invaders::RomSet halt_loop(bool kick) {
    invaders::RomSet s;
    const uint8_t main[] = {0x31, 0x00, 0x24, EI, HLT, JMP, 0x03, 0x00};
    std::copy(std::begin(main), std::end(main), s.program.begin());
    s.program[0x08] = EI;
    s.program[0x09] = RET;
    if (kick) {
        const uint8_t isr[] = {0xD3, 0x06, EI, RET};  // OUT 6
        std::copy(std::begin(isr), std::end(isr), s.program.begin() + 0x10);
    } else {
        s.program[0x10] = EI;
        s.program[0x11] = RET;
    }
    return s;
}

void run_to_line(invaders::Machine& m, int line) {
    for (int guard = 0; guard < invaders::kFrameCycles * 2 && m.line() != line; guard++)
        m.run_cycles(1);
}

bool upright_lit(const invaders::Machine& m, int x, int y) {
    static std::array<uint32_t, invaders::kUprightW * invaders::kUprightH> px;
    m.render(px.data());
    return px[size_t(y) * invaders::kUprightW + size_t(x)] != 0;
}

}  // namespace

TEST(Machine, ClockAndFrameTiming) {
    EXPECT_EQ(invaders::kCpuHz, 1996800);
    EXPECT_EQ(invaders::kFrameCycles, 33536);
    invaders::Machine m;
    m.load_roms(halt_loop(true));
    m.reset();
    m.run_cycles(invaders::kCpuHz);
    EXPECT_GE(m.frames, 59);
    EXPECT_LE(m.frames, 60);
}

TEST(Machine, RamMirrorsAndA15Ignored) {
    invaders::Machine m;
    m.reset();
    m.mem_write(0x2000, 0xA5);
    EXPECT_EQ(m.ram[0], 0xA5);
    EXPECT_EQ(m.mem_read(0x6000), 0xA5);
    EXPECT_EQ(m.mem_read(0xA000), 0xA5);
    EXPECT_EQ(m.mem_read(0xE000), 0xA5);
    m.mem_write(0x7FFF, 0x5A);
    EXPECT_EQ(m.mem_read(0x3FFF), 0x5A);
}

TEST(Machine, RomIsReadOnlyAndEmptySocketReadsZero) {
    invaders::Machine m;
    m.load_roms(rom({0x12, 0x34}));
    m.reset();
    m.mem_write(0x0000, 0xFF);
    EXPECT_EQ(m.mem_read(0x0000), 0x12);
    EXPECT_EQ(m.mem_read(0x8001), 0x34);
    m.mem_write(0x4000, 0xFF);
    EXPECT_EQ(m.mem_read(0x4000), 0x00);
}

TEST(Machine, InputPortsDecodeA0A1AndShifterOnPort3) {
    invaders::Machine m;
    m.reset();
    m.inputs.in0 = 0x11;
    m.inputs.in1 = 0x22;
    m.inputs.in2 = 0x44;
    EXPECT_EQ(m.port_in(0), 0x11);
    EXPECT_EQ(m.port_in(4), 0x11);
    EXPECT_EQ(m.port_in(1), 0x22);
    EXPECT_EQ(m.port_in(5), 0x22);
    EXPECT_EQ(m.port_in(2), 0x44);
    EXPECT_EQ(m.port_in(0xFE), 0x44);
    m.port_out(4, 0xFF);
    m.port_out(4, 0x00);
    m.port_out(2, 7);
    EXPECT_EQ(m.port_in(7), 0x7F);
}

TEST(Machine, IdleInputsMatchThePcb) {
    invaders::Inputs in;
    EXPECT_EQ(in.in0, 0x08);
    EXPECT_EQ(in.in1 & 0x01, 0x01) << "COIN is active-low";
    EXPECT_EQ(in.in2, 0x00);
}

TEST(Machine, Mb14241ShiftsNewestByteHigh) {
    for (int count = 0; count < 8; count++) {
        invaders::Mb14241 s;
        s.data_w(0xA5);
        s.data_w(0x3C);
        s.count_w(uint8_t(count | 0xF8));
        const uint16_t reg = 0x3CA5;
        EXPECT_EQ(s.result(), uint8_t(reg >> (8 - count))) << "count " << count;
    }
}

TEST(Machine, ShifterThroughThePorts) {
    invaders::Machine m;
    m.reset();
    m.port_out(4, 0x81);
    m.port_out(4, 0x42);
    m.port_out(2, 3);
    EXPECT_EQ(m.port_in(3), uint8_t(0x4281 >> 5));
}

TEST(Machine, VectorsFollow64V) {
    EXPECT_EQ(invaders::Machine::vcounter(0), 0x20);
    EXPECT_EQ(invaders::Machine::vcounter(96), 0x80);
    EXPECT_EQ(invaders::Machine::vcounter(224), 0xDA);
    EXPECT_EQ(invaders::Machine::vcounter(261), 0xFF);
    EXPECT_EQ(invaders::Machine::rst_vector(96), 0xCF);
    EXPECT_EQ(invaders::Machine::rst_vector(224), 0xD7);
    EXPECT_EQ(invaders::Machine::rst_vector(160), 0xD7) << "64V set from line 160";
    EXPECT_EQ(invaders::Machine::rst_vector(100), 0xCF);
}

TEST(Machine, Rst1AtLine96AndRst2AtLine224) {
    invaders::Machine m;
    m.load_roms(halt_loop(true));
    m.reset();
    int seen08 = -1, seen10 = -1;
    for (int i = 0; i < invaders::kFrameCycles * 2 && (seen08 < 0 || seen10 < 0); i++) {
        m.run_cycles(1);
        if (m.cpu.pc == 0x08 && seen08 < 0) seen08 = m.line();
        if (m.cpu.pc == 0x10 && seen10 < 0) seen10 = m.line();
    }
    EXPECT_EQ(seen08, 96);
    EXPECT_EQ(seen10, 224);
}

TEST(Machine, IntLatchesInteAtTheEdgeOnly) {
    invaders::Machine m;
    m.load_roms(rom({0x31, 0x00, 0x24, JMP, 0x03, 0x00}));
    m.reset();
    run_to_line(m, 100);
    EXPECT_FALSE(m.int_pending()) << "INTE was low when line 96 clocked E3";
    m.cpu.int_enabled = true;
    run_to_line(m, 200);
    EXPECT_NE(m.cpu.pc, 0x08);
    for (int i = 0; i < invaders::kLineCycles * 40 && m.cpu.pc != 0x10; i++) m.run_cycles(1);
    EXPECT_EQ(m.cpu.pc, 0x10);
    EXPECT_EQ(m.line(), 224);
}

TEST(Machine, WatchdogResetsWithoutOut6AndKeepsRam) {
    invaders::Machine m;
    m.load_roms(halt_loop(false));
    m.reset();
    m.ram[0x100] = 0xA5;
    m.run_cycles(invaders::kFrameCycles * 254);
    EXPECT_FALSE(m.watchdog_reset);
    m.run_cycles(invaders::kFrameCycles * 2);
    EXPECT_TRUE(m.watchdog_reset);
    EXPECT_EQ(m.ram[0x100], 0xA5);
}

TEST(Machine, Out6FeedsTheWatchdog) {
    invaders::Machine m;
    m.load_roms(halt_loop(true));
    m.reset();
    m.run_cycles(invaders::kFrameCycles * 600);
    EXPECT_FALSE(m.watchdog_reset);
}

TEST(Machine, RenderIsRot270WithFourBlankRowsAtTheBottom) {
    invaders::Machine m;
    m.load_roms(halt_loop(true));
    m.reset();
    m.ram[0x400 + 31] = 0x80;          // line 0, last byte, bit 7
    m.ram[0x400 + 223 * 32] = 0x01;    // line 223, first byte, bit 0
    m.run_cycles(invaders::kFrameCycles);
    EXPECT_TRUE(upright_lit(m, 0, 0));
    EXPECT_TRUE(upright_lit(m, 223, 255));
    EXPECT_FALSE(upright_lit(m, 1, 0));
    std::fill(m.ram.begin() + 0x400, m.ram.end(), 0xFF);
    m.run_cycles(invaders::kFrameCycles);
    for (int y = 256; y < invaders::kUprightH; y++) EXPECT_FALSE(upright_lit(m, 100, y)) << y;
    EXPECT_TRUE(upright_lit(m, 100, 255));
}

TEST(Machine, BeamLatchesVramAsItPasses) {
    invaders::Machine m;
    m.load_roms(halt_loop(true));
    m.reset();
    run_to_line(m, 50);
    m.run_cycles(invaders::kLineCycles / 2);
    ASSERT_EQ(m.line(), 50);
    m.ram[0x400 + 50 * 32 + 0] = 0x01;    // already fetched this line
    m.ram[0x400 + 50 * 32 + 31] = 0x80;   // not yet fetched
    run_to_line(m, 230);
    EXPECT_FALSE(upright_lit(m, 50, 255)) << "byte 0 changed after the beam passed it";
    EXPECT_TRUE(upright_lit(m, 50, 0));
    run_to_line(m, 0);
    run_to_line(m, 230);
    EXPECT_TRUE(upright_lit(m, 50, 255));
}
