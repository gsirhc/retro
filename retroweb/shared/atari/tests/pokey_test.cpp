#include <gtest/gtest.h>

#include "pokey.h"

#include <cstdint>
#include <vector>

namespace {

std::vector<float> run(atari::Pokey& p, int clocks) {
    std::vector<float> out;
    p.mix(clocks, atari::Pokey::kClockHz, out);
    return out;
}

// Clocks between successive output edges, one sample per chip clock.
std::vector<int> edge_gaps(atari::Pokey& p, int clocks) {
    std::vector<float> out = run(p, clocks);
    std::vector<int> gaps;
    int last = -1;
    for (size_t i = 1; i < out.size(); i++) {
        if ((out[i - 1] > 0.0f) != (out[i] > 0.0f)) {
            if (last >= 0) gaps.push_back(int(i) - last);
            last = int(i);
        }
    }
    return gaps;
}

bool all_equal(const std::vector<int>& g, int want) {
    if (g.size() < 3) return false;
    for (int x : g)
        if (x != want) return false;
    return true;
}

atari::Pokey running() {
    atari::Pokey p;
    p.reset();
    p.write(0x0F, 0x03);
    return p;
}

double settle_pullup(int audc, double vpull) {
    atari::Pokey p;
    p.set_output_rc(10e3, 0.015e-6, vpull);
    p.reset();
    p.write(0x01, uint8_t(audc));
    std::vector<float> out = run(p, 20000);
    return out.back();
}

}  // namespace

TEST(Pokey, PureToneTogglesEveryNTimes28) {
    atari::Pokey p = running();
    p.write(0x00, 9);
    p.write(0x01, 0xAF);
    EXPECT_TRUE(all_equal(edge_gaps(p, 20000), 10 * 28));
}

TEST(Pokey, FifteenKilohertzClockIsDivide114) {
    atari::Pokey p = running();
    p.write(0x08, 0x01);
    p.write(0x02, 3);
    p.write(0x03, 0xA8);
    EXPECT_TRUE(all_equal(edge_gaps(p, 20000), 4 * 114));
}

TEST(Pokey, FastClockOnChannelOneIsAudfPlusFour) {
    atari::Pokey p = running();
    p.write(0x08, 0x40);
    p.write(0x00, 50);
    p.write(0x01, 0xAF);
    EXPECT_TRUE(all_equal(edge_gaps(p, 2000), 54));
}

TEST(Pokey, FastClockOnChannelThreeIsAudfPlusFour) {
    atari::Pokey p = running();
    p.write(0x08, 0x20);
    p.write(0x04, 20);
    p.write(0x05, 0xAF);
    EXPECT_TRUE(all_equal(edge_gaps(p, 2000), 24));
}

TEST(Pokey, JoinedPairOnTheFastClockIsAudf16PlusSeven) {
    atari::Pokey p = running();
    p.write(0x08, 0x50);
    p.write(0x00, 0x34);
    p.write(0x02, 0x01);
    p.write(0x03, 0xAF);
    EXPECT_TRUE(all_equal(edge_gaps(p, 5000), 0x134 + 7));
}

TEST(Pokey, JoinedPairOnTheBaseClockIsAudf16PlusOne) {
    atari::Pokey p = running();
    p.write(0x08, 0x08);
    p.write(0x04, 0x02);
    p.write(0x06, 0x01);
    p.write(0x07, 0xAF);
    EXPECT_TRUE(all_equal(edge_gaps(p, 200000), (0x102 + 1) * 28));
}

TEST(Pokey, VolumeOnlyHoldsTheLevel) {
    atari::Pokey p = running();
    p.write(0x01, 0x17);
    std::vector<float> out = run(p, 500);
    for (float s : out) EXPECT_FLOAT_EQ(s, 7.0f / 60.0f);
}

TEST(Pokey, VolumeBitsCountUnitsWhileTheOutputIsHigh) {
    atari::Pokey p = running();
    p.write(0x01, 0x15);
    p.write(0x03, 0x1A);
    p.write(0x05, 0x13);
    p.write(0x07, 0x1F);
    EXPECT_EQ(p.units(), 5 + 10 + 3 + 15);
}

TEST(Pokey, VolumeZeroIsSilent) {
    atari::Pokey p = running();
    p.write(0x00, 2);
    p.write(0x01, 0xA0);
    for (float s : run(p, 2000)) EXPECT_EQ(s, 0.0f);
}

