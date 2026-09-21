#include <gtest/gtest.h>

#include "galaxian/video.h"
#include "sound.h"

#include <array>
#include <vector>

TEST(Video, GalaxianHasNoRiverOrScrambleBackdrop) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    v.background_enable = true;
    v.stars_enable = false;
    std::array<uint32_t, galaxian::kUprightW * galaxian::kUprightH> out{};
    v.render(out.data());
    EXPECT_EQ(out[0] & 0x00FFFFFF, 0u);
    EXPECT_EQ(out[200 * galaxian::kUprightW + 0] & 0x00FFFFFF, 0u);
}

TEST(Video, GalaxianScrollDoesNotNibbleSwap) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    constexpr int col = 25;
    v.videoram[2 * 32 + col] = 2;
    v.objram[col * 2] = 0x01;
    v.objram[col * 2 + 1] = 0;
    v.gfx[0x800 + 2 * 8 + 1] = 0x80;
    v.color_prom[1] = 0x07;
    std::array<uint32_t, galaxian::kUprightW * galaxian::kUprightH> out{};
    v.render(out.data());
    EXPECT_EQ(out[200 * galaxian::kUprightW + 223] & 0xFF0000, 0xFF0000u);
}

TEST(Video, GalaxianStarsEnableLightsPixels) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    v.stars_enable = true;
    std::array<uint32_t, galaxian::kUprightW * galaxian::kUprightH> out{};
    v.render(out.data());
    int lit = 0;
    for (uint32_t p : out) {
        if (p & 0x00FFFFFF) lit++;
    }
    EXPECT_GT(lit, 8);
}

TEST(Video, GalaxianBlueUses470And220Ohm) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    v.color_prom[0] = 0xC0;  // PROM bits 6 and 7
    uint32_t rgb = v.prom_rgb(0);
    EXPECT_EQ((rgb >> 16) & 0xFF, 0);
    EXPECT_EQ((rgb >> 8) & 0xFF, 0);
    EXPECT_EQ(rgb & 0xFF, 0x47 + 0x97);
}

TEST(Video, GalaxianBulletsAreFourPixels) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    v.objram[0x60 + 3 * 4 + 1] = 0xDF;
    v.objram[0x60 + 3 * 4 + 3] = 100;
    std::array<uint32_t, galaxian::kUprightW * galaxian::kUprightH> out{};
    v.render(out.data());
    int yellow = 0;
    for (uint32_t p : out) {
        if ((p & 0x00FFFFFF) == 0xFFFF00) yellow++;
    }
    EXPECT_EQ(yellow, 4);
}

TEST(DiscreteSound, FireLatchIsAudible) {
    galaxian::DiscreteSound s;
    s.reset();
    s.sound_w(5, true);
    s.pitch_w(0xFF);
    std::vector<float> audio;
    for (int i = 0; i < 3072000 / 20; i += 64) {
        s.advance(64);
        audio.push_back(s.mix());
    }
    bool any = false;
    for (float v : audio) {
        if (v != 0.0f) { any = true; break; }
    }
    EXPECT_TRUE(any);
}

TEST(DiscreteSound, PitchToneRunsWithoutFire) {
    galaxian::DiscreteSound s;
    s.reset();
    s.pitch_w(0x40);
    bool any = false;
    for (int i = 0; i < 3072000 / 40; i += 64) {
        s.advance(64);
        if (s.mix() != 0.0f) { any = true; break; }
    }
    EXPECT_TRUE(any) << "LS164 pitch is mixed even when FIRE is off";
}

TEST(DiscreteSound, PitchFFIsSilent) {
    galaxian::DiscreteSound s;
    s.reset();
    s.pitch_w(0xFF);
    bool any = false;
    for (int i = 0; i < 3072000 / 40; i += 64) {
        s.advance(64);
        if (s.mix() != 0.0f) { any = true; break; }
    }
    EXPECT_FALSE(any) << "pitch 0xFF is ultrasonic and must not alias";
}

TEST(DiscreteSound, PitchToneLastsAfterFireDrops) {
    galaxian::DiscreteSound s;
    s.reset();
    s.pitch_w(0x40);
    s.sound_w(5, true);
    s.advance(3072);
    s.sound_w(5, false);
    // R41·C25 (~0.1 s) is long gone after 0.5 s; the LS164 tone stays.
    for (int i = 0; i < 3072000 / 2; i += 64) s.advance(64);
    bool any = false;
    for (int i = 0; i < 3072000 / 20; i += 64) {
        s.advance(64);
        if (s.mix() != 0.0f) { any = true; break; }
    }
    EXPECT_TRUE(any) << "bomber whistle is pitch, not the FIRE shot";
}

TEST(DiscreteSound, FireDecaysAfterLatchDrops) {
    galaxian::DiscreteSound s;
    s.reset();
    s.pitch_w(0xFF);
    s.sound_w(5, true);
    s.advance(3072);
    s.sound_w(5, false);
    bool mid = false;
    for (int i = 0; i < 3072000 / 20; i += 64) {
        s.advance(64);
        if (s.mix() != 0.0f) { mid = true; break; }
    }
    EXPECT_TRUE(mid) << "R41·C25 keeps the shot going after FIRE clears";
}
