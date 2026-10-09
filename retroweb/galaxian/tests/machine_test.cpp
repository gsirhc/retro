#include <gtest/gtest.h>

#include "hwtest_roms.h"
#include "machine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <vector>

namespace {

galaxian::RomSet test_set() {
    galaxian::RomSet s;
    std::copy(galaxian::hwtest::program.begin(), galaxian::hwtest::program.end(), s.program.begin());
    std::copy(galaxian::hwtest::gfx.begin(), galaxian::hwtest::gfx.end(), s.gfx.begin());
    std::copy(galaxian::hwtest::color_prom.begin(), galaxian::hwtest::color_prom.end(),
              s.color_prom.begin());
    return s;
}

}  // namespace

TEST(Machine, LoadRomsDoesNotSwapBits) {
    galaxian::RomSet s;
    s.gfx[0x800] = 0x01;
    galaxian::Machine m;
    m.load_roms(s);
    EXPECT_EQ(m.video.gfx[0x800], 0x01);
}

TEST(Machine, NamcoMapWorkRamVramAndMirror) {
    galaxian::Machine m;
    m.reset();
    m.mem_write(0x4000, 0xA5);
    EXPECT_EQ(m.mem_read(0x4000), 0xA5);
    EXPECT_EQ(m.ram[0], 0xA5);
    EXPECT_EQ(m.mem_read(0x4400), 0xA5);  // 1K mirror
    m.mem_write(0x5000, 0x11);
    EXPECT_EQ(m.video.videoram[0], 0x11);
    EXPECT_EQ(m.mem_read(0x5400), 0x11);
    m.mem_write(0x5800, 0x22);
    EXPECT_EQ(m.video.objram[0], 0x22);
    EXPECT_EQ(m.mem_read(0x5F00), 0x22);
}

TEST(Machine, WatchdogAt7800) {
    galaxian::Machine m;
    m.reset();
    EXPECT_EQ(m.mem_read(0x7800), 0xFF);
    EXPECT_EQ(m.watchdog_, galaxian::kWatchdogFrames);
}

TEST(Machine, WatchdogExpiresAfterEightVblanksWithoutKick) {
    galaxian::RomSet s;
    s.program[0] = 0x76;  // HALT
    galaxian::Machine m;
    m.load_roms(s);
    m.reset();
    m.run_cycles(galaxian::kCpuPerFrame * 10);
    EXPECT_TRUE(m.watchdog_reset);
}

TEST(Machine, WatchdogResetKeepsRam) {
    galaxian::RomSet s;
    s.program[0] = 0x76;  // HALT
    galaxian::Machine m;
    m.load_roms(s);
    m.reset();
    m.ram[0x10] = 0xA5;
    m.video.videoram[5] = 0x5A;
    m.run_cycles(galaxian::kCpuPerFrame * 10);
    ASSERT_TRUE(m.watchdog_reset);
    EXPECT_EQ(m.ram[0x10], 0xA5);
    EXPECT_EQ(m.video.videoram[5], 0x5A);
    EXPECT_EQ(m.frames, 0);
}

namespace {

// Counts NMIs at $4000; `rearm` writes 0 then 1 to $7001 inside the handler.
galaxian::RomSet nmi_counter(bool rearm) {
    galaxian::RomSet s;
    const uint8_t main[] = {0x31, 0x00, 0x44, 0x21, 0x00, 0x40, 0x3E, 0x01,
                            0x32, 0x01, 0x70, 0x3A, 0x00, 0x78, 0x18, 0xFB};
    std::copy(std::begin(main), std::end(main), s.program.begin());
    std::vector<uint8_t> nmi = {0xF5, 0x34};
    if (rearm) nmi.insert(nmi.end(), {0xAF, 0x32, 0x01, 0x70, 0x3C, 0x32, 0x01, 0x70});
    nmi.insert(nmi.end(), {0xF1, 0xED, 0x45});
    std::copy(nmi.begin(), nmi.end(), s.program.begin() + 0x66);
    return s;
}

}  // namespace

TEST(Machine, VblankNmiHoldsUntilTheEnableLatchIsCleared) {
    galaxian::Machine m;
    m.load_roms(nmi_counter(false));
    m.reset();
    m.run_cycles(galaxian::kCpuPerFrame * 6);
    EXPECT_EQ(m.ram[0], 1);
}

TEST(Machine, VblankNmiFiresEveryFrameWhenRearmed) {
    galaxian::Machine m;
    m.load_roms(nmi_counter(true));
    m.reset();
    m.run_cycles(galaxian::kCpuPerFrame * 6);
    EXPECT_EQ(m.ram[0], 6);
}

TEST(Machine, CoinLockoutTurnsCoinsAway) {
    galaxian::Machine m;
    m.reset();
    m.inputs.in0 = 0x01;
    EXPECT_EQ(m.mem_read(0x6000), 0x00);
    m.mem_write(0x6002, 1);
    EXPECT_EQ(m.mem_read(0x6000), 0x01);
}

