#include <gtest/gtest.h>

#include "sound.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

using mw8080::Sound;
using mw8080::SoundBoard;
using mw8080::kSampleRate;

constexpr double kDt = 1.0 / kSampleRate;
// LM3900: inputs one VBE up, output swing 0.09 V to V+ less one VBE (AN-72).
constexpr double kVbe = 0.5;
constexpr double kVoh = 11.5;
constexpr double kVol = 0.09;
constexpr double kBias12 = (12.0 - kVbe);

// Taito trimmer: 50K at mid travel, 47K from the wiper into the bus.
constexpr double kTrimIn = 25e3 + 25e3 * 47e3 / (25e3 + 47e3);
constexpr double kTrimRatio = 25e3 / (25e3 + 47e3);

void tick(Sound& s) {
    const uint64_t t = s.ticks();
    s.advance(41);
    while (s.ticks() == t) s.advance(1);
}

template <typename F>
void run(Sound& s, double seconds, F each) {
    const int n = int(seconds * kSampleRate);
    for (int i = 0; i < n; i++) {
        tick(s);
        each(i);
    }
}

void run(Sound& s, double seconds) {
    run(s, seconds, [](int) {});
}

double peak_mix(Sound& s, double seconds) {
    s.mix();
    double peak = 0;
    run(s, seconds, [&](int) { peak = std::max(peak, double(std::fabs(s.mix()))); });
    return peak;
}

template <typename F>
double rising_edges_per_second(Sound& s, double seconds, F level) {
    bool prev = level();
    int edges = 0;
    run(s, seconds, [&](int) {
        const bool now = level();
        if (now && !prev) edges++;
        prev = now;
    });
    return edges / seconds;
}

// Rising-edge times in samples.
template <typename F>
std::vector<int> rising_edges(Sound& s, double seconds, F level) {
    std::vector<int> at;
    bool prev = level();
    run(s, seconds, [&](int i) {
        const bool now = level();
        if (now && !prev) at.push_back(i);
        prev = now;
    });
    return at;
}

Sound powered(SoundBoard b) {
    Sound s(b);
    s.port3_w(0x20);
    run(s, 0.5);
    return s;
}

// --- Sound: clocking, gating, mixing ---

TEST(Sound, ClockRatioGivesExactSampleRate) {
    Sound s;
    s.advance(mw8080::kCpuHz);
    EXPECT_EQ(s.ticks(), uint64_t(kSampleRate));
}

TEST(Sound, IdleIsSilent) {
    for (SoundBoard b : {SoundBoard::Midway, SoundBoard::TaitoPv}) {
        Sound s = powered(b);
        run(s, 1.0);
        s.mix();
        EXPECT_LT(peak_mix(s, 0.5), 1e-3);
    }
}

TEST(Sound, AmpEnableBitMutesTheBoard) {
    for (SoundBoard b : {SoundBoard::Midway, SoundBoard::TaitoPv}) {
        Sound s(b);
        s.port3_w(0x05);
        EXPECT_FALSE(s.amp_enabled());
        EXPECT_EQ(peak_mix(s, 0.5), 0.0);
        s.port3_w(0x25);
        EXPECT_TRUE(s.amp_enabled());
        EXPECT_GT(peak_mix(s, 0.5), 0.01);
    }
}

TEST(Sound, FlipScreenBitIsReadableAndSilent) {
    Sound s = powered(SoundBoard::Midway);
    run(s, 1.0);
    s.port5_w(0x20);
    EXPECT_TRUE(s.flip_screen());
    EXPECT_LT(peak_mix(s, 0.3), 1e-3);
}

TEST(Sound, MixReturnsTheMeanSincePreviousCall) {
    Sound s = powered(SoundBoard::Midway);
    s.port5_w(0x01);
    run(s, 0.2);
    s.mix();
    double sum = 0;
    int n = 0;
    Sound twin = s;
    for (int i = 0; i < 400; i++) {
        tick(twin);
        sum += twin.mix();
        n++;
    }
    for (int i = 0; i < 400; i++) tick(s);
    EXPECT_NEAR(s.mix(), sum / n, 1e-6);
    EXPECT_EQ(s.mix(), s.mix());
}

