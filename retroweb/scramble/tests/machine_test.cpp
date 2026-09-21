#include <gtest/gtest.h>

#include "hwtest_roms.h"
#include "machine.h"

#include <algorithm>
#include <array>

namespace {

scramble::RomSet test_set() {
    scramble::RomSet s;
    std::copy(scramble::hwtest::program.begin(), scramble::hwtest::program.end(), s.program.begin());
    std::copy(scramble::hwtest::sound.begin(), scramble::hwtest::sound.end(), s.sound.begin());
    std::copy(scramble::hwtest::gfx.begin(), scramble::hwtest::gfx.end(), s.gfx.begin());
    std::copy(scramble::hwtest::color_prom.begin(), scramble::hwtest::color_prom.end(),
              s.color_prom.begin());
    return s;
}

}  // namespace

TEST(Machine, LoadRomsDoesNotSwapBits) {
    scramble::RomSet s;
    s.sound[0] = 0x01;
    s.gfx[0x800] = 0x01;
    scramble::Machine m;
    m.load_roms(s);
    EXPECT_EQ(m.sound_rom[0], 0x01);
    EXPECT_EQ(m.video.gfx[0x800], 0x01);
}

TEST(Machine, TheEndMapWorkRamAndVram) {
    scramble::Machine m;
    m.reset();
    m.mem_write(0x4000, 0xA5);
    EXPECT_EQ(m.mem_read(0x4000), 0xA5);
    EXPECT_EQ(m.ram[0], 0xA5);
    m.mem_write(0x4800, 0x11);
    EXPECT_EQ(m.video.videoram[0], 0x11);
    m.mem_write(0x5000, 0x22);
    EXPECT_EQ(m.video.objram[0], 0x22);
}

TEST(Machine, WatchdogAt7000) {
    scramble::Machine m;
    m.reset();
    EXPECT_EQ(m.mem_read(0x7000), 0xFF);
    EXPECT_EQ(m.watchdog_, scramble::kWatchdogFrames);
}

TEST(Machine, WatchdogExpiresAfterEightVblanksWithoutKick) {
    scramble::RomSet s;
    s.program[0] = 0x76;  // HALT
    scramble::Machine m;
    m.load_roms(s);
    m.reset();
    m.run_cycles(scramble::kCpuPerFrame * 10);
    EXPECT_TRUE(m.watchdog_reset);
}

TEST(Machine, LatchesAt6800) {
    scramble::Machine m;
    m.reset();
    m.mem_write(0x6801, 1);
    EXPECT_TRUE(m.nmi_enable);
    m.mem_write(0x6803, 1);
    EXPECT_TRUE(m.video.background_enable);
    m.mem_write(0x6804, 1);
    EXPECT_TRUE(m.video.stars_enable);
    m.mem_write(0x6806, 1);
    m.mem_write(0x6807, 1);
    EXPECT_TRUE(m.video.flip_x);
    EXPECT_TRUE(m.video.flip_y);
}

TEST(Machine, Ppi0IfA8Ppi1IfA9) {
    scramble::Machine m;
    m.reset();
    m.mem_write(0x8103, 0x9B);  // PPI0 inputs
    m.inputs.in0 = 0xA5;
    m.inputs.in1 = 0x5A;
    m.inputs.in2 = 0x51;
    EXPECT_EQ(m.mem_read(0x8100), 0xA5);
    EXPECT_EQ(m.mem_read(0x8101), 0x5A);
    EXPECT_EQ(m.mem_read(0x8102) & ~0xA0, 0x51 & ~0xA0);
}

TEST(Machine, Ppi1LatchFallingBit3InterruptsSoundCpu) {
    scramble::Machine m;
    m.reset();
    m.mem_write(0x8203, 0x80);
    m.sound.im = 1;
    m.sound.iff1 = m.sound.iff2 = true;
    m.sound.sp = 0x83F0;
    m.sound.pc = 0x1234;
    m.mem_write(0x8200, 0x07);
    EXPECT_EQ(m.sound_latch, 0x07);
    m.mem_write(0x8201, 0x08);
    EXPECT_EQ(m.sound.pc, 0x1234);
    m.mem_write(0x8201, 0x00);
    EXPECT_EQ(m.sound.pc, 0x0038);
}

TEST(Machine, TwoAysDecodeOnKonamiBits) {
    scramble::Machine m;
    m.sound_out(0x40, 8);   // AY1 address = volume A
    m.sound_out(0x80, 0x0F);
    EXPECT_EQ(m.ay1.regs[8], 0x0F);
    m.sound_out(0x10, 8);   // AY2 address
    m.sound_out(0x20, 0x0A);
    EXPECT_EQ(m.ay2.regs[8], 0x0A);
    m.sound_latch = 0x42;
    m.sound_out(0x40, 14);
    EXPECT_EQ(m.sound_in(0x80), 0x42);
}

