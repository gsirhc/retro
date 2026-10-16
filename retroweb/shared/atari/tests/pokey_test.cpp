#include <gtest/gtest.h>

#include "pokey.h"

#include <cstdint>
#include <vector>

namespace {

// Chip clocks between the last two rising edges, sampled once per clock.
int rise_spacing(atari::Pokey& p, int cycles) {
    std::vector<float> out;
    p.mix(cycles, atari::Pokey::kClockHz, out);
    int last = -1, spacing = 0;
    for (size_t i = 1; i < out.size(); i++) {
        if (out[i - 1] == 0.0f && out[i] > 0.0f) {
            if (last >= 0) spacing = int(i) - last;
            last = int(i);
        }
    }
    return spacing;
}

atari::Pokey running() {
    atari::Pokey p;
    p.reset();
    p.write(0x0F, 0x03);
    return p;
}

}  // namespace

TEST(Pokey, PureToneIsSquare) {
    // AUDC $AF = NOTPOLY5|PURE|vol, must toggle the latch, not sample poly.
    atari::Pokey p;
    p.reset();
    p.write(0x0F, 0x03);
    p.write(0x08, 0x00);
    p.write(0x00, 0x20);
    p.write(0x01, 0xAF);
    std::vector<float> out;
    p.mix(atari::Pokey::kClockHz / 20, 48000, out);
    ASSERT_GT(out.size(), 200u);
    int hi = 0, lo = 0, mid = 0;
    for (float s : out) {
        if (s > 0.08f) hi++;
        else if (s < 0.02f) lo++;
        else mid++;
    }
    EXPECT_LT(mid, int(out.size()) / 25);
    EXPECT_GT(hi, int(out.size()) / 8);
    EXPECT_GT(lo, int(out.size()) / 8);
    // Edges should land on a regular grid (square), not poly-noise jitter.
    int edges = 0;
    bool high = out[0] > 0.05f;
    for (size_t i = 1; i < out.size(); i++) {
        bool h = out[i] > 0.05f;
        if (h != high) {
            edges++;
            high = h;
        }
    }
    EXPECT_GT(edges, 20);
    EXPECT_LT(edges, int(out.size()) / 4);
}

// Pure tone, NOTPOLY5: the latch toggles once per period. MAME pokey.cpp:
// AUDF+1 base clocks at 64/15 kHz, AUDF+4 at 1.79 MHz, AUDF+7 when linked.
TEST(Pokey, SixtyFourKilohertzPeriodIsAudfPlusOneTimes28) {
    auto p = running();
    p.write(0x00, 32);
    p.write(0x01, 0xAF);
    EXPECT_EQ(rise_spacing(p, 20000), 2 * 33 * 28);
}

TEST(Pokey, FifteenKilohertzPeriodIsAudfPlusOneTimes114) {
    auto p = running();
    p.write(0x08, 0x01);
    p.write(0x00, 32);
    p.write(0x01, 0xAF);
    EXPECT_EQ(rise_spacing(p, 40000), 2 * 33 * 114);
}

TEST(Pokey, FastClockOnChannelOneIsAudfPlusFour) {
    auto p = running();
    p.write(0x08, 0x40);
    p.write(0x00, 32);
    p.write(0x01, 0xAF);
    EXPECT_EQ(rise_spacing(p, 2000), 2 * 36);
}

TEST(Pokey, FastClockOnChannelThreeIsAudfPlusFour) {
    auto p = running();
    p.write(0x08, 0x20);
    p.write(0x04, 32);
    p.write(0x05, 0xAF);
    EXPECT_EQ(rise_spacing(p, 2000), 2 * 36);
}

TEST(Pokey, LinkedPairOnTheFastClockIsAudfPlusSeven) {
    auto p = running();
    p.write(0x08, 0x50);
    p.write(0x00, 0x34);
    p.write(0x02, 0x12);
    p.write(0x03, 0xAF);
    EXPECT_EQ(rise_spacing(p, 40000), 2 * (0x1234 + 7));
}

