#include <gtest/gtest.h>

#include "sound.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr int kHz = 48000;

battlezone::Sound fresh(uint8_t latch) {
    battlezone::Sound s;
    s.reset();
    s.set_rate(kHz);
    s.write(latch);
    return s;
}

void run(battlezone::Sound& s, double seconds, float pokey = 5.0f) {
    for (int i = 0; i < int(seconds * kHz); i++) s.sample(pokey);
}

// Samples of one of the board's nodes over `seconds`.
template <typename F>
std::vector<double> trace(battlezone::Sound& s, double seconds, F node, float pokey = 5.0f) {
    std::vector<double> v;
    for (int i = 0; i < int(seconds * kHz); i++) {
        s.sample(pokey);
        v.push_back(node(s));
    }
    return v;
}

double peak(const std::vector<double>& v, size_t from, size_t to) {
    double m = 0;
    for (size_t i = from; i < to && i < v.size(); i++) m = std::max(m, v[i]);
    return m;
}

int toggles(const std::vector<double>& v) {
    int n = 0;
    for (size_t i = 1; i < v.size(); i++) n += (v[i - 1] > 1e-3) != (v[i] > 1e-3);
    return n;
}

double spread(const std::vector<double>& v) {
    auto [lo, hi] = std::minmax_element(v.begin(), v.end());
    return *hi - *lo;
}

double aud_node(const battlezone::Sound& s) { return s.aud_plus(); }
double shell_node(const battlezone::Sound& s) { return s.shell_out(); }
double shell_gate(const battlezone::Sound& s) { return s.shell_gate() ? 1 : 0; }
double expl_gate(const battlezone::Sound& s) { return s.explosion_gate() ? 1 : 0; }

}  // namespace

TEST(Sound, SoundEnableLowMutesTheMixer) {
    battlezone::Sound s = fresh(0x80);
    run(s, 0.5);
    EXPECT_EQ(s.aud_plus(), 0.0);
}

TEST(Sound, EngineIsStillWithoutMotorEnable) {
    battlezone::Sound s = fresh(0x20);
    run(s, 1.0);
    EXPECT_LT(spread(trace(s, 0.2, aud_node)), 0.01);
}

TEST(Sound, MotorEnableRunsTheCounters) {
    battlezone::Sound s = fresh(0xA0);
    run(s, 1.0);
    EXPECT_GT(spread(trace(s, 0.2, aud_node)), 1.0);
}

// R7 1K / R6 4.7K at idle, R5 1K to ground added by ENGINE REV EN.
TEST(Sound, EngineControlVoltageSettlesOnTheDivider) {
    battlezone::Sound s = fresh(0xA0);
    run(s, 6.0);
    EXPECT_NEAR(s.engine_cv(), 5.0 * 4.7 / 5.7, 0.02);
    double idle = s.engine_hz();
    s.write(0xB0);
    run(s, 12.0);
    double r = 1.0 / (1 / 1e3 + 1 / 4.7e3 + 1 / 1.47e3);
    EXPECT_NEAR(s.engine_cv(), 5.0 * r / 1e3, 0.02);
    EXPECT_GT(s.engine_hz(), idle * 1.8);
}

// The rev drains C13 through R8 100K; letting off refills it through CR1 and R9 22K.
TEST(Sound, EngineRevsUpSlowlyAndDropsBackFast) {
    battlezone::Sound s = fresh(0xA0);
    run(s, 6.0);
    double idle = s.engine_cv();
    s.write(0xB0);
    run(s, 0.3);
    double down = idle - s.engine_cv();
    run(s, 12.0);
    double rev = s.engine_cv();
    s.write(0xA0);
    run(s, 0.3);
    double up = s.engine_cv() - rev;
    EXPECT_GT(up, 2 * down);
}

TEST(Sound, ShellDecaysAfterTheEnableDrops) {
    battlezone::Sound s = fresh(0x24);
    run(s, 0.05);
    s.write(0x20);
    std::vector<double> v = trace(s, 0.6, shell_node);
    EXPECT_GT(peak(v, 0, kHz / 20), 3 * peak(v, kHz / 2, kHz * 6 / 10));
}

// Non-inverting gain 1 + R32 / (R13 + R12) soft, 1 + R32 / (R13 + r_on || R12) loud.
TEST(Sound, ShellLoudBitRaisesTheGain) {
    battlezone::Sound soft = fresh(0x24), loud = fresh(0x2C);
    double a = peak(trace(soft, 0.3, shell_node), 0, kHz), b = peak(trace(loud, 0.3, shell_node), 0, kHz);
    EXPECT_GT(b / a, 3.0);
    EXPECT_LT(b / a, 4.5);
}

// J5 toggles on H4 QH for the shell and on the J4 four-bit NAND for the explosion.
TEST(Sound, ExplosionNoiseIsSlowerThanShellNoise) {
    battlezone::Sound s = fresh(0x25);
    std::vector<double> sh = trace(s, 0.5, shell_gate), ex;
    s = fresh(0x25);
    ex = trace(s, 0.5, expl_gate);
    EXPECT_GT(toggles(ex), 50);
    EXPECT_GT(toggles(sh), toggles(ex) * 2);
}

TEST(Sound, NoiseIsClearedWhileSoundEnableIsLow) {
    battlezone::Sound s = fresh(0x05);
    EXPECT_EQ(toggles(trace(s, 0.5, shell_gate)), 0);
    EXPECT_EQ(toggles(trace(s, 0.5, expl_gate)), 0);
}

TEST(Sound, PokeyReachesTheMixThroughR27) {
    battlezone::Sound s = fresh(0x20);
    run(s, 1.0, 5.0f);
    double hi = s.aud_plus();
    run(s, 0.2, 1.0f);
    double lo = s.aud_plus();
    EXPECT_GT(hi - lo, 1.0);
    EXPECT_LT(hi - lo, 2.0);
}