TEST(Machine, AyPortBIsGenericKonamiTimer) {
    EXPECT_EQ(galaxian::konami_sound_timer(0), 0x0E);
    scramble::Machine m;
    m.sound.cycles = 0;
    m.sound_out(0x40, 15);
    EXPECT_EQ(m.sound_in(0x80), 0x0E);
}

TEST(Machine, Pal6JOp9IncrementsNibble) {
    scramble::Machine m;
    m.reset();
    m.mem_write(0x8203, 0x80);
    // State shifts: write num1, num2, op. Op $9: result = min(num1+1, 15) << 4.
    m.mem_write(0x8202, 0x03);  // num1
    m.mem_write(0x8202, 0x00);  // num2
    m.mem_write(0x8202, 0x09);  // op
    EXPECT_EQ(m.protection_result, 0x40);  // 4 << 4
    EXPECT_EQ(m.mem_read(0x8202), 0x40);
    // Alt bits 5 and 7 of IN2 follow result bit 7 (clear here).
    EXPECT_EQ(m.mem_read(0x8102) & 0xA0, 0);
}

TEST(Machine, Pal6JOp9SetsAltBitsWhenHigh) {
    scramble::Machine m;
    m.reset();
    m.mem_write(0x8203, 0x80);
    m.mem_write(0x8202, 0x0F);
    m.mem_write(0x8202, 0x00);
    m.mem_write(0x8202, 0x09);  // min(15+1,15)<<4 = 0xF0, bit 7 set
    EXPECT_EQ(m.protection_result, 0xF0);
    EXPECT_EQ(m.mem_read(0x8102) & 0xA0, 0xA0);
}

TEST(Machine, JoystickEchoesToRam) {
    scramble::Machine m;
    m.load_roms(test_set());
    m.reset();
    m.inputs.in0 = 0xDF;
    m.run_cycles(scramble::kCpuHz / 20);
    EXPECT_EQ(m.ram[0x10], 0xDF);
}

TEST(Machine, HwtestVoiceIsAudible) {
    scramble::Machine m;
    m.load_roms(test_set());
    m.reset();
    m.audio.clear();
    m.run_cycles(scramble::kCpuHz * 4);
    ASSERT_FALSE(m.audio.empty());
    bool any = false;
    for (float s : m.audio) {
        if (s != 0.0f) { any = true; break; }
    }
    EXPECT_TRUE(any) << "self-test ROM should beep both AYs after the screens";
    m.audio.clear();
    m.run_cycles(scramble::kCpuHz / 5);
    bool still = false;
    for (float s : m.audio) {
        if (s != 0.0f) { still = true; break; }
    }
    EXPECT_FALSE(still) << "POST beep should end; help screen is silent";
}

TEST(Machine, HwtestHelpScreenShowsCopyrightPrompt) {
    scramble::Machine m;
    m.load_roms(test_set());
    m.reset();
    m.run_cycles(scramble::kCpuHz * 4);
    std::array<uint32_t, scramble::kUprightW * scramble::kUprightH> rgb{};
    m.render(rgb.data());
    int lit = 0, yellow = 0, white = 0;
    for (uint32_t p : rgb) {
        int r = int(p >> 16), g = int(p >> 8) & 0xFF, b = int(p & 0xFF);
        bool bg = (r == 0 && g == 0 && b == int(galaxian::kScrambleBgBlue));
        if ((r | g | b) && !bg) lit++;
        if (r > 180 && g > 180 && b < 40) yellow++;
        if (r > 180 && g > 180 && b > 80) white++;
    }
    EXPECT_GT(yellow, 40);
    EXPECT_GT(white, 80);
    EXPECT_LT(lit, scramble::kUprightW * scramble::kUprightH * 50 / 100);
}

TEST(Machine, FactoryDipIdles) {
    scramble::Inputs in;
    EXPECT_EQ(in.in0, 0xFF);
    EXPECT_EQ(in.in1, 0xFC);
    EXPECT_EQ(in.in2, 0x51);
}

TEST(Machine, SoundRamAt8000) {
    scramble::Machine m;
    m.reset();
    m.sound_write(0x8000, 0x5A);
    EXPECT_EQ(m.sound_read(0x8000), 0x5A);
    EXPECT_EQ(m.sound_read(0x8400), 0x5A);  // mirror (A10)
}