TEST(Sound, ResetSilencesEverything) {
    Sound s = powered(SoundBoard::Midway);
    s.port3_w(0x3F);
    s.port5_w(0x1F);
    run(s, 0.3);
    s.reset();
    EXPECT_FALSE(s.amp_enabled());
    EXPECT_EQ(s.ticks(), 0u);
    s.port3_w(0x20);
    run(s, 1.0);
    s.mix();
    EXPECT_LT(peak_mix(s, 0.3), 1e-3);
}

TEST(Sound, ExplosionAtFullEnvelopeReachesTheAmpRails) {
    Sound s = powered(SoundBoard::Midway);
    s.port3_w(0x24);
    double lo = 0, hi = 0;
    run(s, 0.6, [&](int) {
        lo = std::min(lo, s.amp_out());
        hi = std::max(hi, s.amp_out());
    });
    // R105 100K against R104 220K biases the output at 5.7 V.
    const double dc = kVbe + 100e3 * kBias12 / 220e3;
    EXPECT_GT(hi - lo, 0.8 * (kVoh - kVol));
    EXPECT_LE(hi, kVoh - dc + 1e-9);
    EXPECT_GE(lo, kVol - dc - 1e-9);
}

// --- Noise generator ---

TEST(Noise, BitRateFollowsTheCmosClock) {
    Sound s;
    // A random bit rises on a quarter of the clocks; clock 1/(2 ln3 R113 C46).
    const double clock = 1.0 / (2 * std::log(3.0) * 56e3 * 0.001e-6);
    EXPECT_NEAR(rising_edges_per_second(s, 4.0, [&] { return s.noise(); }), clock / 4, clock / 4 * 0.05);
}

TEST(Noise, NeverLocksUp) {
    Sound s;
    int ones = 0, n = 0;
    run(s, 3.0, [&](int) {
        ones += s.noise();
        n++;
    });
    EXPECT_NEAR(double(ones) / n, 0.5, 0.03);
}

// --- SN76477 ---

mw8080::analog::Sn76477 midway_sn() {
    mw8080::analog::Sn76477 sn{120e3, 1e-6, 39e3, 0.047e-6, 150e3, 10e3};
    sn.reset();
    return sn;
}

TEST(Sn76477, SlfFrequencyIsDatasheetEquationOne) {
    auto sn = midway_sn();
    int edges = 0;
    bool prev = sn.slf_rising;
    const int n = 10 * kSampleRate;
    for (int i = 0; i < n; i++) {
        sn.step(kDt);
        if (sn.slf_rising && !prev) edges++;
        prev = sn.slf_rising;
    }
    EXPECT_NEAR(edges / 10.0, 0.64 / (120e3 * 1e-6), 0.1);
}

TEST(Sn76477, SlfIsAnEqualSidedTriangleBetweenTheTripPoints) {
    auto sn = midway_sn();
    while (!sn.slf_rising) sn.step(kDt);
    while (sn.slf_rising) sn.step(kDt);
    double lo = 9, hi = 0;
    int up = 0, down = 0;
    for (int cycles = 0; cycles < 10;) {
        const bool was = sn.slf_rising;
        sn.step(kDt);
        lo = std::min(lo, sn.v_slf);
        hi = std::max(hi, sn.v_slf);
        (sn.slf_rising ? up : down)++;
        if (!sn.slf_rising && was) cycles++;
    }
    EXPECT_NEAR(lo, 0.35, 0.01);
    EXPECT_NEAR(hi, 2.5, 0.01);
    EXPECT_NEAR(double(up) / down, 1.0, 0.01);
}

