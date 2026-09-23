#include <gtest/gtest.h>

#include "hwtest_roms.h"
#include "machine.h"

#include <algorithm>
#include <array>

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

TEST(Machine, PortsAndLatches) {
    galaxian::Machine m;
    m.reset();
    m.inputs.in0 = 0x15;
    m.inputs.in1 = 0x03;
    m.inputs.in2 = 0x04;
    EXPECT_EQ(m.mem_read(0x6000), 0x15);
    EXPECT_EQ(m.mem_read(0x6800), 0x03);
    EXPECT_EQ(m.mem_read(0x7000), 0x04);
    m.mem_write(0x7001, 1);
    EXPECT_TRUE(m.nmi_enable);
    m.mem_write(0x7004, 1);
    EXPECT_TRUE(m.video.stars_enable);
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
    bool any = false;
    for (float s : m.audio) if (s != 0.0f) { any = true; break; }
    EXPECT_FALSE(any);
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
    m.video.stars_enable = true;
    m.run_cycles(galaxian::kCpuHz);
    EXPECT_EQ(m.video.stars_blink_state, 0);
}
