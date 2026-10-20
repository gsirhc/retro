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

// NOISE chopped as a square wave at `hz`.
std::vector<float> run_chopped(galaxian::DiscreteSound& s, double seconds, double hz) {
    std::vector<float> out;
    int n = int(seconds * 48000);
    for (int k = 0; k < n; k++) {
        s.set_noise(std::fmod(k / 48000.0 * hz, 1.0) < 0.5);
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

double ramp_hz(uint8_t bits) {
    galaxian::DiscreteSound s;
    s.reset();
    for (int b = 0; b < 4; b++) s.lfo_freq_w(b, (bits >> b) & 1);
    double prev = 0;
    int drops = 0;
    double first = -1, last = 0;
    for (int k = 0; k < 3072000 * 4 / 64; k++) {
        s.advance(64);
        double v = s.ramp_volts();
        if (v < prev - 0.5) {
            double t = k * 64.0 / 3072000.0;
            if (first < 0) first = t;
            last = t;
            drops++;
        }
        prev = v;
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
    // Latches low: Q1 node 3.40 V, I1 = 66.8 uA, Q2 mirrors 10.5 uA into C15 across 1/3 VCC.
    double f0 = ramp_hz(0x0);
    double f15 = ramp_hz(0xF);
    EXPECT_NEAR(f0, 6.30, 0.1);
    EXPECT_LT(f15, f0 / 2);
}

TEST(DiscreteSound, BackgroundCvStaysWithinTheOpAmpSwing) {
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
    // 7T gain 1 + 10k/47k + 10k/47k less 5 V x 10k/47k, from 1/3 VCC up to the LM324's VCC - 1.5 V.
    EXPECT_NEAR(lo, 5.0 / 3 * (1 + 20.0 / 47) - 50.0 / 47, 0.02);
    EXPECT_NEAR(hi, 3.5, 0.01);
}

TEST(DiscreteSound, FsVoicesAreAudible) {
    galaxian::DiscreteSound s;
    s.reset();
    s.sound_w(0, true);
    auto out = run_sound(s, 0.5);
    EXPECT_GT(rms(out, out.size() / 2), 0.05);
}

TEST(DiscreteSound, LatchQ4DoesNothing) {
    galaxian::DiscreteSound s;
    s.reset();
    s.sound_w(4, true);
    auto out = run_sound(s, 0.3);
    EXPECT_LT(rms(out, out.size() / 2), 1e-3);
}

TEST(DiscreteSound, PitchToneRunsWithoutFire) {
    galaxian::DiscreteSound s;
    s.reset();
    s.pitch_w(0x40);
    auto out = run_sound(s, 0.3);
    EXPECT_GT(rms(out, out.size() / 2), 0.05);
}

TEST(DiscreteSound, PitchFFHoldsTheCounterAndIsSilent) {
    galaxian::DiscreteSound s;
    s.reset();
    s.pitch_w(0xFF);
    auto out = run_sound(s, 0.5);
    EXPECT_LT(rms(out, out.size() / 2), 1e-3);
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
    EXPECT_GT(rms(soon), 0.05) << "C25 into R41 keeps the shot going after FIRE clears";
    run_sound(s, 1.5);
    auto late = run_sound(s, 0.1);
    EXPECT_LT(rms(late), rms(soon) * 0.01);
}

TEST(DiscreteSound, FireCvFollowsR47C28) {
    galaxian::DiscreteSound s;
    s.reset();
    run_sound(s, 1.5);
    // CV node: R48 from C28, R46 from NOISE high, the 555's 5k/5k/5k divider.
    EXPECT_NEAR(s.fire_cv(), 3.368, 0.01);
    s.sound_w(5, true);
    run_sound(s, 1.5);
    EXPECT_NEAR(s.fire_cv(), 2.209, 0.01);
    s.sound_w(5, false);
    run_sound(s, 0.05);
    EXPECT_LT(s.fire_cv(), 3.0) << "47 uF behind 2.2k recovers over about 0.1 s";
    run_sound(s, 1.5);
    EXPECT_NEAR(s.fire_cv(), 3.368, 0.01);
}

TEST(DiscreteSound, HitNeedsNoiseToReachTheBandPass) {
    galaxian::DiscreteSound s;
    s.reset();
    s.set_noise(false);
    s.sound_w(3, true);
    auto out = run_sound(s, 0.3);
    EXPECT_LT(rms(out, out.size() / 2), 1e-3);
}

TEST(DiscreteSound, HitIsAudibleAndDecays) {
    galaxian::DiscreteSound s;
    s.reset();
    s.sound_w(3, true);
    auto on = run_chopped(s, 0.2, 168.0);
    EXPECT_GT(rms(on, on.size() / 2), 0.05);
    s.sound_w(3, false);
    run_chopped(s, 3.0, 168.0);
    auto off = run_chopped(s, 0.2, 168.0);
    EXPECT_LT(rms(off), rms(on, on.size() / 2) * 0.1);
}

TEST(DiscreteSound, HitBandPassCentresNear168Hz) {
    // f0 = sqrt((1/150.5k + 1/22k) / (470k x 0.01 uF x 0.01 uF)) / 2 pi = 167.6 Hz.
    auto level = [](double hz) {
        galaxian::DiscreteSound s;
        s.reset();
        s.sound_w(3, true);
        auto out = run_chopped(s, 0.3, hz);
        return rms(out, out.size() / 2);
    };
    const double centre = level(168.0);
    EXPECT_GT(centre, level(40.0) * 2);
    EXPECT_GT(centre, level(1000.0) * 2);
}
