// GoogleTest suite for the PC speaker's AND-gate signal and edge log: the
// standard tone-generation combination, the direct-toggle "digitized"
// playback technique (Speaker Data Enable relayed straight through while
// the PIT side is pinned high), edge deduplication, draining, and the
// bounded log's oldest-drops-first overflow behavior.

#include <gtest/gtest.h>

#include "pcspeaker.h"

namespace {

using ibmpcat::PcSpeaker;

TEST(PcSpeakerTest, SignalIsAndOfSpeakerDataEnableAndPitChannel2Output) {
    PcSpeaker sp;
    sp.reset();
    sp.update(100, /*speaker_data_enable=*/true, /*pit_channel2_output=*/false);
    EXPECT_FALSE(sp.level());
    sp.update(200, true, true);
    EXPECT_TRUE(sp.level());
    sp.update(300, false, true);
    EXPECT_FALSE(sp.level());  // data enable gates the speaker regardless of the PIT
}

TEST(PcSpeakerTest, UpdateOnlyRecordsAnEdgeWhenTheLevelActuallyChanges) {
    PcSpeaker sp;
    sp.reset();
    sp.update(10, true, true);   // level false -> true: one edge
    sp.update(20, true, true);   // unchanged: no edge
    sp.update(30, true, true);   // unchanged: no edge
    sp.update(40, true, false);  // true -> false: one edge
    auto edges = sp.drain_edges();
    ASSERT_EQ(edges.size(), 2u);
    EXPECT_EQ(edges[0].cpu_cycle, 10u);
    EXPECT_TRUE(edges[0].level);
    EXPECT_EQ(edges[1].cpu_cycle, 40u);
    EXPECT_FALSE(edges[1].level);
}

TEST(PcSpeakerTest, DrainEdgesReturnsAndClearsTheLog) {
    PcSpeaker sp;
    sp.reset();
    sp.update(1, true, true);
    EXPECT_EQ(sp.drain_edges().size(), 1u);
    EXPECT_TRUE(sp.drain_edges().empty());  // already drained -- nothing left
}

TEST(PcSpeakerTest, DigitizedPlaybackRelaysSpeakerDataEnableWhenPitIsParked) {
    // The direct-toggle technique: gate the PIT off (its output is pinned
    // high the whole time, per pit8253.h's real Mode-3 gate-low behavior)
    // and drive the speaker purely through the Speaker Data Enable bit --
    // the AND gate then just relays that bit's own transitions verbatim.
    PcSpeaker sp;
    sp.reset();
    bool pit_parked_high = true;
    sp.update(0, false, pit_parked_high);
    sp.update(100, true, pit_parked_high);
    sp.update(150, false, pit_parked_high);
    sp.update(220, true, pit_parked_high);
    auto edges = sp.drain_edges();
    ASSERT_EQ(edges.size(), 3u);
    EXPECT_EQ(edges[0].cpu_cycle, 100u); EXPECT_TRUE(edges[0].level);
    EXPECT_EQ(edges[1].cpu_cycle, 150u); EXPECT_FALSE(edges[1].level);
    EXPECT_EQ(edges[2].cpu_cycle, 220u); EXPECT_TRUE(edges[2].level);
}

TEST(PcSpeakerTest, ResetClearsLevelAndPendingEdges) {
    PcSpeaker sp;
    sp.reset();
    sp.update(1, true, true);
    ASSERT_TRUE(sp.level());
    sp.reset();
    EXPECT_FALSE(sp.level());
    EXPECT_TRUE(sp.drain_edges().empty());
}

TEST(PcSpeakerTest, OverflowDropsTheOldestEdgeNotTheNewest) {
    // Real hardware has no such limit -- this just bounds memory for a
    // speaker that's actively playing with nobody draining it (see
    // pcspeaker.h). Prove it drops the oldest transition, not the newest.
    PcSpeaker sp;
    sp.reset();
    constexpr int kMaxEdges = 1 << 16;
    for (int i = 0; i < kMaxEdges + 5; ++i) {
        sp.update(uint64_t(i), (i % 2) == 0, true);  // alternates every call -- always an edge
    }
    auto edges = sp.drain_edges();
    EXPECT_EQ(edges.size(), std::size_t(kMaxEdges));
    // The first 5 edges (cycles 0-4) were dropped to stay within the cap.
    EXPECT_EQ(edges.front().cpu_cycle, 5u);
    EXPECT_EQ(edges.back().cpu_cycle, uint64_t(kMaxEdges + 4));
}

}  // namespace
