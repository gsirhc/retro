#include <gtest/gtest.h>

#include "board.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

namespace {

constexpr uint8_t HLT = 0x76, EI = 0xFB, RET = 0xC9, JMP = 0xC3;

void rom(mw8080::Board& m, std::initializer_list<uint8_t> at0) {
    m.program.fill(0);
    std::copy(at0.begin(), at0.end(), m.program.begin());
}

// EI; HLT; JMP 0 at reset; RST 1 and RST 2 just EI; RET.
void halt_loop(mw8080::Board& m, bool kick) {
    rom(m, {0x31, 0x00, 0x24, EI, HLT, JMP, 0x03, 0x00});
    m.program[0x08] = EI;
    m.program[0x09] = RET;
    if (kick) {
        const uint8_t isr[] = {0xD3, 0x06, EI, RET};  // OUT 6
        std::copy(std::begin(isr), std::end(isr), m.program.begin() + 0x10);
    } else {
        m.program[0x10] = EI;
        m.program[0x11] = RET;
    }
}

void run_to_line(mw8080::Board& m, int line) {
    for (int guard = 0; guard < mw8080::kFrameCycles * 2 && m.line() != line; guard++)
        m.run_cycles(1);
}

bool upright_lit(const mw8080::Board& m, int x, int y) { return m.lit(x, y); }

class Recorder : public mw8080::Board {
public:
    std::vector<std::pair<int, int>> outs;
    long elapsed_t = 0;

protected:
    void sound_out(uint8_t port, uint8_t v) override { outs.emplace_back(port, v); }
    void elapsed(int t) override { elapsed_t += t; }
};

}  // namespace

TEST(Board, ClockAndFrameTiming) {
    EXPECT_EQ(mw8080::kCpuHz, 1996800);
    EXPECT_EQ(mw8080::kFrameCycles, 33536);
    mw8080::Board m;
    halt_loop(m, true);
    m.reset();
    m.run_cycles(mw8080::kCpuHz);
    EXPECT_GE(m.frames, 59);
    EXPECT_LE(m.frames, 60);
}

TEST(Board, RamMirrorsAndA15Ignored) {
    mw8080::Board m;
    m.reset();
    m.mem_write(0x2000, 0xA5);
    EXPECT_EQ(m.ram[0], 0xA5);
    EXPECT_EQ(m.mem_read(0x6000), 0xA5);
    EXPECT_EQ(m.mem_read(0xA000), 0xA5);
    EXPECT_EQ(m.mem_read(0xE000), 0xA5);
    m.mem_write(0x7FFF, 0x5A);
    EXPECT_EQ(m.mem_read(0x3FFF), 0x5A);
}

TEST(Board, RomIsReadOnlyAndEmptySocketReadsZero) {
    mw8080::Board m;
    rom(m, {0x12, 0x34});
    m.reset();
    m.mem_write(0x0000, 0xFF);
    EXPECT_EQ(m.mem_read(0x0000), 0x12);
    EXPECT_EQ(m.mem_read(0x8001), 0x34);
    m.mem_write(0x4000, 0xFF);
    EXPECT_EQ(m.mem_read(0x4000), 0x00);
}

