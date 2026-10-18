#include <gtest/gtest.h>

#include "sound.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr uint8_t kAmp = 0x20;
constexpr uint8_t kUfo = 0x01;
constexpr uint8_t kMissile = 0x02;
constexpr uint8_t kExplosion = 0x04;
constexpr uint8_t kInvaderHit = 0x08;
constexpr uint8_t kBonusBase = 0x10;
constexpr uint8_t kSaucerHit = 0x10;

constexpr int kRate = mw8080::kSampleRate;

std::vector<float> run(mw8080::Sound& s, double seconds) {
    std::vector<float> out;
    uint64_t end = s.ticks() + uint64_t(seconds * kRate);
    while (s.ticks() < end) {
        s.advance(42);
        out.push_back(s.mix());
    }
    return out;
}

double rms(const std::vector<float>& v, double from_s = 0, double to_s = 1e9) {
    size_t a = size_t(from_s * kRate);
    size_t b = std::min(v.size(), size_t(to_s * kRate));
    double sum = 0;
    for (size_t i = a; i < b; i++) sum += double(v[i]) * v[i];
    return b > a ? std::sqrt(sum / double(b - a)) : 0.0;
}

mw8080::Sound quiet_board() {
    mw8080::Sound s;
    s.port3_w(kAmp);
    s.port5_w(0);
    run(s, 0.5);
    return s;
}

// Fleet 555 output edges per second at the given thump data.
double fleet_hz(uint8_t data) {
    mw8080::Sound s = quiet_board();
    s.port5_w(data);
    run(s, 0.3);
    bool high = s.fleet_555() > 1.7;
    int edges = 0;
    uint64_t start = s.ticks(), end = start + kRate * 2;
    while (s.ticks() < end) {
        s.advance(42);
        bool now = s.fleet_555() > 1.7;
        if (now && !high) edges++;
        high = now;
    }
    return edges / 2.0;
}

// R1 and R2 charge to 4.4 V (5 V less the diode), thresholds 1/3 and 2/3 of 5 V.
double fleet_expected_hz(double r1) {
    const double r2 = 75e3, c = 0.1e-6;
    double t_charge = (r1 + r2) * c * std::log((4.4 - 5.0 / 3) / (4.4 - 10.0 / 3));
    double t_discharge = r2 * c * std::log(2.0);
    return 1.0 / (t_charge + t_discharge);
}

}  // namespace

TEST(Sound, ClockRatioGivesExactSampleRate) {
    mw8080::Sound s;
    s.advance(mw8080::kCpuHz);
    EXPECT_EQ(s.ticks(), uint64_t(kRate));
}

TEST(Sound, IdleIsSilent) {
    auto s = quiet_board();
    auto out = run(s, 1.0);
    EXPECT_LT(rms(out), 1e-3);
}

TEST(Sound, PowerOnLatchIsMuted) {
    mw8080::Sound s;
    EXPECT_FALSE(s.amp_enabled());
    s.port3_w(kExplosion);
    s.port5_w(0x0f);
    auto out = run(s, 0.3);
    EXPECT_EQ(rms(out), 0.0);
}

TEST(Sound, AmpEnableBitMutesTheBoard) {
    mw8080::Sound s = quiet_board();
    s.port5_w(0x01);
    auto on = run(s, 0.5);
    EXPECT_GT(rms(on, 0.2), 0.1);
    s.port3_w(0);
    auto off = run(s, 0.2);
    EXPECT_EQ(rms(off), 0.0);
    s.port3_w(kAmp);
    auto back = run(s, 0.3);
    EXPECT_GT(rms(back), 0.1);
}

TEST(Sound, FlipScreenBitIsReadableAndSilent) {
    mw8080::Sound s = quiet_board();
    EXPECT_FALSE(s.flip_screen());
    s.port5_w(0x20);
    EXPECT_TRUE(s.flip_screen());
    auto out = run(s, 0.5);
    EXPECT_LT(rms(out, 0.2), 1e-3);
}

