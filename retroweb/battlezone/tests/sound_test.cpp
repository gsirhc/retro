#include <gtest/gtest.h>

#include "sound.h"

#include <cmath>
#include <vector>

namespace {

std::vector<float> render(battlezone::Sound& s, int n, float pokey = 0) {
    std::vector<float> out;
    for (int i = 0; i < n; i++) out.push_back(s.sample(pokey));
    return out;
}

double rms(const std::vector<float>& v, size_t from = 0) {
    double acc = 0;
    for (size_t i = from; i < v.size(); i++) acc += double(v[i]) * v[i];
    return std::sqrt(acc / double(v.size() - from));
}

battlezone::Sound fresh(uint8_t latch) {
    battlezone::Sound s;
    s.reset();
    s.set_rate(48000);
    s.write(latch);
    return s;
}

}  // namespace

TEST(Sound, SilentUntilSoundEnable) {
    auto s = fresh(0x80 | 0x10 | 0x0F);
    EXPECT_EQ(rms(render(s, 24000, 0.8f)), 0.0);
}

TEST(Sound, EngineRunsOnlyWithMotorEnable) {
    auto off = fresh(0x20);
    auto on = fresh(0x20 | 0x80);
    EXPECT_LT(rms(render(off, 48000), 4800), 0.002);
    EXPECT_GT(rms(render(on, 48000), 4800), 0.01);
}

TEST(Sound, EngineRevRaisesThe555) {
    auto idle = fresh(0x20 | 0x80);
    auto rev = fresh(0x20 | 0x80 | 0x10);
    render(idle, 96000);
    render(rev, 96000);
    EXPECT_GT(rev.engine_hz(), idle.engine_hz() * 1.8);
    EXPECT_NEAR(idle.engine_hz(), 350, 60);
}

TEST(Sound, ExplosionIsGatedNoise) {
    auto quiet = fresh(0x20);
    auto boom = fresh(0x20 | 0x01);
    EXPECT_GT(rms(render(boom, 24000), 2400), rms(render(quiet, 24000), 2400) + 0.01);
}

TEST(Sound, ShellLoudBitRaisesTheShell) {
    auto soft = fresh(0x20 | 0x04);
    auto loud = fresh(0x20 | 0x04 | 0x08);
    EXPECT_GT(rms(render(loud, 24000), 2400), rms(render(soft, 24000), 2400) * 2);
}

TEST(Sound, ShellDecaysAfterTheEnableDrops) {
    auto s = fresh(0x20 | 0x04 | 0x08);
    render(s, 4800);
    s.write(0x20);
    double early = rms(render(s, 4800));
    render(s, 24000);
    double late = rms(render(s, 4800));
    EXPECT_LT(late, early * 0.3);
}

TEST(Sound, SoundEnableDropDoesNotPop) {
    auto s = fresh(0x20 | 0x80 | 0x04 | 0x08);
    render(s, 24000, 4.9f);
    s.write(0x80 | 0x04 | 0x08);
    auto after = render(s, 4800, 4.9f);
    for (float v : after) EXPECT_EQ(v, 0.0f);
}

TEST(Sound, PokeyPassesThroughTheMix) {
    auto s = fresh(0x20);
    std::vector<float> out;
    for (int i = 0; i < 24000; i++) out.push_back(s.sample((i / 40) % 2 ? 4.9f : 0.88f));
    EXPECT_GT(rms(out, 2400), 0.005);
}