TEST(Board, InputPortsDecodeA0A1AndShifterOnPort3) {
    mw8080::Board m;
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

TEST(Board, ShifterPutsNewestByteHigh) {
    for (int count = 0; count < 8; count++) {
        mw8080::Shifter s;
        s.data_w(0xA5);
        s.data_w(0x3C);
        s.offset_w(uint8_t(count | 0xF8));
        const uint16_t reg = 0x3CA5;
        EXPECT_EQ(s.result(), uint8_t(reg >> (8 - count))) << "count " << count;
    }
}

TEST(Board, ShifterThroughThePorts) {
    mw8080::Board m;
    m.reset();
    m.port_out(4, 0x81);
    m.port_out(4, 0x42);
    m.port_out(2, 3);
    EXPECT_EQ(m.port_in(3), uint8_t(0x4281 >> 5));
}

TEST(Board, VectorsFollow64V) {
    EXPECT_EQ(mw8080::Board::vcounter(0), 0x20);
    EXPECT_EQ(mw8080::Board::vcounter(96), 0x80);
    EXPECT_EQ(mw8080::Board::vcounter(224), 0xDA);
    EXPECT_EQ(mw8080::Board::vcounter(261), 0xFF);
    EXPECT_EQ(mw8080::Board::rst_vector(96), 0xCF);
    EXPECT_EQ(mw8080::Board::rst_vector(224), 0xD7);
    EXPECT_EQ(mw8080::Board::rst_vector(160), 0xD7) << "64V set from line 160";
    EXPECT_EQ(mw8080::Board::rst_vector(100), 0xCF);
}

TEST(Board, Rst1AtLine96AndRst2AtLine224) {
    mw8080::Board m;
    halt_loop(m, true);
    m.reset();
    int seen08 = -1, seen10 = -1;
    for (int i = 0; i < mw8080::kFrameCycles * 2 && (seen08 < 0 || seen10 < 0); i++) {
        m.run_cycles(1);
        if (m.cpu.pc == 0x08 && seen08 < 0) seen08 = m.line();
        if (m.cpu.pc == 0x10 && seen10 < 0) seen10 = m.line();
    }
    EXPECT_EQ(seen08, 96);
    EXPECT_EQ(seen10, 224);
}

TEST(Board, IntLatchesInteAtTheEdgeOnly) {
    mw8080::Board m;
    rom(m, {0x31, 0x00, 0x24, JMP, 0x03, 0x00});
    m.reset();
    run_to_line(m, 100);
    EXPECT_FALSE(m.int_pending()) << "INTE was low when line 96 clocked E3";
    m.cpu.int_enabled = true;
    run_to_line(m, 200);
    EXPECT_NE(m.cpu.pc, 0x08);
    for (int i = 0; i < mw8080::kLineCycles * 40 && m.cpu.pc != 0x10; i++) m.run_cycles(1);
    EXPECT_EQ(m.cpu.pc, 0x10);
    EXPECT_EQ(m.line(), 224);
}

TEST(Board, WatchdogResetsWithoutOut6AndKeepsRam) {
    mw8080::Board m;
    halt_loop(m, false);
    m.reset();
    m.ram[0x100] = 0xA5;
    m.run_cycles(mw8080::kFrameCycles * 254);
    EXPECT_FALSE(m.watchdog_reset);
    m.run_cycles(mw8080::kFrameCycles * 2);
    EXPECT_TRUE(m.watchdog_reset);
    EXPECT_EQ(m.ram[0x100], 0xA5);
}

TEST(Board, Out6FeedsTheWatchdog) {
    mw8080::Board m;
    halt_loop(m, true);
    m.reset();
    m.run_cycles(mw8080::kFrameCycles * 600);
    EXPECT_FALSE(m.watchdog_reset);
}

TEST(Board, RenderIsRot270WithFourBlankRowsAtTheBottom) {
    mw8080::Board m;
    halt_loop(m, true);
    m.reset();
    m.ram[0x400 + 31] = 0x80;          // line 0, last byte, bit 7
    m.ram[0x400 + 223 * 32] = 0x01;    // line 223, first byte, bit 0
    m.run_cycles(mw8080::kFrameCycles);
    EXPECT_TRUE(upright_lit(m, 0, 0));
    EXPECT_TRUE(upright_lit(m, 223, 255));
    EXPECT_FALSE(upright_lit(m, 1, 0));
    std::fill(m.ram.begin() + 0x400, m.ram.end(), 0xFF);
    m.run_cycles(mw8080::kFrameCycles);
    for (int y = 256; y < mw8080::kUprightH; y++) EXPECT_FALSE(upright_lit(m, 100, y)) << y;
    EXPECT_TRUE(upright_lit(m, 100, 255));
}

TEST(Board, BeamLatchesVramAsItPasses) {
    mw8080::Board m;
    halt_loop(m, true);
    m.reset();
    run_to_line(m, 50);
    m.run_cycles(mw8080::kLineCycles / 2);
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

TEST(Board, SecondRomBankAt4000) {
    mw8080::Board m;
    m.program_hi[0] = 0x5A;
    m.program_hi[0x7FF] = 0xA5;
    EXPECT_EQ(m.mem_read(0x4000), 0x5A);
    EXPECT_EQ(m.mem_read(0xC7FF), 0xA5);
    m.mem_write(0x4000, 0);
    EXPECT_EQ(m.mem_read(0x4000), 0x5A);
}

TEST(Board, WatchdogCanBeAbsent) {
    mw8080::Board m;
    m.watchdog_enabled = false;
    halt_loop(m, false);
    m.reset();
    m.run_cycles(mw8080::kFrameCycles * 300);
    EXPECT_FALSE(m.watchdog_reset);
}

TEST(Board, Out3And5ReachTheSoundBoardAndTimeIsReported) {
    Recorder m;
    m.port_out(3, 0x21);
    m.port_out(0x0D, 0x0F);
    m.port_out(6, 0xFF);
    m.port_out(7, 0xFF);
    ASSERT_EQ(m.outs.size(), 2u);
    EXPECT_EQ(m.outs[0], std::make_pair(3, 0x21));
    EXPECT_EQ(m.outs[1], std::make_pair(5, 0x0F));
    halt_loop(m, true);
    m.reset();
    const int done = m.run_cycles(10000);
    EXPECT_EQ(m.elapsed_t, done);
}

TEST(Board, BeamTagIsLatchedPerLine) {
    mw8080::Board m;
    halt_loop(m, true);
    m.reset();
    run_to_line(m, 100);
    m.beam_tag = 1;
    run_to_line(m, 230);
    EXPECT_EQ(m.tag(99), 0);
    EXPECT_EQ(m.tag(101), 1);
}
