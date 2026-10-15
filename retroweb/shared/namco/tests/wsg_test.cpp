#include <gtest/gtest.h>

#include "namco/wsg.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr int kCpuHz = 3072000;

float wsg_peak(namco::Wsg& w) {
    std::vector<float> out;
    w.advance(3072, 48000, out);
    float p = 0;
    for (float s : out) p = std::max(p, std::fabs(s));
    return p;
}

}  // namespace

TEST(Wsg, SilentWhenDisabled) {
    namco::Wsg w;
    w.reset();
    w.regs[0x11] = 1;
    w.regs[0x15] = 0x0F;
    w.wave_prom.fill(0x0F);
    EXPECT_EQ(wsg_peak(w), 0);
    w.enabled = true;
    EXPECT_GT(wsg_peak(w), 0);
}

// Real register map (MAME namco.cpp pacman_sound_w): only 0x05/0x0a/0x0f
// (waveform), 0x10-0x15 (ch0), 0x16-0x1a (ch1), 0x1b-0x1f (ch2) are wired.
TEST(Wsg, RegisterMapMatchesRealHardwareOffsets) {
    namco::Wsg w;
    w.reset();
    w.enabled = true;
    w.wave_prom.fill(0x0F);
    w.write(0x1b, 0x01);
    w.write(0x1f, 0x0F);
    w.write(0x0f, 0x00);
    EXPECT_GT(wsg_peak(w), 0) << "ch2 is audible from its own offsets";

    w.reset();
    w.enabled = true;
    w.write(0x0a, 0x01);
    w.write(0x17, 0x0F);
    w.write(0x07, 0x00);
    EXPECT_EQ(wsg_peak(w), 0) << "ch1 has no volume, and 0x07 is unwired";
}

TEST(Wsg, OnlyVoiceZeroHasLowFrequencyNibble) {
    namco::Wsg w;
    w.reset();
    w.enabled = true;
    w.wave_prom.fill(0x0F);
    w.write(0x15, 0x0F);
    w.write(0x10, 0x01);
    EXPECT_GT(wsg_peak(w), 0);
    w.reset();
    w.enabled = true;
    w.write(0x1a, 0x0F);
    w.write(0x10, 0x01);
    EXPECT_EQ(wsg_peak(w), 0) << "ch1 ignores 0x10";
}

TEST(Wsg, AdvancingWithoutOutputStillTurnsThePhase) {
    namco::Wsg a, b;
    for (namco::Wsg* w : {&a, &b}) {
        w->reset();
        w->enabled = true;
        for (int i = 0; i < 256; i++) w->wave_prom[unsigned(i)] = uint8_t(i & 15);
        w->write(0x13, 0x08);  // freq = 1 << 15: one wave step per WSG clock
        w->write(0x15, 0x0F);
    }
    std::vector<float> none, sa, sb;
    a.advance(32 * 5, 0, none);
    EXPECT_TRUE(none.empty());
    a.advance(32, 96000, sa);
    b.advance(32, 96000, sb);
    ASSERT_EQ(sa.size(), 1u);
    ASSERT_EQ(sb.size(), 1u);
    EXPECT_FLOAT_EQ(sa[0], (6 - 8) * 15 / 360.0f);
    EXPECT_FLOAT_EQ(sb[0], (1 - 8) * 15 / 360.0f);
}

// Frequency writes must not jump the waveform phase.
TEST(Wsg, FrequencyWriteKeepsPhaseContinuous) {
    namco::Wsg w;
    w.reset();
    w.enabled = true;
    for (int i = 0; i < 256; i++) w.wave_prom[unsigned(i)] = uint8_t(i & 15);
    w.write(0x05, 0);
    w.write(0x13, 1);  // freq = 0x1000
    w.write(0x15, 15);
    std::vector<float> a;
    w.advance(kCpuHz / 20, 48000, a);
    ASSERT_GT(a.size(), 10u);
    double step = 0;
    for (size_t i = 1; i < a.size(); i++) step += std::fabs(a[i] - a[i - 1]);
    step /= double(a.size() - 1);
    w.write(0x13, 2);  // freq = 0x2000
    size_t mid = a.size();
    w.advance(kCpuHz / 50, 48000, a);
    ASSERT_GT(a.size(), mid);
    float jump = std::fabs(a[mid] - a[mid - 1]);
    EXPECT_LT(jump, step * 4.0) << "freq write jumped phase (jump=" << jump << " avg step=" << step << ")";
}
