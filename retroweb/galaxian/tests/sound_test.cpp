#include <gtest/gtest.h>

#include "sound.h"

#include <algorithm>
#include <cmath>
#include <vector>

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

TEST(DiscreteSound, Vol2AddsR52ToThePitchMix) {
    galaxian::DiscreteSound a, b;
    a.reset();
    b.reset();
    a.pitch_w(0x80);
    b.pitch_w(0x80);
    b.sound_w(7, true);
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