TEST(Sound, MissileIsAudibleAndTimesOutWhileHeld) {
    mw8080::Sound s = quiet_board();
    s.port3_w(kAmp | kMissile);
    auto out = run(s, 1.5);
    EXPECT_GT(rms(out, 0.1, 0.5), 0.008);
    EXPECT_LT(rms(out, 1.2, 1.5), rms(out, 0.1, 0.5) * 0.1);
}

TEST(Sound, MissileStopsWhenReleased) {
    mw8080::Sound s = quiet_board();
    s.port3_w(kAmp | kMissile);
    auto held = run(s, 0.3);
    s.port3_w(kAmp);
    auto after = run(s, 0.5);
    EXPECT_LT(rms(after, 0.3), rms(held, 0.1) * 0.1);
}

TEST(Sound, ExplosionIsNoiseAndFallsOff) {
    mw8080::Sound s = quiet_board();
    s.port3_w(kAmp | kExplosion);
    auto out = run(s, 0.3);
    EXPECT_GT(rms(out, 0.05), 0.1);
    s.port3_w(kAmp);
    auto tail = run(s, 2.5);
    EXPECT_GT(rms(tail, 0, 0.2), 0.05) << "C26 keeps the burst going after the trigger drops";
    EXPECT_LT(rms(tail, 2.0, 2.5), rms(out, 0.05) * 0.05);
}

TEST(Sound, ExplosionIsBroadbandNoiseNotATone) {
    mw8080::Sound s = quiet_board();
    s.port3_w(kAmp | kExplosion);
    auto out = run(s, 0.4);
    int crossings = 0;
    for (size_t i = size_t(0.1 * kRate) + 1; i < out.size(); i++) {
        if ((out[i - 1] < 0) != (out[i] < 0)) crossings++;
    }
    EXPECT_GT(crossings, 40) << "LFSR noise at 7515 Hz crosses the mixer's midline often";
}

TEST(Sound, InvaderHitIsAudibleAndFallsOff) {
    mw8080::Sound s = quiet_board();
    s.port3_w(kAmp | kInvaderHit);
    auto out = run(s, 1.0);
    EXPECT_GT(rms(out, 0.05, 0.25), 0.015);
    EXPECT_LT(rms(out, 0.8, 1.0), rms(out, 0.05, 0.25) * 0.1);
}

TEST(Sound, InvaderHitReleaseStopsIt) {
    mw8080::Sound s = quiet_board();
    s.port3_w(kAmp | kInvaderHit);
    auto held = run(s, 0.2);
    s.port3_w(kAmp);
    auto after = run(s, 0.4);
    EXPECT_LT(rms(after, 0.2), rms(held, 0.05) * 0.1);
}

TEST(Sound, BonusBaseIsAnInterruptedTone) {
    mw8080::Sound s = quiet_board();
    s.port3_w(kAmp | kBonusBase);
    auto out = run(s, 1.0);
    EXPECT_GT(rms(out, 0.2), 0.02);
    // R94/R95/C34 555 at ~7.4 Hz gates the 480 Hz square, 33 ms off per cycle.
    int quiet = 0, loud = 0;
    for (double t = 0.2; t + 0.02 <= 1.0; t += 0.02) {
        double w = rms(out, t, t + 0.02);
        if (w < 0.025) quiet++;
        if (w > 0.035) loud++;
    }
    EXPECT_GT(quiet, 4);
    EXPECT_GT(loud, 20);
    s.port3_w(kAmp);
    auto after = run(s, 0.5);
    EXPECT_LT(rms(after, 0.2), 1e-3);
}

TEST(Sound, SaucerHitIsAudibleAndSweepsDown) {
    mw8080::Sound s = quiet_board();
    s.port5_w(kSaucerHit);
    auto out = run(s, 1.0);
    EXPECT_GT(rms(out, 0.2), 0.03);
    s.port5_w(0);
    auto tail = run(s, 2.0);
    EXPECT_GT(rms(tail, 0, 0.2), 0.02) << "C23 holds the sweep after the latch clears";
    EXPECT_LT(rms(tail, 1.5, 2.0), rms(out, 0.2) * 0.05);
}

