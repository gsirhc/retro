#include <gtest/gtest.h>

#include "galaxian/video.h"
#include "sound.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

TEST(Video, GalaxianHasNoRiverOrScrambleBackdrop) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    v.background_enable = true;
    v.set_stars_enable(false);
    std::array<uint32_t, galaxian::kUprightW * galaxian::kUprightH> out{};
    v.advance(galaxian::kCpuPerFrame);
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
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    EXPECT_EQ(out[200 * galaxian::kUprightW + 223] & 0xFF0000, 0xE00000u);
}

TEST(Video, GalaxianStarsEnableLightsPixels) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    v.set_stars_enable(true);
    std::array<uint32_t, galaxian::kUprightW * galaxian::kUprightH> out{};
    v.advance(galaxian::kCpuPerFrame);
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
    EXPECT_EQ(rgb & 0xFF, 217);
}

TEST(Video, GalaxianShellsAreFourWhitePixels) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    v.objram[0x60 + 3 * 4 + 1] = 0xDF;
    v.objram[0x60 + 3 * 4 + 3] = 100;
    std::array<uint32_t, galaxian::kUprightW * galaxian::kUprightH> out{};
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    int white = 0;
    for (uint32_t p : out) {
        if ((p & 0x00FFFFFF) == 0xFFFFFF) white++;
    }
    EXPECT_EQ(white, 4);
}

namespace {

using Frame = std::array<uint32_t, galaxian::kUprightW * galaxian::kUprightH>;

uint32_t native_at(const Frame& out, int x, int line) {
    return out[unsigned(x * galaxian::kUprightW + (galaxian::kVisH - 1) - (line - galaxian::kVisY0))] & 0x00FFFFFF;
}

void solid_sprite(galaxian::Video& v, int code) {
    for (int i = 0; i < 32; i++) {
        v.gfx[unsigned(code * 32 + i)] = 0xFF;
        v.gfx[unsigned(0x800 + code * 32 + i)] = 0xFF;
    }
}

void place_sprite(galaxian::Video& v, int n, uint8_t y, uint8_t x, uint8_t color) {
    v.objram[unsigned(0x40 + n * 4 + 0)] = y;
    v.objram[unsigned(0x40 + n * 4 + 1)] = 1;
    v.objram[unsigned(0x40 + n * 4 + 2)] = color;
    v.objram[unsigned(0x40 + n * 4 + 3)] = x;
}

}  // namespace

TEST(Video, LowerNumberedSpriteWins) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    solid_sprite(v, 1);
    v.color_prom[7] = 0x07;
    v.color_prom[11] = 0x38;
    place_sprite(v, 0, 140, 100, 1);
    place_sprite(v, 1, 140, 100, 2);
    Frame out{};
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    EXPECT_EQ(native_at(out, 110, 105), v.prom_rgb(7));
}

TEST(Video, FirstThreeSpritesMatchAgainstLineMinusOne) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    solid_sprite(v, 1);
    v.color_prom[7] = 0x07;
    place_sprite(v, 0, 140, 60, 1);
    place_sprite(v, 3, 140, 160, 1);
    Frame out{};
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    EXPECT_EQ(native_at(out, 70, 100), 0u);
    EXPECT_EQ(native_at(out, 70, 116), v.prom_rgb(7));
    EXPECT_EQ(native_at(out, 170, 100), v.prom_rgb(7));
    EXPECT_EQ(native_at(out, 170, 116), 0u);
}

TEST(Video, SpriteLineBufferClipsFirstSixteenPixels) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    solid_sprite(v, 1);
    v.color_prom[7] = 0x07;
    place_sprite(v, 3, 140, 8, 1);
    Frame out{};
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    int lit = 0;
    for (uint32_t p : out)
        if ((p & 0x00FFFFFF) == v.prom_rgb(7)) lit++;
    EXPECT_EQ(lit, 8 * 16);
    EXPECT_EQ(native_at(out, 16, 105), 0u);
    EXPECT_EQ(native_at(out, 17, 105), v.prom_rgb(7));
}

