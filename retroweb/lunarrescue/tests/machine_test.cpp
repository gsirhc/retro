#include <gtest/gtest.h>

#include "hwtest_roms.h"
#include "hwtest_set.h"
#include "machine.h"

#include <array>
#include <cstdint>

namespace {

// OUT 3 / OUT 5 from the reset vector, then spin.
lunarrescue::RomSet outs(uint8_t p3, uint8_t p5) {
    lunarrescue::RomSet s;
    const uint8_t code[] = {0x3E, p3, 0xD3, 0x03, 0x3E, p5, 0xD3, 0x05, 0xC3, 0x08, 0x00};
    std::copy(std::begin(code), std::end(code), s.program.begin());
    return s;
}

// Run the self-test until its frame counter reaches n.
void hwtest_to_frame(lunarrescue::Machine& m, int n) {
    m.load_roms(lunarrescue::hwtest_set());
    m.reset();
    while (m.frames < n) m.run_cycles(lunarrescue::kLineCycles);
}

}  // namespace

TEST(Machine, IdleInputsMatchTheTaitoPcb) {
    lunarrescue::Machine m;
    EXPECT_EQ(m.inputs.in0, 0xFF);
    EXPECT_EQ(m.inputs.in1, 0x89) << "COIN active-low, bits 3 and 7 high";
    EXPECT_EQ(m.inputs.in2, 0x00);
}

TEST(Machine, NoWatchdog) {
    lunarrescue::Machine m;
    m.load_roms(outs(0x20, 0x00));
    m.reset();
    m.run_cycles(lunarrescue::kFrameCycles * 300);
    EXPECT_FALSE(m.watchdog_reset);
}

TEST(Machine, SecondBankHoldsRoms5And6) {
    lunarrescue::Machine m;
    lunarrescue::RomSet s;
    s.program_hi[0] = 0x11;
    s.program_hi[0xFFF] = 0x22;
    m.load_roms(s);
    EXPECT_EQ(m.mem_read(0x4000), 0x11);
    EXPECT_EQ(m.mem_read(0x4FFF), 0x22);
    EXPECT_EQ(m.mem_read(0x5000), 0x00);
}

TEST(Machine, OutsReachTheTaitoSoundBoard) {
    lunarrescue::Machine m;
    m.load_roms(outs(0x20, 0x20));
    m.reset();
    m.run_cycles(200);
    EXPECT_TRUE(m.sound.amp_enabled());
    EXPECT_TRUE(m.sound.flip_screen());
}

TEST(Machine, ColourComesFromThePromByCell) {
    lunarrescue::Machine m;
    lunarrescue::RomSet s = outs(0x20, 0x00);
    // (V >> 3) * 32 + k: line 0 is V 0x20, so cell column 0 reads PROM row 4.
    s.prom[4 * 32 + 31] = 0x04;
    s.prom[4 * 32 + 30] = 0x02;
    s.prom[5 * 32 + 31] = 0x0B;
    m.load_roms(s);
    m.reset();
    m.ram[0x400 + 31] = 0x80;
    m.ram[0x400 + 30] = 0x80;
    m.ram[0x400 + 8 * 32 + 31] = 0x80;
    m.run_cycles(lunarrescue::kFrameCycles);
    EXPECT_EQ(m.pen(0, 0), 4);
    EXPECT_EQ(m.pen(0, 8), 2);
    EXPECT_EQ(m.pen(8, 0), 3) << "PROM bit 3 is not a colour line";
    EXPECT_EQ(m.pen(1, 0), 0) << "background is black";
    static std::array<uint32_t, lunarrescue::kUprightW * lunarrescue::kUprightH> px;
    m.render(px.data());
    EXPECT_EQ(px[0], 0x00FF00u) << "PROM bit 2 drives green";
    EXPECT_EQ(px[size_t(8) * lunarrescue::kUprightW], 0x0000FFu) << "bit 1 is blue";
    EXPECT_EQ(px[8], 0xFF00FFu);
}

TEST(Machine, HwtestShowsThePromBandsOnTheWhiteField) {
    lunarrescue::Machine m;
    hwtest_to_frame(m, 120);
    EXPECT_FALSE(m.screen_red());
    // ux 100 is V 0x84, PROM row 16
    for (int uy = 0; uy < 256; uy += 8)
        EXPECT_EQ(m.pen(100, uy), lunarrescue::hwtest::prom[16 * 32 + 31 - (uy >> 3)] & 7) << uy;
    EXPECT_NE(m.pen(100, 0), m.pen(100, 40));
}

TEST(Machine, Out3Bit2TurnsTheWholeScreenRed) {
    lunarrescue::Machine m;
    hwtest_to_frame(m, 170);
    ASSERT_TRUE(m.screen_red());
    for (int uy : {0, 40, 120, 250}) EXPECT_EQ(m.pen(100, uy), 1) << uy;
    while (m.frames < 200) m.run_cycles(lunarrescue::kLineCycles);
    EXPECT_FALSE(m.screen_red());
}

TEST(Machine, RedLatchFollowsTheBeamPerLine) {
    lunarrescue::Machine m;
    lunarrescue::RomSet s;
    s.program[0] = 0xC3;  // JMP $0000
    s.prom.fill(0x07);
    m.load_roms(s);
    m.reset();
    std::fill(m.ram.begin() + 0x400, m.ram.end(), 0xFF);
    while (m.line() != 100) m.run_cycles(1);
    m.port_out(3, 0x24);
    const int f = m.frames;
    while (m.frames == f) m.run_cycles(1);
    EXPECT_EQ(m.pen(99, 0), 7);
    EXPECT_EQ(m.pen(100, 0), 7) << "line 100 started before the write";
    EXPECT_EQ(m.pen(101, 0), 1);
    EXPECT_EQ(m.pen(223, 0), 1);
}

TEST(Machine, BombLineOrsRedAndBlocksGreenAndBlue) {
    lunarrescue::Machine m;
    lunarrescue::RomSet s;
    s.program[0] = 0xC3;  // JMP $0000
    s.prom.fill(0x06);
    m.load_roms(s);
    m.reset();
    std::fill(m.ram.begin() + 0x400, m.ram.end(), 0xFF);
    m.port_out(3, 0x04);
    int f = m.frames;
    while (m.frames == f) m.run_cycles(1);
    f = m.frames;
    while (m.frames == f) m.run_cycles(1);
    EXPECT_EQ(m.pen(100, 0), 1) << "PROM red is 0, BOMB still lights red";
    m.port_out(3, 0x00);
    f = m.frames;
    while (m.frames == f) m.run_cycles(1);
    f = m.frames;
    while (m.frames == f) m.run_cycles(1);
    EXPECT_EQ(m.pen(100, 0), 6);
}

TEST(Machine, AudioIsSampledAtAudioHz) {
    lunarrescue::Machine m;
    m.load_roms(outs(0x00, 0x00));
    m.reset();
    m.run_cycles(lunarrescue::kCpuHz / 10);
    EXPECT_NEAR(double(m.audio.size()), 4800.0, 2.0);
    m.reset();
    EXPECT_TRUE(m.audio.empty());
}