TEST(Pokey, LinkedPairOnTheBaseClockIsAudfPlusOneTimes28) {
    auto p = running();
    p.write(0x08, 0x10);
    p.write(0x00, 0x02);
    p.write(0x02, 0x01);
    p.write(0x03, 0xAF);
    EXPECT_EQ(rise_spacing(p, 60000), 2 * (0x0102 + 1) * 28);
}

TEST(Pokey, LinkedChannelsThreeAndFourOnTheBaseClock) {
    auto p = running();
    p.write(0x08, 0x08);
    p.write(0x04, 0x02);
    p.write(0x06, 0x01);
    p.write(0x07, 0xAF);
    EXPECT_EQ(rise_spacing(p, 60000), 2 * (0x0102 + 1) * 28);
}

TEST(Pokey, VolumeOnlyHoldsTheLevel) {
    auto p = running();
    p.write(0x00, 32);
    p.write(0x01, 0x1F);
    std::vector<float> out;
    p.mix(5000, 48000, out);
    ASSERT_FALSE(out.empty());
    for (float s : out) EXPECT_FLOAT_EQ(s, out.front());
    EXPECT_GT(out.front(), 0.0f);
}

TEST(Pokey, VolumeOnlyDrivesTheLevelFromTheFirstClock) {
    auto p = running();
    p.write(0x01, 0x1F);
    std::vector<float> out;
    p.mix(1, atari::Pokey::kClockHz, out);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_GT(out[0], 0.0f);
}

TEST(Pokey, VolumeZeroIsSilent) {
    auto p = running();
    p.write(0x00, 32);
    p.write(0x01, 0xA0);
    std::vector<float> out;
    p.mix(5000, 48000, out);
    for (float s : out) EXPECT_EQ(s, 0.0f);
}

TEST(Pokey, RandomReadsFFInInitMode) {
    atari::Pokey p;
    p.reset();
    p.write(0x0F, 0x00);
    std::vector<float> out;
    p.mix(100, 48000, out);
    EXPECT_EQ(p.read(0x0A), 0xFF);
}

TEST(Pokey, RandomRunsOnceInitModeEnds) {
    atari::Pokey p;
    p.reset();
    p.write(0x0F, 0x00);
    p.write(0x0F, 0x03);
    std::vector<uint8_t> seen;
    std::vector<float> out;
    for (int i = 0; i < 8; i++) {
        p.mix(37, 48000, out);
        seen.push_back(p.read(0x0A));
    }
    int distinct = 0;
    for (size_t i = 1; i < seen.size(); i++) distinct += seen[i] != seen[i - 1];
    EXPECT_GE(distinct, 6);
}

// x^17 + x^12 + 1 is maximal length: 2^17 - 1 steps.
TEST(Pokey, SeventeenBitPolyRepeatsEvery131071Clocks) {
    auto p = running();
    std::vector<float> out;
    std::vector<uint8_t> seq;
    for (int i = 0; i < 131071 + 64; i++) {
        seq.push_back(p.random());
        p.mix(1, 48000, out);
        out.clear();
    }
    for (int i = 0; i < 64; i++) EXPECT_EQ(seq[size_t(i)], seq[size_t(i) + 131071]) << i;
    int same_at_half = 0;
    for (int i = 0; i < 64; i++) same_at_half += seq[size_t(i)] == seq[size_t(i) + 65535];
    EXPECT_LT(same_at_half, 64);
}

// RANDOM bit 0 is the newest LFSR bit: b[t] = b[t-17] ^ b[t-12].
TEST(Pokey, SeventeenBitPolyFeedsBackFromTaps17And12) {
    auto p = running();
    std::vector<float> out;
    std::vector<int> b;
    for (int i = 0; i < 400; i++) {
        p.mix(1, 48000, out);
        b.push_back(~p.random() & 1);
    }
    for (size_t t = 17; t < b.size(); t++) EXPECT_EQ(b[t], b[t - 17] ^ b[t - 12]) << t;
}