TEST(Sn76477, VcoSweepsBetweenTheSlfEndsTenToOneLaw) {
    auto sn = midway_sn();
    const double f_min = 0.64 / (39e3 * 0.047e-6);
    // f = f_min (2.5 + V0) / (V + V0), V0 = 2.5 / 9 puts 0 V at 10 f_min.
    const double v0 = 2.5 / 9;
    int last = -1;
    bool prev = sn.vco_out;
    int checked = 0;
    for (int i = 0; i < 2 * kSampleRate; i++) {
        sn.step(kDt);
        if (sn.vco_out && !prev) {
            if (last >= 0) {
                const double f = double(kSampleRate) / (i - last);
                const double want = f_min * (2.5 + v0) / (sn.v_slf + v0);
                EXPECT_NEAR(f, want, want * 0.08) << "slf " << sn.v_slf;
                checked++;
            }
            last = i;
        }
        prev = sn.vco_out;
    }
    EXPECT_GT(checked, 1000);
}

TEST(Sn76477, OutputPulseHeightIsDatasheetEquationEight) {
    auto sn = midway_sn();
    sn.inhibit_w(false);
    double lo = 0, hi = -1;
    for (int i = 0; i < kSampleRate / 10; i++) {
        const double v = sn.step(kDt);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    EXPECT_NEAR(lo, -3.4 * 10e3 / 150e3, 1e-9);
    EXPECT_EQ(hi, 0.0);
}

TEST(Sn76477, Pin9HighInhibitsAndOneShotLatchHoldsWithoutRos) {
    auto sn = midway_sn();
    auto active = [&] {
        double lo = 0;
        for (int i = 0; i < kSampleRate / 20; i++) lo = std::min(lo, sn.step(kDt));
        return lo < 0;
    };
    sn.inhibit = false;
    EXPECT_FALSE(active());
    sn.inhibit_w(true);
    sn.inhibit_w(false);
    EXPECT_TRUE(active());
    sn.inhibit_w(true);
    EXPECT_FALSE(active());
    sn.inhibit_w(false);
    EXPECT_TRUE(active());
    for (int i = 0; i < 20 * kSampleRate; i++) sn.step(kDt);
    EXPECT_TRUE(active());
}

TEST(Sound, UfoBitDrivesTheSn76477ThroughR125) {
    Sound s = powered(SoundBoard::Midway);
    double lo = 0;
    run(s, 0.2, [&](int) { lo = std::min(lo, s.source_current(Sound::kUfo)); });
    EXPECT_EQ(lo, 0.0);
    s.port3_w(0x21);
    run(s, 0.2, [&](int) { lo = std::min(lo, s.source_current(Sound::kUfo)); });
    EXPECT_NEAR(lo, -3.4 * 10e3 / 150e3 / 47e3, 1e-9);
    s.port3_w(0x20);
    run(s, 0.01);
    double now = 0;
    run(s, 0.2, [&](int) { now = std::min(now, s.source_current(Sound::kUfo)); });
    EXPECT_EQ(now, 0.0);
}

TEST(Sound, UfoSlfAndVcoUseR120C35AndR121C36) {
    for (SoundBoard b : {SoundBoard::Midway, SoundBoard::TaitoPv}) {
        Sound s(b);
        s.port3_w(0x21);
        EXPECT_NEAR(rising_edges_per_second(s, 6.0, [&] { return s.ufo_slf_voltage() > 1.4; }), 0.64 / (120e3 * 1e-6), 0.2);
        const double f_min = 0.64 / (39e3 * 0.047e-6);
        int last = -1, checked = 0;
        bool prev = s.ufo_vco();
        run(s, 1.0, [&](int i) {
            const bool now = s.ufo_vco();
            if (now && !prev) {
                if (last >= 0 && s.ufo_slf_voltage() > 2.3) {
                    const double f = double(kSampleRate) / (i - last);
                    const double want = f_min * (2.5 + 2.5 / 9) / (s.ufo_slf_voltage() + 2.5 / 9);
                    EXPECT_NEAR(f, want, want * 0.05);
                    checked++;
                }
                last = i;
            }
            prev = now;
        });
        EXPECT_GT(checked, 20);
    }
}

// --- Norton one-shot ---

mw8080::analog::NortonOneShot missile_one_shot(double c) {
    mw8080::analog::NortonOneShot os{4.7e6, 1e6, 1e6, c, 2.2e6, 100e3, 470e-12};
    os.reset();
    return os;
}

// Cap charges toward the R33/R31 Thevenin point and trips when R31 adds enough
// current to outweigh R34's feedback.
double one_shot_period(const mw8080::analog::NortonOneShot& os, double v0) {
    const double v_th = (kVoh * os.r_in + kVbe * os.r_out) / (os.r_in + os.r_out);
    const double tau = os.c * os.r_in * os.r_out / (os.r_in + os.r_out);
    const double v_trip = kVbe + os.r_in * ((kVoh - kVbe) / os.r_fb - kBias12 / os.r_bias);
    return tau * std::log((v_th - v0) / (v_th - v_trip));
}

double measure_one_shot(mw8080::analog::NortonOneShot& os, double v_high, double* v0) {
    for (int i = 0; i < kSampleRate; i++) os.step(0, kDt);
    *v0 = os.v_cap;
    int n = 0;
    os.step(v_high, kDt);
    EXPECT_TRUE(os.high);
    while (os.high && n < 20 * kSampleRate) {
        os.step(v_high, kDt);
        n++;
    }
    return double(n + 1) / kSampleRate;
}

TEST(NortonOneShot, PeriodFollowsTheTimingCapCharge) {
    for (double c : {1e-6, 0.22e-6, 2.2e-6, 0.1e-6}) {
        auto os = missile_one_shot(c);
        double v0 = 0;
        const double t = measure_one_shot(os, 5.0, &v0);
        const double want = one_shot_period(os, v0);
        EXPECT_NEAR(t, want, want * 0.01 + 2.0 / kSampleRate) << "C " << c;
    }
}

TEST(NortonOneShot, HeldLineFiresOnceAndNeedsAFreshEdge) {
    auto os = missile_one_shot(0.1e-6);
    double v0 = 0;
    measure_one_shot(os, 5.0, &v0);
    for (int i = 0; i < kSampleRate; i++) {
        os.step(5.0, kDt);
        ASSERT_FALSE(os.high);
    }
    os.step(0, kDt);
    for (int i = 0; i < kSampleRate / 100; i++) os.step(0, kDt);
    os.step(5.0, kDt);
    EXPECT_TRUE(os.high);
}

TEST(Sound, TaitoTriggerOneShotIsShorterByItsC12) {
    auto midway = missile_one_shot(1e-6);
    auto taito = missile_one_shot(0.22e-6);
    double v0m = 0, v0t = 0;
    const double tm = measure_one_shot(midway, 5.0, &v0m);
    const double tt = measure_one_shot(taito, 12.0, &v0t);
    EXPECT_NEAR(tt / tm, 0.22, 0.01);

    auto duration = [](SoundBoard b) {
        Sound s = powered(b);
        s.port3_w(0x22);
        int n = 0;
        run(s, 1.0, [&](int) { n += s.missile_one_shot() > 5; });
        return double(n) / kSampleRate;
    };
    EXPECT_NEAR(duration(SoundBoard::TaitoPv) / duration(SoundBoard::Midway), 0.22, 0.02);
}

// --- Norton oscillators ---

struct RampLimits {
    double hi, lo, up, down;
};

RampLimits ramp_limits(const mw8080::analog::NortonRamp& r, double i_extra) {
    const double i_ref = kBias12 / r.r_ref;
    return {
        kVbe + r.r_cmp * (i_ref + (kVoh - kVbe) / r.r_hys),
        kVbe + r.r_cmp * (i_ref + (kVol - kVbe) / r.r_hys),
        ((kVoh - kVbe) / r.r_sq - kBias12 / r.r_bias - i_extra) / r.c,
        (kBias12 / r.r_bias + i_extra) / r.c,
    };
}

double ramp_hz(mw8080::analog::NortonRamp r, double i_extra, double seconds) {
    r.reset();
    for (int i = 0; i < kSampleRate; i++) r.step(i_extra, kDt);
    int edges = 0;
    bool prev = r.high;
    const int n = int(seconds * kSampleRate);
    for (int i = 0; i < n; i++) {
        r.step(i_extra, kDt);
        if (r.high && !prev) edges++;
        prev = r.high;
    }
    return edges / seconds;
}

TEST(NortonRamp, SawAndTriangleFollowTheirChargeCurrents) {
    // C16 R37 R41 R39 R40 R38, C21 R70 R64 R62 R63 R61
    for (const mw8080::analog::NortonRamp& r : {mw8080::analog::NortonRamp{0.1e-6, 1e6, 10e3, 1e6, 120e3, 100e3},
                                               mw8080::analog::NortonRamp{0.22e-6, 1e6, 10e3, 1e6, 120e3, 100e3},
                                               mw8080::analog::NortonRamp{0.1e-6, 1e6, 470e3, 1e6, 120e3, 100e3}}) {
        const RampLimits l = ramp_limits(r, 0);
        const double want = 1.0 / ((l.hi - l.lo) / l.up + (l.hi - l.lo) / l.down);
        EXPECT_NEAR(ramp_hz(r, 0, 20), want, want * 0.02) << "C " << r.c << " Rsq " << r.r_sq;
    }
}

TEST(NortonRamp, ExtraInputCurrentTradesRiseForFall) {
    // C10 R13 R16 R18 R19 R17 with R14/R12 currents
    const mw8080::analog::NortonRamp r{330e-12, 3.3e6, 1e6, 1e6, 2.2e6, 560e3};
    for (double i_extra : {0.0, 1e-6, 2e-6}) {
        const RampLimits l = ramp_limits(r, i_extra);
        const double want = 1.0 / ((l.hi - l.lo) / l.up + (l.hi - l.lo) / l.down);
        EXPECT_NEAR(ramp_hz(r, i_extra, 2), want, want * 0.03) << i_extra;
    }
}

TEST(NortonVco, FrequencyIsLinearInTheControlVoltage) {
    // C17 R42 R43 R44 R46 R45
    mw8080::analog::NortonVco v{330e-12, 1e6, 470e3, 680e3, 1e6, 1e6};
    const double hi = kVbe + v.r_in * (kBias12 / v.r_ref - (kVol - kVbe) / v.r_hys);
    const double lo = kVbe + v.r_in * (kBias12 / v.r_ref - (kVoh - kVbe) / v.r_hys);
    for (double vin : {2.0, 5.0, 10.0}) {
        v.reset();
        const double i = vin - kVbe;
        const double up = i * (1 / v.r_plus - 1 / v.r_minus) / v.c;
        const double down = i / v.r_minus / v.c;
        const double want = 1.0 / ((hi - lo) / up + (hi - lo) / down);
        int edges = 0;
        bool prev = v.high;
        for (int k = 0; k < kSampleRate; k++) {
            v.step(vin, kDt);
            if (v.high && !prev) edges++;
            prev = v.high;
        }
        EXPECT_NEAR(edges, want, want * 0.04) << vin;
    }
}

TEST(Sound, HitSawRunsSlowerOnTaitoByItsCap) {
    auto saw_hz = [](SoundBoard b) {
        Sound s(b);
        run(s, 0.5);
        return rising_edges_per_second(s, 4.0, [&] { return s.hit3_saw() > 6.0; });
    };
    const double midway = saw_hz(SoundBoard::Midway);
    EXPECT_NEAR(saw_hz(SoundBoard::TaitoPv) / midway, 0.1 / 0.22, 0.03);
    EXPECT_NEAR(midway, 12.0, 0.6);
}

// A linear VCO's cycle count is k times the integral of its input above VBE.
template <typename Ramp, typename Vco>
void expect_vco_tracks(Sound& s, Ramp ramp, Vco vco, double c) {
    const double hi = kVbe + 680e3 * (kBias12 / 1e6 - (kVol - kVbe) / 1e6);
    const double lo = kVbe + 680e3 * (kBias12 / 1e6 - (kVoh - kVbe) / 1e6);
    const double k = 1.0 / ((hi - lo) * c * (1 / (1 / 470e3 - 1 / 1e6) + 1e6));
    int last = -1, checked = 0;
    double area = 0;
    bool prev = vco() > 4.0;
    run(s, 1.0, [&](int i) {
        area += (ramp() - kVbe) * kDt;
        const bool now = vco() > 4.0;
        if (now && !prev) {
            if (last >= 0) {
                const double cycles = k * area;
                EXPECT_NEAR(cycles, 1.0, 0.05 + 2.0 * k * (ramp() - kVbe) * kDt) << "at " << i;
                checked++;
            }
            last = i;
            area = 0;
        }
        prev = now;
    });
    EXPECT_GT(checked, 100);
}

TEST(Sound, HitVcosAreSweptByTheirRamps) {
    for (SoundBoard b : {SoundBoard::Midway, SoundBoard::TaitoPv}) {
        Sound s(b);
        expect_vco_tracks(s, [&] { return s.hit3_saw(); }, [&] { return s.hit3_vco(); }, 330e-12);  // C17
        expect_vco_tracks(s, [&] { return s.hit5_tri(); }, [&] { return s.hit5_vco(); }, 470e-12);  // C22
    }
}

// --- Missile ---

TEST(Sound, MissileSoundsWhileHeldAndStopsWithC14) {
    Sound s = powered(SoundBoard::Midway);
    s.port3_w(0x22);
    double peak = 0;
    run(s, 0.2, [&](int) { peak = std::max(peak, std::fabs(s.source_current(Sound::kMissile))); });
    EXPECT_GT(peak, 1e-6);
    s.port3_w(0x20);
    // R60 C14 bleed, 56 ms
    run(s, 0.4);
    double after = 0;
    run(s, 0.2, [&](int) { after = std::max(after, std::fabs(s.source_current(Sound::kMissile))); });
    EXPECT_LT(after, peak * 0.01);
}

TEST(Sound, MissilePitchRampFallsAtR36OverC13) {
    Sound s = powered(SoundBoard::Midway);
    s.port3_w(0x22);
    while (s.missile_one_shot() < 5) tick(s);
    while (s.missile_one_shot() > 5) tick(s);
    run(s, 0.01);
    const double a = s.missile_pitch();
    run(s, 0.1);
    EXPECT_NEAR((a - s.missile_pitch()) / 0.1, kBias12 / 1.5e6 / 0.22e-6, 1.0);
}

// --- Explosion ---

TEST(Sound, ExplosionEnvelopeBleedsThroughR83IntoC26) {
    Sound s = powered(SoundBoard::Midway);
    s.port3_w(0x24);
    run(s, 1.0);
    const double a = s.explosion_env();
    run(s, 0.68);
    EXPECT_NEAR((s.explosion_env() - kVbe) / (a - kVbe), std::exp(-1.0), 0.02);
}

TEST(Sound, ExplosionIsNoiseThatFallsOff) {
    Sound s = powered(SoundBoard::Midway);
    s.port3_w(0x24);
    auto rms = [&](double t) {
        double sum = 0;
        int n = 0;
        run(s, t, [&](int) {
            sum += s.source_current(Sound::kExplosion) * s.source_current(Sound::kExplosion);
            n++;
        });
        return std::sqrt(sum / n);
    };
    const double early = rms(0.3);
    run(s, 1.5);
    EXPECT_GT(early, 4 * rms(0.3));
}

// --- OUT 3 bit 3 and OUT 5 bit 4 hits ---

TEST(Sound, Hit3EnvelopeFallsAtR137OverC19) {
    Sound s = powered(SoundBoard::Midway);
    s.port3_w(0x28);
    double top = 0;
    run(s, 0.1, [&](int) { top = std::max(top, s.hit3_env()); });
    EXPECT_NEAR(top, kVoh, 0.01);
    const double a = s.hit3_env();
    run(s, 0.1);
    EXPECT_NEAR((a - s.hit3_env()) / 0.1, kBias12 / 1e6 / 0.47e-6, 0.5);
}

TEST(Sound, Hit5EnvelopeRisesAndFallsWithTheBit) {
    Sound s = powered(SoundBoard::Midway);
    s.port5_w(0x10);
    run(s, 0.01);
    double a = s.hit5_env();
    run(s, 0.05);
    // 1K + R72 against R71, into C23
    EXPECT_NEAR((s.hit5_env() - a) / 0.05, (kBias12 / 101e3 - kBias12 / 1e6) / 1e-6, 2.0);
    run(s, 0.2);
    s.port5_w(0x00);
    a = s.hit5_env();
    run(s, 0.2);
    EXPECT_NEAR((a - s.hit5_env()) / 0.2, kBias12 / 1e6 / 1e-6, 0.3);
}

TEST(Sound, HitsAreSilentUntilTriggered) {
    for (SoundBoard b : {SoundBoard::Midway, SoundBoard::TaitoPv}) {
        Sound s = powered(b);
        double hit3 = 0, hit5 = 0;
        run(s, 0.3, [&](int) {
            hit3 = std::max(hit3, s.source_current(Sound::kHit3));
            hit5 = std::max(hit5, s.source_current(Sound::kHit5));
        });
        EXPECT_LT(hit3, 1e-6);
        EXPECT_LT(hit5, 1e-6);
        s.port3_w(0x28);
        s.port5_w(0x10);
        double lo3 = 1, hi3 = 0, lo5 = 1, hi5 = 0;
        run(s, 0.3, [&](int) {
            lo3 = std::min(lo3, s.source_current(Sound::kHit3));
            hi3 = std::max(hi3, s.source_current(Sound::kHit3));
            lo5 = std::min(lo5, s.source_current(Sound::kHit5));
            hi5 = std::max(hi5, s.source_current(Sound::kHit5));
        });
        EXPECT_GT(hi3 - lo3, 5e-6);
        EXPECT_GT(hi5 - lo5, 5e-6);
    }
}

// --- Fleet / step ---

double fleet_hz(double r_ladder) {
    const double r_a = 1e3 + r_ladder, r_b = 75e3, c = 0.1e-6, vs = 5.0 - 0.6;
    const double t_hi = (r_a + r_b) * c * std::log((vs - 5.0 / 3) / (vs - 10.0 / 3));
    return 1.0 / (t_hi + r_b * c * std::log(2.0));
}

TEST(Sound, FleetNotesFollowTheLadderResistors) {
    const double ladder[4] = {40e3, 68e3, 82e3, 100e3};  // R126+R127, R128, R129, R130
    for (int bit = 0; bit < 4; bit++) {
        Sound s = powered(SoundBoard::Midway);
        s.port5_w(uint8_t(1 << bit));
        run(s, 0.1);
        const double f = rising_edges_per_second(s, 4.0, [&] { return s.fleet_555() > 1.7; });
        EXPECT_NEAR(f, fleet_hz(ladder[bit]), fleet_hz(ladder[bit]) * 0.02) << bit;
    }
}

TEST(Sound, FleetBitsInParallelRaiseThePitch) {
    Sound s = powered(SoundBoard::Midway);
    s.port5_w(0x0F);
    run(s, 0.1);
    EXPECT_GT(rising_edges_per_second(s, 2.0, [&] { return s.fleet_555() > 1.7; }), fleet_hz(40e3) * 1.1);
}

TEST(Sound, FleetDataZeroParksThe556) {
    Sound s = powered(SoundBoard::Midway);
    s.port5_w(0x01);
    run(s, 0.2);
    s.port5_w(0x00);
    run(s, 0.05);
    EXPECT_EQ(rising_edges_per_second(s, 1.0, [&] { return s.fleet_555() > 1.7; }), 0.0);
    EXPECT_GT(s.fleet_555(), 1.7);
    s.mix();
    run(s, 1.0);
    s.mix();
    EXPECT_LT(peak_mix(s, 0.3), 1e-3);
}

TEST(Sound, FleetThumpIsTheLoudestRegularSound) {
    Sound s = powered(SoundBoard::Midway);
    s.port5_w(0x01);
    EXPECT_GT(peak_mix(s, 0.2), 0.4);
}

// --- Bonus / extend play ---

TEST(Sound, BonusGatesTheTapWithTheSlow556) {
    for (SoundBoard b : {SoundBoard::Midway, SoundBoard::TaitoPv}) {
        Sound s = powered(b);
        EXPECT_EQ(rising_edges_per_second(s, 0.2, [&] { return s.bonus_out() > 1.7; }), 0.0);
        s.port3_w(0x30);
        run(s, 0.5);
        // R94 + R95 charge, R95 discharge, C34
        const double t_hi = (100e3 + 47e3) * 1e-6 * std::log(2.0), t_lo = 47e3 * 1e-6 * std::log(2.0);
        const std::vector<int> on = rising_edges(s, 4.0, [&] { return s.bonus_out() > 1.7 || s.ticks() == 0; });
        int bursts = 0;
        for (size_t i = 1; i < on.size(); i++) bursts += on[i] - on[i - 1] > kSampleRate / 100;
        EXPECT_NEAR(bursts / 4.0, 1.0 / (t_hi + t_lo), 0.4);
        const double tone = b == SoundBoard::Midway ? 480 : 960;
        EXPECT_NEAR(double(on.size()) / 4.0, tone * t_hi / (t_hi + t_lo), tone * 0.04);
        s.port3_w(0x20);
        run(s, 0.01);
        EXPECT_EQ(rising_edges_per_second(s, 0.3, [&] { return s.bonus_out() > 1.7; }), 0.0);
    }
}

// --- Taito PV mixer ---

TEST(TaitoSound, EverySourceReachesTheBusThroughItsTrimmer) {
    Sound s = powered(SoundBoard::TaitoPv);
    s.port3_w(0x3F);
    s.port5_w(0x11);
    double lo[Sound::kSources]{}, hi[Sound::kSources]{};
    run(s, 0.5, [&](int) {
        for (int i = 0; i < Sound::kSources; i++) {
            lo[i] = std::min(lo[i], s.source_current(i));
            hi[i] = std::max(hi[i], s.source_current(i));
        }
    });
    for (int i = 0; i < Sound::kSources; i++) EXPECT_GT(hi[i] - lo[i], 1e-8) << i;
    // SN76477 through 47K, then VR1 and its 47K
    EXPECT_NEAR(lo[Sound::kUfo], -3.4 * 10e3 / 150e3 / (47e3 + kTrimIn) * kTrimRatio, 1e-12);
}

TEST(TaitoSound, IdleIsSilentAndSx5Mutes) {
    Sound s = powered(SoundBoard::TaitoPv);
    run(s, 1.0);
    s.mix();
    EXPECT_LT(peak_mix(s, 0.3), 1e-3);
    s.port3_w(0x08);
    EXPECT_EQ(peak_mix(s, 0.3), 0.0);
}

TEST(TaitoSound, TrimmersMakeTheBoardQuieterThanMidwayForTheStep) {
    auto peak = [](SoundBoard b) {
        Sound s = powered(b);
        s.port5_w(0x01);
        return peak_mix(s, 0.2);
    };
    // 68K plus the trimmer into the bus, against R134 10K
    const double want = (10e3 / (68e3 + kTrimIn)) * kTrimRatio;
    EXPECT_NEAR(peak(SoundBoard::TaitoPv) / peak(SoundBoard::Midway), want, want * 0.15);
}

}  // namespace
