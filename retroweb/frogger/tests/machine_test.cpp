#include <gtest/gtest.h>

#include "hwtest_roms.h"
#include "machine.h"

#include <algorithm>
#include <array>

namespace {

frogger::RomSet test_set() {
    frogger::RomSet s;
    std::copy(frogger::hwtest::program.begin(), frogger::hwtest::program.end(), s.program.begin());
    std::copy(frogger::hwtest::sound.begin(), frogger::hwtest::sound.end(), s.sound.begin());
    std::copy(frogger::hwtest::gfx.begin(), frogger::hwtest::gfx.end(), s.gfx.begin());
    std::copy(frogger::hwtest::color_prom.begin(), frogger::hwtest::color_prom.end(),
              s.color_prom.begin());
    return s;
}

}  // namespace

TEST(Machine, SwapD0D1RoundTrip) {
    EXPECT_EQ(frogger::swap_d0d1(0x01), 0x02);
    EXPECT_EQ(frogger::swap_d0d1(0x02), 0x01);
    EXPECT_EQ(frogger::swap_d0d1(frogger::swap_d0d1(0xA5)), 0xA5);
}

TEST(Machine, LoadRomsSwapsSoundFirst2kAndGfxSecond2k) {
    frogger::RomSet s;
    s.sound[0] = 0x01;
    s.sound[0x800] = 0x01;
    s.gfx[0] = 0x01;
    s.gfx[0x800] = 0x01;
    frogger::Machine m;
    m.load_roms(s);
    EXPECT_EQ(m.sound_rom[0], 0x02);
    EXPECT_EQ(m.sound_rom[0x800], 0x01);
    EXPECT_EQ(m.video.gfx[0], 0x01);
    EXPECT_EQ(m.video.gfx[0x800], 0x02);
}

TEST(Machine, WatchdogReadReturnsFfAndPreventsExpiry) {
    frogger::Machine m;
    m.reset();
    EXPECT_EQ(m.mem_read(0x8800), 0xFF);
    for (int i = 0; i < 20; i++) {
        m.video.vblank_edge = true;
        // Kick each "frame" the way the NMI handler does.
        (void)m.mem_read(0x8800);
        m.watchdog_--;
        if (m.watchdog_ <= 0) {
            m.watchdog_reset = true;
            break;
        }
    }
    EXPECT_FALSE(m.watchdog_reset);
}

TEST(Machine, WatchdogExpiresAfterEightVblanksWithoutKick) {
    frogger::RomSet s;
    s.program[0] = 0x76;  // HALT
    frogger::Machine m;
    m.load_roms(s);
    m.reset();
    m.run_cycles(frogger::kCpuPerFrame * 10);
    EXPECT_TRUE(m.watchdog_reset);
}

TEST(Machine, WriteB808EnablesNmi) {
    frogger::Machine m;
    m.reset();
    EXPECT_FALSE(m.nmi_enable);
    m.mem_write(0xB808, 1);
    EXPECT_TRUE(m.nmi_enable);
    m.mem_write(0xB808, 0);
    EXPECT_FALSE(m.nmi_enable);
}

TEST(Machine, Ppi0ReadsCabinetPorts) {
    frogger::Machine m;
    m.reset();
    m.mem_write(0xE006, 0x9B);  // all inputs
    m.inputs.in0 = 0xA5;
    m.inputs.in1 = 0x5A;
    m.inputs.in2 = 0x3C;
    EXPECT_EQ(m.mem_read(0xE000), 0xA5);
    EXPECT_EQ(m.mem_read(0xE002), 0x5A);
    EXPECT_EQ(m.mem_read(0xE004), 0x3C);
}