TEST(Sound, FleetThumpIsAudible) {
    mw8080::Sound s = quiet_board();
    s.port5_w(0x01);
    auto out = run(s, 0.5);
    EXPECT_GT(rms(out, 0.2), 0.1);
}

TEST(Sound, FleetDataZeroStopsTheThump) {
    EXPECT_EQ(fleet_hz(0), 0.0);
}

TEST(Sound, FleetNotesFallInPitchAcrossTheFourBits) {
    double f0 = fleet_hz(0x01), f1 = fleet_hz(0x02), f2 = fleet_hz(0x04), f3 = fleet_hz(0x08);
    EXPECT_GT(f0, f1);
    EXPECT_GT(f1, f2);
    EXPECT_GT(f2, f3);
    EXPECT_NEAR(f0, fleet_expected_hz(40e3), 2.0);
    EXPECT_NEAR(f1, fleet_expected_hz(68e3), 2.0);
    EXPECT_NEAR(f2, fleet_expected_hz(82e3), 2.0);
    EXPECT_NEAR(f3, fleet_expected_hz(100e3), 2.0);
}

TEST(Sound, FleetBitsInParallelRaiseThePitch) {
    double both = fleet_hz(0x03);
    EXPECT_GT(both, fleet_hz(0x01));
    EXPECT_NEAR(both, fleet_expected_hz(1.0 / (1.0 / 40e3 + 1.0 / 68e3)), 2.0);
}

TEST(Sound, UfoIsAudibleAndStopsWhenBitClears) {
    mw8080::Sound s = quiet_board();
    s.port3_w(kAmp | kUfo);
    auto out = run(s, 0.5);
    EXPECT_GT(rms(out, 0.1), 0.1);
    s.port3_w(kAmp);
    auto off = run(s, 0.2);
    EXPECT_LT(rms(off), 1e-6);
}

TEST(Sound, UfoWarbleFollowsTheSlfTriangle) {
    mw8080::Sound s = quiet_board();
    s.port3_w(kAmp | kUfo);
    run(s, 0.1);
    // Rising VCO edges per 20 ms window: the VCO pitch tracks the 1 uF / 120k SLF ramp.
    const int win = kRate / 50;
    std::vector<int> edges;
    bool prev = s.ufo_vco();
    int n = 0, tick = 0;
    double slf_lo = 9, slf_hi = 0;
    for (int k = 0; k < kRate; k++) {
        s.advance(42);
        bool now = s.ufo_vco();
        if (now && !prev) n++;
        prev = now;
        slf_lo = std::min(slf_lo, s.ufo_slf_voltage());
        slf_hi = std::max(slf_hi, s.ufo_slf_voltage());
        if (++tick == win) {
            edges.push_back(n);
            n = 0;
            tick = 0;
        }
    }
    auto [mn, mx] = std::minmax_element(edges.begin(), edges.end());
    EXPECT_GT(*mn, 5);
    EXPECT_GT(*mx, *mn * 3) << "pitch sweeps roughly 800 Hz to 5 kHz";
    EXPECT_LT(slf_lo, 0.4);
    EXPECT_GT(slf_hi, 2.3);
}

TEST(Sound, UfoSlfRunsAtAbout7Hz) {
    mw8080::Sound s = quiet_board();
    int peaks = 0;
    bool high = false;
    const int total = kRate * 2;
    for (int k = 0; k < total; k++) {
        s.advance(42);
        bool now = s.ufo_slf_voltage() > 2.3;
        if (now && !high) peaks++;
        high = now;
    }
    EXPECT_NEAR(peaks / 2.0, 7.2, 1.0);
}