TEST(Video, GalaxianStarFieldDriftsOneStepPerFrame) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    v.set_stars_enable(true);
    ASSERT_EQ(v.star_origin, 0u);
    v.advance(galaxian::kCpuPerFrame);
    EXPECT_EQ(v.star_origin, uint32_t(galaxian::kStarPeriod - 1));
    v.flip_x = true;
    v.advance(galaxian::kCpuPerFrame);
    v.advance(galaxian::kCpuPerFrame);
    EXPECT_EQ(v.star_origin, 1u);
}

TEST(Video, StarsEnableRestartsTheFieldAtTheBeam) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    v.set_stars_enable(true);
    v.advance(galaxian::kCpuPerFrame * 5);
    v.set_stars_enable(false);
    v.set_stars_enable(true);
    EXPECT_EQ(v.star_origin, 0u);
}

TEST(Video, OneShellPerLineLastMatchWins) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    v.objram[0x60 + 3 * 4 + 1] = 0xDF;
    v.objram[0x60 + 3 * 4 + 3] = 100;
    v.objram[0x60 + 4 * 4 + 1] = 0xDF;
    v.objram[0x60 + 4 * 4 + 3] = 50;
    Frame out{};
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    int white = 0;
    for (uint32_t p : out)
        if ((p & 0x00FFFFFF) == 0xFFFFFF) white++;
    EXPECT_EQ(white, 4);
    for (int x = 201; x < 205; x++) EXPECT_EQ(native_at(out, x, 32), 0xFFFFFFu);
}

TEST(Video, GalaxianMissileIsFourYellowPixels) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    v.objram[0x60 + 7 * 4 + 1] = 0xDF;
    v.objram[0x60 + 7 * 4 + 3] = 100;
    Frame out{};
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    int yellow = 0;
    for (uint32_t p : out)
        if ((p & 0x00FFFFFF) == 0xFFFF00) yellow++;
    EXPECT_EQ(yellow, 4);
}

TEST(Video, ShellsOrIntoTheTileColour) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    v.videoram[4 * 32 + 19] = 2;
    for (int r = 0; r < 8; r++) v.gfx[unsigned(0x800 + 2 * 8 + r)] = 0xFF;
    v.objram[19 * 2 + 1] = 0;
    v.color_prom[1] = 0x38;
    v.objram[0x60 + 7 * 4 + 1] = 0xFF - 35;
    v.objram[0x60 + 7 * 4 + 3] = 255 - 156;
    Frame out{};
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    EXPECT_EQ(native_at(out, 153, 35), 0xFFFF00u);
    EXPECT_EQ(native_at(out, 158, 34), v.prom_rgb(1));
}

TEST(Video, FlipXMirrorsTheTilemap) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    v.videoram[4 * 32 + 0] = 2;
    for (int r = 0; r < 8; r++) v.gfx[unsigned(0x800 + 2 * 8 + r)] = 0xFF;
    v.color_prom[1] = 0x07;
    Frame out{};
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    EXPECT_EQ(native_at(out, 3, 35), v.prom_rgb(1));
    v.flip_x = true;
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    EXPECT_EQ(native_at(out, 3, 35), 0u);
    EXPECT_EQ(native_at(out, 252, 35), v.prom_rgb(1));
}

TEST(Video, FlipYMirrorsTheTilemap) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    v.videoram[4 * 32 + 0] = 2;
    for (int r = 0; r < 8; r++) v.gfx[unsigned(0x800 + 2 * 8 + r)] = 0xFF;
    v.color_prom[1] = 0x07;
    v.flip_y = true;
    Frame out{};
    v.advance(galaxian::kCpuPerFrame);
    v.render(out.data());
    EXPECT_EQ(native_at(out, 3, 35), 0u);
    EXPECT_EQ(native_at(out, 3, 220), v.prom_rgb(1));
}