TEST(Machine, CoinCounterCountsRisingEdges) {
    galaxian::Machine m;
    m.reset();
    m.mem_write(0x6003, 1);
    m.mem_write(0x6003, 1);
    m.mem_write(0x6003, 0);
    m.mem_write(0x6003, 1);
    EXPECT_EQ(m.coin_counter[0], 2);
    m.reset();
    EXPECT_EQ(m.coin_counter[0], 2) << "the meter is mechanical";
}

TEST(Machine, PortsAndLatches) {
    galaxian::Machine m;
    m.reset();
    m.inputs.in0 = 0x15;
    m.inputs.in1 = 0x03;
    m.inputs.in2 = 0x04;
    m.mem_write(0x6002, 1);
    EXPECT_EQ(m.mem_read(0x6000), 0x15);
    EXPECT_EQ(m.mem_read(0x6800), 0x03);
    EXPECT_EQ(m.mem_read(0x7000), 0x04);
    m.mem_write(0x7001, 1);
    EXPECT_TRUE(m.nmi_enable);
    m.mem_write(0x7004, 1);
    EXPECT_TRUE(m.video.stars_enable());
    m.mem_write(0x7006, 1);
    m.mem_write(0x7007, 1);
    EXPECT_TRUE(m.video.flip_x);
    EXPECT_TRUE(m.video.flip_y);
}

TEST(Machine, DiscreteSoundLatches) {
    galaxian::Machine m;
    m.reset();
    m.mem_write(0x6800, 1);
    m.mem_write(0x6801, 1);
    m.mem_write(0x6802, 1);
    m.mem_write(0x6803, 1);
    m.mem_write(0x6805, 1);
    m.mem_write(0x6806, 1);
    m.mem_write(0x6807, 1);
    EXPECT_TRUE(m.sound.fs[0]);
    EXPECT_TRUE(m.sound.fs[1]);
    EXPECT_TRUE(m.sound.fs[2]);
    EXPECT_TRUE(m.sound.hit);
    EXPECT_TRUE(m.sound.fire);
    EXPECT_TRUE(m.sound.vol[0]);
    EXPECT_TRUE(m.sound.vol[1]);
    m.mem_write(0x6004, 1);
    m.mem_write(0x6007, 1);
    EXPECT_EQ(m.sound.lfo_bits, 0x09);
    m.mem_write(0x7800, 0x40);
    EXPECT_EQ(m.sound.pitch, 0x40);
}

TEST(Machine, UnusedLatchesDoNothing) {
    galaxian::Machine m;
    m.reset();
    m.mem_write(0x6804, 1);
    for (uint16_t a : {0x7000, 0x7002, 0x7003, 0x7005}) m.mem_write(a, 1);
    EXPECT_FALSE(m.sound.hit);
    EXPECT_FALSE(m.sound.fire);
    EXPECT_FALSE(m.nmi_enable);
    EXPECT_FALSE(m.video.stars_enable());
    EXPECT_FALSE(m.video.flip_x);
    EXPECT_FALSE(m.video.flip_y);
}

TEST(Machine, JoystickEchoesToRam) {
    galaxian::Machine m;
    m.load_roms(test_set());
    m.reset();
    m.inputs.in0 = 0x1C;
    m.run_cycles(galaxian::kCpuHz / 20);
    EXPECT_EQ(m.ram[0x10], 0x1C);
}

TEST(Machine, HwtestStaysSilent) {
    galaxian::Machine m;
    m.load_roms(test_set());
    m.reset();
    m.audio.clear();
    m.run_cycles(galaxian::kCpuHz * 4);
    ASSERT_FALSE(m.audio.empty());
    double peak = 0;
    for (size_t i = m.audio.size() / 2; i < m.audio.size(); i++) peak = std::max(peak, double(std::fabs(m.audio[i])));
    EXPECT_LT(peak, 1e-3);
}

TEST(Machine, HwtestHelpScreenShowsCopyrightPrompt) {
    galaxian::Machine m;
    m.load_roms(test_set());
    m.reset();
    m.run_cycles(galaxian::kCpuHz * 4);
    std::array<uint32_t, galaxian::kUprightW * galaxian::kUprightH> rgb{};
    m.render(rgb.data());
    int lit = 0, yellow = 0, white = 0;
    for (uint32_t p : rgb) {
        int r = int(p >> 16), g = int(p >> 8) & 0xFF, b = int(p & 0xFF);
        if (r | g | b) lit++;
        if (r > 180 && g > 180 && b < 40) yellow++;
        if (r > 180 && g > 180 && b > 80) white++;
    }
    EXPECT_GT(yellow, 40);
    EXPECT_GT(white, 80);
    EXPECT_LT(lit, galaxian::kUprightW * galaxian::kUprightH * 50 / 100);
}

TEST(Machine, FactoryDipIdles) {
    galaxian::Inputs in;
    EXPECT_EQ(in.in0, 0x00);
    EXPECT_EQ(in.in1, 0x00);
    EXPECT_EQ(in.in2, 0x04);
}

TEST(Machine, StarsDoNotBlink) {
    galaxian::Machine m;
    m.reset();
    m.video.set_stars_enable(true);
    m.run_cycles(galaxian::kCpuHz);
    EXPECT_EQ(m.video.stars_blink_state, 0);
}
