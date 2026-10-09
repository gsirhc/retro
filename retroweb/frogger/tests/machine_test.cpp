#include <gtest/gtest.h>

#include "hwtest_roms.h"
#include "machine.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <vector>

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

TEST(Machine, WatchdogResetKeepsRam) {
    frogger::RomSet s;
    s.program[0] = 0x76;  // HALT
    frogger::Machine m;
    m.load_roms(s);
    m.reset();
    m.ram[0x10] = 0xA5;
    m.video.videoram[5] = 0x5A;
    m.run_cycles(frogger::kCpuPerFrame * 10);
    ASSERT_TRUE(m.watchdog_reset);
    EXPECT_EQ(m.ram[0x10], 0xA5);
    EXPECT_EQ(m.video.videoram[5], 0x5A);
    EXPECT_EQ(m.frames, 0);
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

namespace {

// Counts NMIs at $8000; `rearm` writes 0 then 1 to $B808 inside the handler.
frogger::RomSet nmi_counter(bool rearm) {
    frogger::RomSet s;
    const uint8_t main[] = {0x31, 0x00, 0x88, 0x21, 0x00, 0x80, 0x3E, 0x01,
                            0x32, 0x08, 0xB8, 0x3A, 0x00, 0x88, 0x18, 0xFB};
    std::copy(std::begin(main), std::end(main), s.program.begin());
    std::vector<uint8_t> nmi = {0xF5, 0x34};
    if (rearm) nmi.insert(nmi.end(), {0xAF, 0x32, 0x08, 0xB8, 0x3C, 0x32, 0x08, 0xB8});
    nmi.insert(nmi.end(), {0xF1, 0xED, 0x45});
    std::copy(nmi.begin(), nmi.end(), s.program.begin() + 0x66);
    return s;
}

}  // namespace

TEST(Machine, VblankNmiHoldsUntilTheEnableLatchIsCleared) {
    frogger::Machine m;
    m.load_roms(nmi_counter(false));
    m.reset();
    m.run_cycles(frogger::kCpuPerFrame * 6);
    EXPECT_EQ(m.ram[0], 1);
}

TEST(Machine, VblankNmiFiresEveryFrameWhenRearmed) {
    frogger::Machine m;
    m.load_roms(nmi_counter(true));
    m.reset();
    m.run_cycles(frogger::kCpuPerFrame * 6);
    EXPECT_EQ(m.ram[0], 6);
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
    EXPECT_TRUE(m.sound.int_line);
    m.sound.step();
    EXPECT_EQ(m.sound.pc, 0x0038);
    EXPECT_FALSE(m.sound.int_line);
}

TEST(Machine, SoundIntHoldsUntilTheCpuAcknowledges) {
    frogger::Machine m;
    m.reset();
    m.mem_write(0xD006, 0x80);
    m.sound.im = 1;
    m.sound.sp = 0x43F0;
    m.mem_write(0xD002, 0x08);
    m.mem_write(0xD002, 0x00);
    for (int i = 0; i < 10; i++) m.sound.step();
    EXPECT_TRUE(m.sound.int_line);
    m.sound.iff1 = m.sound.iff2 = true;
    m.sound.step();
    EXPECT_EQ(m.sound.pc, 0x0038);
    EXPECT_FALSE(m.sound.int_line);
}

TEST(Machine, FilterLatchDecodesAddressLinesAt6000) {
    frogger::Machine m;
    m.reset();
    m.sound_write(0x6000 | (0x3F << 6), 0);
    for (int n = 0; n < 6; n++) EXPECT_TRUE(m.konami.filter_switch(n));
    m.sound_write(0x7000 | (1 << 7), 0);
    EXPECT_FALSE(m.konami.filter_switch(0));
    EXPECT_TRUE(m.konami.filter_switch(1));
}

TEST(Machine, AyIoBit6IsDataBit7IsAddress) {
    frogger::Machine m;
    m.sound_out(0x80, 8);   // address = channel A volume
    m.sound_out(0x40, 0x0F);
    EXPECT_EQ(m.ay.regs[8], 0x0F);
    m.sound_latch = 0x42;
    m.sound_out(0x80, 14);  // address = port A
    EXPECT_EQ(m.sound_in(0x40), 0x42);
    EXPECT_EQ(m.sound_in(0x80), 0xFF) << "only bit 6 reads the AY";
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
    for (float s : m.audio) if (s > 1e-3f || s < -1e-3f) { any = true; break; }
    EXPECT_FALSE(any);
}

TEST(Machine, SoundCpuTracksMainClock) {
    // A sync that discards instruction overshoot runs the sound CPU ~2x fast.
    frogger::Machine m;
    m.reset();
    const int main_run = frogger::kCpuHz / 10;
    const uint64_t s0 = m.sound.cycles;
    m.run_cycles(main_run);
    const uint64_t got = m.sound.cycles - s0;
    const uint64_t expect = uint64_t(main_run) * frogger::kSoundHz / frogger::kCpuHz;
    EXPECT_NEAR(double(got), double(expect), double(expect) * 0.02);
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

TEST(Machine, CoinCounterCountsRisingEdges) {
    frogger::Machine m;
    m.reset();
    m.mem_write(0xB818, 1);
    m.mem_write(0xB818, 1);
    m.mem_write(0xB818, 0);
    m.mem_write(0xB818, 1);
    EXPECT_EQ(m.coin_counter[0], 2);
    m.mem_write(0xB81C, 1);
    EXPECT_EQ(m.coin_counter[1], 1);
}