TEST(Video, MidFrameWritesTakeEffectAtTheBeam) {
    galaxian::Video v;
    v.board = galaxian::Board::Galaxian;
    for (int row = 0; row < 32; row++) v.videoram[unsigned(row * 32)] = 2;
    for (int r = 0; r < 8; r++) v.gfx[unsigned(0x800 + 2 * 8 + r)] = 0xFF;
    v.color_prom[1] = 0x07;
    v.advance(128 * galaxian::kCpuPerLine);
    v.flip_x = true;
    v.advance(galaxian::kCpuPerFrame - 128 * galaxian::kCpuPerLine);
    Frame out{};
    v.render(out.data());
    EXPECT_EQ(native_at(out, 3, 40), v.prom_rgb(1));
    EXPECT_EQ(native_at(out, 252, 40), 0u);
    EXPECT_EQ(native_at(out, 3, 200), 0u);
    EXPECT_EQ(native_at(out, 252, 200), v.prom_rgb(1));
}

TEST(Video, VblankEdgeFiresOncePerFrameAtLine240) {
    galaxian::Video v;
    v.advance(galaxian::kVBlankLine * galaxian::kCpuPerLine - 1);
    EXPECT_FALSE(v.vblank_edge);
    v.advance(1);
    EXPECT_TRUE(v.vblank_edge);
    EXPECT_TRUE(v.vblank);
    v.vblank_edge = false;
    v.advance(galaxian::kCpuPerFrame - galaxian::kVBlankLine * galaxian::kCpuPerLine);
    EXPECT_FALSE(v.vblank_edge);
    EXPECT_FALSE(v.vblank);
}

namespace {

std::vector<float> run_sound(galaxian::DiscreteSound& s, double seconds) {
    std::vector<float> out;
    int n = int(seconds * 48000);
    for (int k = 0; k < n; k++) {
        s.advance(64);
        out.push_back(s.mix());
    }
    return out;
}

double rms(const std::vector<float>& v, size_t from = 0) {
    double sum = 0;
    for (size_t i = from; i < v.size(); i++) sum += double(v[i]) * v[i];
    return v.size() > from ? std::sqrt(sum / double(v.size() - from)) : 0.0;
}

double background_hz(uint8_t bits) {
    galaxian::DiscreteSound s;
    s.reset();
    for (int b = 0; b < 4; b++) s.lfo_freq_w(b, (bits >> b) & 1);
    double prev = 0;
    int drops = 0;
    double first = -1, last = 0;
    for (int k = 0; k < 3072000 * 4 / 64; k++) {
        s.advance(64);
        double cv = s.background_cv();
        if (cv < prev - 0.5) {
            double t = k * 64.0 / 3072000.0;
            if (first < 0) first = t;
            last = t;
            drops++;
        }
        prev = cv;
    }
    return drops > 1 ? (drops - 1) / (last - first) : 0.0;
}

}  // namespace

TEST(DiscreteSound, IdleIsSilent) {
    galaxian::DiscreteSound s;
    s.reset();
    auto out = run_sound(s, 0.5);
    EXPECT_LT(rms(out, out.size() / 2), 1e-3);
}

TEST(DiscreteSound, BackgroundRampSlowsAsTheDacRises) {
    double f0 = background_hz(0x0);
    double f15 = background_hz(0xF);
    // C15 charged by (5 - 0.7 - Vdac)/R21 across 1/3 Vcc: 5.6 Hz with the ladder at rest.
    EXPECT_NEAR(f0, 5.65, 0.3);
    EXPECT_LT(f15, f0 / 3);
}

TEST(DiscreteSound, BackgroundCvStaysWithinTheOpAmpClamp) {
    galaxian::DiscreteSound s;
    s.reset();
    double lo = 9, hi = -9;
    for (int k = 0; k < 3072000 * 2 / 64; k++) {
        s.advance(64);
        if (k > 3072000 / 64) {
            lo = std::min(lo, s.background_cv());
            hi = std::max(hi, s.background_cv());
        }
    }
    EXPECT_NEAR(lo, 5.0 / 3 * 1.4255 - 1.0638, 0.05);
    EXPECT_NEAR(hi, 10.0 / 3 * 1.4255 - 1.0638, 0.05);
}