TEST(Machine, Ppi1LatchFallingBit3InterruptsSoundCpu) {
    frogger::Machine m;
    m.reset();
    m.mem_write(0xD006, 0x80);  // PPI1 outputs
    m.sound.im = 1;
    m.sound.iff1 = m.sound.iff2 = true;
    m.sound.sp = 0x43F0;
    m.sound.pc = 0x1234;
    m.mem_write(0xD000, 0x07);
    EXPECT_EQ(m.sound_latch, 0x07);
    m.mem_write(0xD002, 0x08);
    EXPECT_EQ(m.sound.pc, 0x1234);  // still high
    m.mem_write(0xD002, 0x00);      // falling edge
    EXPECT_EQ(m.sound.pc, 0x0038);
}

TEST(Machine, AyIoBit6IsDataBit7IsAddress) {
    frogger::Machine m;
    m.sound_out(0x80, 8);   // address = channel A volume
    m.sound_out(0x40, 0x0F);
    EXPECT_EQ(m.ay.regs[8], 0x0F);
    m.sound_latch = 0x42;
    m.sound_out(0x80, 14);  // address = port A
    EXPECT_EQ(m.sound_in(0x40), 0x42);
}

TEST(Machine, AyPortBIsFroggerSoundTimer) {
    // Generic Konami at t=0 is 0x0E; Frogger swaps bits 3 and 5 → 0x26.
    EXPECT_EQ(frogger::sound_timer_port(0), 0x26);
    frogger::Machine m;
    m.sound.cycles = 0;
    m.sound_out(0x80, 15);
    EXPECT_EQ(m.sound_in(0x40), 0x26);
    // One half-period of the 40960-clock chain is 20480 crystal clocks =
    // 2560 sound T-states; that sets the high bit.
    EXPECT_NE(frogger::sound_timer_port(2560), frogger::sound_timer_port(0));
}

TEST(Machine, FlipLatches) {
    frogger::Machine m;
    m.mem_write(0xB80C, 1);
    m.mem_write(0xB810, 1);
    EXPECT_TRUE(m.video.flip_y);
    EXPECT_TRUE(m.video.flip_x);
}

TEST(Machine, JoystickEchoesToRam) {
    frogger::Machine m;
    m.load_roms(test_set());
    m.reset();
    m.inputs.in0 = 0xDF;  // left pressed
    m.run_cycles(frogger::kCpuHz / 20);
    EXPECT_EQ(m.ram[0x10], 0xDF);
}

TEST(Machine, HwtestStaysSilent) {
    frogger::Machine m;
    m.load_roms(test_set());
    m.reset();
    m.audio.clear();
    m.run_cycles(frogger::kCpuHz * 4);
    ASSERT_FALSE(m.audio.empty());
    bool any = false;
    for (float s : m.audio) if (s != 0.0f) { any = true; break; }
    EXPECT_FALSE(any);
}

TEST(Machine, HwtestHelpScreenShowsCopyrightPrompt) {
    frogger::Machine m;
    m.load_roms(test_set());
    m.reset();
    m.run_cycles(frogger::kCpuHz * 4);
    std::array<uint32_t, frogger::kUprightW * frogger::kUprightH> rgb{};
    m.render(rgb.data());
    int lit = 0, yellow = 0, white = 0;
    for (uint32_t p : rgb) {
        int r = int(p >> 16), g = int(p >> 8) & 0xFF, b = int(p & 0xFF);
        bool river = (r == 0 && g == 0 && b == int(frogger::kRiverBlue));
        if ((r | g | b) && !river) lit++;
        if (r > 180 && g > 180 && b < 40) yellow++;
        if (r > 180 && g > 180 && b > 180) white++;
    }
    EXPECT_GT(yellow, 40);
    EXPECT_GT(white, 80);
    EXPECT_LT(lit, frogger::kUprightW * frogger::kUprightH * 35 / 100);
}

TEST(Machine, FactoryDipIdles) {
    frogger::Inputs in;
    EXPECT_EQ(in.in0, 0xFF);
    EXPECT_EQ(in.in1, 0xFC);  // 3 lives
    EXPECT_EQ(in.in2, 0xF1);  // 1C/1C upright
}