TEST(Sound, ResetSilencesEverything) {
    mw8080::Sound s = quiet_board();
    s.port3_w(kAmp | kExplosion | kUfo | kBonusBase);
    s.port5_w(0x0f | kSaucerHit);
    run(s, 0.3);
    s.reset();
    EXPECT_FALSE(s.amp_enabled());
    s.port3_w(kAmp);
    s.port5_w(0);
    auto out = run(s, 0.8);
    EXPECT_LT(rms(out, 0.3), 1e-3);
    EXPECT_EQ(s.ticks(), uint64_t(0.8 * kRate));
}

TEST(Sound, MixReturnsTheMeanSincePreviousCall) {
    mw8080::Sound s = quiet_board();
    s.port5_w(0x01);
    run(s, 0.3);
    s.advance(mw8080::kCpuHz / 100);
    float coarse = s.mix();
    EXPECT_LE(std::fabs(coarse), 1.0f);
    s.advance(0);
    EXPECT_EQ(s.mix(), 0.0f);
}

namespace {

mw8080::Sound quiet_taito() {
    mw8080::Sound s(mw8080::SoundBoard::TaitoPv);
    s.port3_w(kAmp);
    s.port5_w(0);
    run(s, 0.5);
    return s;
}

int crossings(const std::vector<float>& v, double from_s) {
    int n = 0;
    for (size_t i = size_t(from_s * kRate) + 1; i < v.size(); i++) {
        if (v[i - 1] < 0 && v[i] >= 0) n++;
    }
    return n;
}

// Seconds until a 20 ms window falls below a tenth of the first loud one.
double decay_time(const std::vector<float>& v) {
    const double first = rms(v, 0.02, 0.04);
    for (double t = 0.04; t + 0.02 <= double(v.size()) / kRate; t += 0.01) {
        if (rms(v, t, t + 0.02) < first * 0.1) return t;
    }
    return 1e9;
}

}  // namespace

TEST(TaitoSound, EverySourceReachesTheSpeaker) {
    const uint8_t p3[] = {kUfo, kMissile, kExplosion, kInvaderHit, kBonusBase};
    for (uint8_t bit : p3) {
        mw8080::Sound s = quiet_taito();
        s.port3_w(kAmp | bit);
        EXPECT_GT(rms(run(s, 0.3), 0.05), 0.01) << "OUT 3 bit " << int(bit);
    }
    const uint8_t p5[] = {0x01, kSaucerHit};
    for (uint8_t bit : p5) {
        mw8080::Sound s = quiet_taito();
        s.port5_w(bit);
        EXPECT_GT(rms(run(s, 0.3), 0.05), 0.01) << "OUT 5 bit " << int(bit);
    }
}

TEST(TaitoSound, IdleIsSilentAndSx5Mutes) {
    mw8080::Sound s = quiet_taito();
    EXPECT_LT(rms(run(s, 0.3)), 1e-3);
    s.port3_w(kInvaderHit);
    EXPECT_EQ(rms(run(s, 0.3)), 0.0);
}

TEST(TaitoSound, ExtendPlayGatesTheFasterTap) {
    mw8080::Sound midway = quiet_board();
    mw8080::Sound taito = quiet_taito();
    midway.port3_w(kAmp | kBonusBase);
    taito.port3_w(kAmp | kBonusBase);
    const double ratio = double(crossings(run(taito, 1.0), 0.2)) / crossings(run(midway, 1.0), 0.2);
    EXPECT_NEAR(ratio, 2.0, 0.3) << "960 Hz on PVR00001, 480 Hz on Midway";
}

TEST(TaitoSound, TriggerOneShotIsShorter) {
    mw8080::Sound midway = quiet_board();
    mw8080::Sound taito = quiet_taito();
    midway.port3_w(kAmp | kMissile);
    taito.port3_w(kAmp | kMissile);
    const double tm = decay_time(run(midway, 1.5));
    const double tt = decay_time(run(taito, 1.5));
    EXPECT_LT(tt, tm * 0.5) << "C12 0.22uF against Midway's 1uF";
}