TEST(DiscreteSound, FsVoicesAreAudible) {
    galaxian::DiscreteSound s;
    s.reset();
    s.sound_w(0, true);
    auto out = run_sound(s, 0.5);
    EXPECT_GT(rms(out, out.size() / 2), 0.02);
}

TEST(DiscreteSound, PitchToneRunsWithoutFire) {
    galaxian::DiscreteSound s;
    s.reset();
    s.pitch_w(0x40);
    auto out = run_sound(s, 0.3);
    EXPECT_GT(rms(out, out.size() / 2), 0.05);
}

TEST(DiscreteSound, PitchFFIsUltrasonicAndCouplesOut) {
    galaxian::DiscreteSound s;
    s.reset();
    s.pitch_w(0xFF);
    auto out = run_sound(s, 0.5);
    EXPECT_LT(rms(out, out.size() / 2), 2e-3);
}

TEST(DiscreteSound, Vol1AddsR49ToThePitchMix) {
    galaxian::DiscreteSound a, b;
    a.reset();
    b.reset();
    a.pitch_w(0x80);
    b.pitch_w(0x80);
    b.sound_w(6, true);
    auto oa = run_sound(a, 0.3);
    auto ob = run_sound(b, 0.3);
    EXPECT_GT(rms(ob, ob.size() / 2), rms(oa, oa.size() / 2) * 1.2);
}

TEST(DiscreteSound, FireLatchIsAudible) {
    galaxian::DiscreteSound s;
    s.reset();
    s.sound_w(5, true);
    auto out = run_sound(s, 0.1);
    EXPECT_GT(rms(out), 0.05);
}

TEST(DiscreteSound, FireDecaysAfterLatchDrops) {
    galaxian::DiscreteSound s;
    s.reset();
    s.sound_w(5, true);
    run_sound(s, 0.05);
    s.sound_w(5, false);
    auto soon = run_sound(s, 0.03);
    EXPECT_GT(rms(soon), 0.02) << "R41/C25 keeps the shot going after FIRE clears";
    run_sound(s, 1.5);
    auto late = run_sound(s, 0.1);
    EXPECT_LT(rms(late), rms(soon) * 0.1);
}

TEST(DiscreteSound, FireCvRecoversThroughR47C28) {
    galaxian::DiscreteSound s;
    s.reset();
    run_sound(s, 1.0);
    s.sound_w(5, true);
    run_sound(s, 1.0);
    double firing = s.fire_cv();
    s.sound_w(5, false);
    run_sound(s, 1.0);
    EXPECT_LT(firing, 1.0);
    EXPECT_GT(s.fire_cv(), 3.0);
}

TEST(DiscreteSound, NoiseLatchToggles) {
    galaxian::DiscreteSound s;
    s.reset();
    int flips = 0;
    bool prev = s.noise();
    for (int k = 0; k < 3072000 / 10 / 64; k++) {
        s.advance(64);
        if (s.noise() != prev) flips++;
        prev = s.noise();
    }
    // Sampled once per two lines, 800 latches in 0.1 s; a random bit changes about half the time.
    EXPECT_GT(flips, 250);
    EXPECT_LT(flips, 550);
}

TEST(DiscreteSound, HitIsAudibleAndDecays) {
    galaxian::DiscreteSound s;
    s.reset();
    s.sound_w(3, true);
    auto on = run_sound(s, 0.2);
    EXPECT_GT(rms(on, on.size() / 2), 0.02);
    s.sound_w(3, false);
    run_sound(s, 3.0);
    auto off = run_sound(s, 0.2);
    EXPECT_LT(rms(off), rms(on, on.size() / 2) * 0.1);
}