TEST(Pokey, StimerSetsOneAndTwoHighThreeAndFourLow) {
    atari::Pokey p = running();
    p.write(0x09, 0);
    EXPECT_TRUE(p.channel_out(0));
    EXPECT_TRUE(p.channel_out(1));
    EXPECT_FALSE(p.channel_out(2));
    EXPECT_FALSE(p.channel_out(3));
}

// Channel 3 clocks the D flip-flop; the XOR only passes the edges between samples.
TEST(Pokey, HighPassCancelsAToneSampledFasterThanItMoves) {
    auto high_clocks = [](uint8_t audctl) {
        atari::Pokey p = running();
        p.write(0x08, audctl);
        p.write(0x00, 200);
        p.write(0x01, 0xAF);
        p.write(0x04, 0);
        p.write(0x05, 0xA0);
        int high = 0;
        for (float s : run(p, 20000)) high += s > 0;
        return high;
    };
    int plain = high_clocks(0x60), filtered = high_clocks(0x64);
    EXPECT_GT(plain, 8000);
    EXPECT_LT(filtered, plain / 10);
}

TEST(Pokey, FivePolyGatesTheDivider) {
    atari::Pokey pure = running(), gated = running();
    for (atari::Pokey* p : {&pure, &gated}) p->write(0x00, 0);
    pure.write(0x01, 0xAF);
    gated.write(0x01, 0x2F);
    size_t a = edge_gaps(pure, 30000).size(), b = edge_gaps(gated, 30000).size();
    EXPECT_GT(b, a / 4);
    EXPECT_LT(b, a * 3 / 4);
}

TEST(Pokey, RandomReadsFFInInitMode) {
    atari::Pokey p;
    p.reset();
    run(p, 1000);
    EXPECT_EQ(p.read(0x0A), 0xFF);
}

TEST(Pokey, RandomRunsOnceInitModeEnds) {
    atari::Pokey p = running();
    bool moved = false;
    for (int i = 0; i < 20 && !moved; i++) {
        run(p, 3);
        moved = p.read(0x0A) != 0xFF;
    }
    EXPECT_TRUE(moved);
}

TEST(Pokey, SeventeenBitPolyIsMaximalLength) {
    atari::Pokey p = running();
    std::vector<uint8_t> seq;
    for (int i = 0; i < 200; i++) {
        run(p, 1);
        seq.push_back(p.read(0x0A));
    }
    run(p, 131071 - 200);
    for (int i = 0; i < 200; i++) {
        run(p, 1);
        ASSERT_EQ(p.read(0x0A), seq[size_t(i)]);
    }
}

TEST(Pokey, AllpotReadsThePotLines) {
    atari::Pokey p = running();
    p.allpot = 0x5A;
    EXPECT_EQ(p.read(0x08), 0x5A);
}

TEST(Pokey, PullupIdlesAtTheRail) {
    EXPECT_NEAR(settle_pullup(0x10, 5.0), 5.0, 1e-3);
}

// VOL limits with 10K to 4.75 V, one device of each width on (sh. 27).
TEST(Pokey, PullupMeetsTheDataSheetVolLimits) {
    EXPECT_LE(settle_pullup(0x11, 4.75), 4.2);
    EXPECT_LE(settle_pullup(0x12, 4.75), 3.4);
    EXPECT_LE(settle_pullup(0x14, 4.75), 2.1);
    EXPECT_LE(settle_pullup(0x18, 4.75), 1.2);
    EXPECT_GE(settle_pullup(0x10, 4.75), 4.2);
}

TEST(Pokey, PullupVoltageFallsWithEveryVolumeStep) {
    double last = 6;
    for (int v = 0; v < 16; v++) {
        double now = settle_pullup(0x10 | v, 5.0);
        EXPECT_LT(now, last);
        last = now;
    }
}

TEST(Pokey, TransimpedanceSwingsUpWithSinkCurrent) {
    atari::Pokey p;
    p.set_output_transimpedance(3.3e3, 0.01e-6, 5.0, 10.5);
    p.reset();
    EXPECT_NEAR(run(p, 2000).back(), 5.0, 1e-6);
    p.write(0x01, 0x18);
    EXPECT_NEAR(run(p, 2000).back(), 5.0 + 8 * atari::Pokey::unit_current(5.0) * 3.3e3, 1e-3);
    p.write(0x03, 0x1F);
    p.write(0x05, 0x1F);
    p.write(0x07, 0x1F);
    EXPECT_NEAR(run(p, 2000).back(), 10.5, 1e-6);
}
