#include <gtest/gtest.h>

#include "galaxian/ay8910.h"
#include "galaxian/i8255.h"
#include "galaxian/timer.h"
#include "galaxian/video.h"

#include <array>
#include <vector>

TEST(I8255, ResetLeavesPortsAsInputs) {
    galaxian::I8255 p;
    p.reset();
    p.in_a = [] { return uint8_t(0xA5); };
    EXPECT_TRUE(p.a_in);
    EXPECT_EQ(p.read(0), 0xA5);
}

TEST(Ay8910, ToneAIsAudibleWhenEnabled) {
    galaxian::Ay8910 ay;
    ay.reset();
    ay.write_addr(0);
    ay.write_data(0x20);
    ay.write_addr(1);
    ay.write_data(0x00);
    ay.write_addr(7);
    ay.write_data(0x38);
    ay.write_addr(8);
    ay.write_data(0x0F);
    std::vector<float> audio;
    ay.advance(1789772 / 20, 48000, audio);
    ASSERT_FALSE(audio.empty());
    bool any = false;
    for (float s : audio) {
        if (s != 0.0f) { any = true; break; }
    }
    EXPECT_TRUE(any);
}

TEST(Timer, GenericKonamiDoesNotSwapBits35) {
    // Generic Konami at t=0 is 0x0E; Frogger would swap that to 0x26.
    EXPECT_EQ(galaxian::konami_sound_timer(0), 0x0E);
    EXPECT_NE(galaxian::konami_sound_timer(2560), galaxian::konami_sound_timer(0));
}

TEST(Video, ScrambleBackdropIsBlueWhenEnabled) {
    galaxian::Video v;
    v.board = galaxian::Board::Scramble;
    v.background_enable = true;
    v.stars_enable = false;
    std::array<uint32_t, galaxian::kUprightW * galaxian::kUprightH> out{};
    v.render(out.data());
    EXPECT_EQ(out[0] & 0x00FFFFFF, galaxian::kScrambleBgBlue);
}

TEST(Video, ScrambleHasNoRiverSplit) {
    galaxian::Video v;
    v.board = galaxian::Board::Scramble;
    std::array<uint32_t, galaxian::kUprightW * galaxian::kUprightH> out{};
    v.render(out.data());
    EXPECT_EQ(out[0 * galaxian::kUprightW + 0] & 0x00FFFFFF, 0u);
    EXPECT_EQ(out[200 * galaxian::kUprightW + 0] & 0x00FFFFFF, 0u);
}

TEST(Video, ScrambleScrollDoesNotNibbleSwap) {
    galaxian::Video v;
    v.board = galaxian::Board::Scramble;
    constexpr int col = 25;
    v.videoram[2 * 32 + col] = 2;
    v.objram[col * 2] = 0x01;  // no nibble-swap: scroll 1
    v.objram[col * 2 + 1] = 0;
    v.gfx[0x800 + 2 * 8 + 1] = 0x80;
    v.color_prom[1] = 0x07;
    std::array<uint32_t, galaxian::kUprightW * galaxian::kUprightH> out{};
    v.render(out.data());
    EXPECT_EQ(out[200 * galaxian::kUprightW + 223] & 0xFF0000, 0xFF0000u);
}

TEST(Video, StarsEnableLightsPixels) {
    galaxian::Video v;
    v.board = galaxian::Board::Scramble;
    v.stars_enable = true;
    v.stars_blink_state = 3;  // mask 0xFF, no 2V skip
    std::array<uint32_t, galaxian::kUprightW * galaxian::kUprightH> out{};
    v.render(out.data());
    int lit = 0;
    for (uint32_t p : out) {
        if (p & 0x00FFFFFF) lit++;
    }
    EXPECT_GT(lit, 8);
}

TEST(Video, YellowShellsDrawAtMatchY) {
    galaxian::Video v;
    v.board = galaxian::Board::Scramble;
    // Bullet 3 matches V. Y such that y + obj[1] = 0xFF for native y = vis+16.
    // vis y=16 → native 32. obj[1] = 0xFF - 32 = 0xDF. X via 255 - obj[3] - 4.
    v.objram[0x60 + 3 * 4 + 1] = 0xDF;
    v.objram[0x60 + 3 * 4 + 3] = 100;
    std::array<uint32_t, galaxian::kUprightW * galaxian::kUprightH> out{};
    v.render(out.data());
    int yellow = 0;
    for (uint32_t p : out) {
        if ((p & 0x00FFFFFF) == 0xFFFF00) yellow++;
    }
    EXPECT_GE(yellow, 1);
}
