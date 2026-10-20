#include <gtest/gtest.h>

#include "sound.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr int kHz = 48000;

asteroids::Sound fresh() {
    asteroids::Sound s;
    s.reset();
    s.set_rate(kHz);
    return s;
}

void run(asteroids::Sound& s, double seconds) {
    for (int i = 0; i < int(seconds * kHz); i++) s.sample();
}

template <typename F>
std::vector<double> trace(asteroids::Sound& s, double seconds, F node) {
    std::vector<double> v;
    for (int i = 0; i < int(seconds * kHz); i++) {
        s.sample();
        v.push_back(node(s));
    }
    return v;
}

// Rising crossings of the trace's mean, per second.
double rate(const std::vector<double>& v) {
    double mean = 0;
    for (double x : v) mean += x;
    mean /= double(v.size());
    int n = 0;
    for (size_t i = 1; i < v.size(); i++) n += v[i - 1] <= mean && v[i] > mean;
    return n * double(kHz) / double(v.size());
}

double spread(const std::vector<double>& v) {
    auto [lo, hi] = std::minmax_element(v.begin(), v.end());
    return *hi - *lo;
}

double mix(const asteroids::Sound& s) { return s.mix_out(); }
double saucer(const asteroids::Sound& s) { return s.saucer_hz(); }
double expff(const asteroids::Sound& s) { return s.explosion_ff() ? 1 : 0; }

}  // namespace

// Q2 sources about 77 uA into C33 at nibble 0, about 30 uA at nibble 15.
TEST(Sound, ThumpRateFollowsTheNibble) {
    asteroids::Sound s = fresh();
    s.write_thump(0x10);
    EXPECT_NEAR(rate(trace(s, 1.0, mix)), 81, 6);
    s.write_thump(0x1F);
    EXPECT_NEAR(rate(trace(s, 1.0, mix)), 63, 6);
}

TEST(Sound, ThumpIsHeldInResetWithoutD4) {
    asteroids::Sound s = fresh();
    s.write_thump(0x0F);
    run(s, 0.2);
    EXPECT_LT(spread(trace(s, 0.5, mix)), 1e-3);
}

// R11 555: R67 + R69 on C45, R68 joins R69 for the small saucer.
TEST(Sound, SaucerWarbleRateFollowsSaucerSelect) {
    asteroids::Sound small = fresh(), large = fresh();
    small.write_latch(0, true);
    large.write_latch(0, true);
    large.write_latch(2, true);
    run(small, 0.5);
    run(large, 0.5);
    EXPECT_NEAR(rate(trace(small, 3.0, saucer)), 1.44 / (17.6e3 * 10e-6), 0.8);
    EXPECT_NEAR(rate(trace(large, 3.0, saucer)), 1.44 / (25.6e3 * 10e-6), 0.6);
}

TEST(Sound, LargeSaucerSitsLower) {
    asteroids::Sound small = fresh(), large = fresh();
    small.write_latch(0, true);
    large.write_latch(0, true);
    large.write_latch(2, true);
    auto mean = [](const std::vector<double>& v) {
        double m = 0;
        for (double x : v) m += x;
        return m / double(v.size());
    };
    EXPECT_GT(mean(trace(small, 1.0, saucer)), mean(trace(large, 1.0, saucer)) * 1.3);
}

TEST(Sound, SaucerOscillatorStopsWhenDisabled) {
    asteroids::Sound s = fresh();
    s.write_latch(0, true);
    run(s, 0.3);
    s.write_latch(0, false);
    run(s, 0.2);
    EXPECT_EQ(s.saucer_hz(), 0.0);
}

// Q5 starts near 1.9 mA from C47's +5 V and fades as Q1 charges it.
TEST(Sound, ShipFireFallsInPitchAndLevel) {
    asteroids::Sound s = fresh();
    s.write_latch(4, true);
    run(s, 0.005);
    EXPECT_NEAR(s.ship_fire_hz(), 750, 80);
    std::vector<double> early = trace(s, 0.02, [](const asteroids::Sound& x) { return x.ship_fire_out(); });
    run(s, 0.15);
    EXPECT_LT(s.ship_fire_hz(), 400);
    std::vector<double> late = trace(s, 0.02, [](const asteroids::Sound& x) { return x.ship_fire_out(); });
    EXPECT_GT(spread(early), 2 * spread(late));
}

TEST(Sound, SaucerFireOutlastsShipFire) {
    asteroids::Sound s = fresh();
    s.write_latch(1, true);
    s.write_latch(4, true);
    run(s, 0.25);
    EXPECT_GT(s.saucer_fire_hz(), 2 * s.ship_fire_hz());
}

TEST(Sound, LifeToneIsThe3kHzClock) {
    asteroids::Sound s = fresh();
    s.write_latch(5, true);
    run(s, 0.05);
    EXPECT_NEAR(rate(trace(s, 0.5, mix)), 12096000.0 / 4096, 40);
}

// P8 reloads give /12, /6, /3, /5 of 12 kHz for pitch 00, 01, 10, 11.
TEST(Sound, ExplosionPitchSetsTheSampleRate) {
    asteroids::Sound slow = fresh(), fast = fresh();
    slow.write_explode(0x3C);
    fast.write_explode(0xBC);
    double a = rate(trace(slow, 1.0, expff)), b = rate(trace(fast, 1.0, expff));
    EXPECT_NEAR(b / a, 4.0, 0.6);
}

TEST(Sound, ThrustPassesNoiseOnlyWhenEnabled) {
    asteroids::Sound s = fresh();
    run(s, 0.3);
    double off = spread(trace(s, 0.3, mix));
    s.write_latch(3, true);
    run(s, 0.3);
    double on = spread(trace(s, 0.3, mix));
    EXPECT_LT(off, 1e-3);
    EXPECT_GT(on, 0.05);
}

TEST(Sound, NoiseResetClearsTheShiftRegister) {
    asteroids::Sound s = fresh();
    run(s, 0.1);
    s.noise_reset();
    EXPECT_TRUE(s.noise());
}
