#include <gtest/gtest.h>

#include "../cache486.h"

using pc486::Cache486;

namespace {

// Core clocks for the bus-clock figures in cache486.h (DX2: 2 per bus clock).
constexpr int T(int bus) { return bus * Cache486::kBusRatio; }

// A far-off time, so earlier traffic has long drained.
constexpr uint64_t kLater = 1'000'000;

}  // namespace

TEST(Cache486Test, AColdReadFillsFromDramThenHitsInTheL1) {
    Cache486 c;
    // DRAM 4-3-3-3 burst plus a row miss (3T + 2T) on the first row touched.
    EXPECT_EQ(c.read(0x1000, 4, true, 0), T(13 + 5));
    EXPECT_TRUE(c.l1_has(0x1000));
    EXPECT_TRUE(c.l2_has(0x1000)) << "the L2 fills alongside the L1";
    EXPECT_EQ(c.read(0x100C, 4, true, kLater), 0) << "same 16-byte line";
}

TEST(Cache486Test, AnL1MissThatHitsTheL2CostsTheL2Burst) {
    Cache486 c;
    c.read(0x2000, 4, true, 0);
    c.invalidate_l1();
    EXPECT_EQ(c.read(0x2000, 4, true, kLater), T(2 + 2 + 2 + 2));
}

TEST(Cache486Test, TheSameDramRowSkipsTheRowMiss) {
    Cache486 c;
    c.read(0x3000, 4, true, 0);
    EXPECT_EQ(c.read(0x3010, 4, true, kLater), T(13)) << "next line, same 4KB row";
    EXPECT_EQ(c.read(0x5000, 4, true, 2 * kLater), T(13 + 5)) << "another row";
}

TEST(Cache486Test, AReadSpanningTwoLinesFillsBoth) {
    Cache486 c;
    int stall = c.read(0x600E, 4, true, 0);
    EXPECT_EQ(stall, T(13 + 5) + T(13));
    EXPECT_TRUE(c.l1_has(0x6000));
    EXPECT_TRUE(c.l1_has(0x6010));
}

TEST(Cache486Test, WithCr0CdSetTheL1AnswersHitsButNeverFills) {
    Cache486 c;
    EXPECT_EQ(c.read(0x7000, 4, false, 0), T(4 + 5)) << "a single non-burst DRAM read";
    EXPECT_FALSE(c.l1_has(0x7000));
    EXPECT_EQ(c.read(0x7000, 4, false, kLater), T(4)) << "still a bus read";
}

TEST(Cache486Test, FifthLineInOneSetEvictsThePseudoLruWay) {
    Cache486 c;
    // Lines 2KB apart share an L1 set (128 sets of 16 bytes).
    for (uint32_t i = 0; i < 4; ++i) c.read(i * 0x800, 1, true, i * kLater);
    c.read(0x0000, 1, true, 5 * kLater);   // way 0 is now the most recent
    c.read(0x2000, 1, true, 6 * kLater);   // a fifth line
    EXPECT_TRUE(c.l1_has(0x0000)) << "the recently used way survives";
    EXPECT_TRUE(c.l1_has(0x2000));
    int resident = 0;
    for (uint32_t i = 0; i < 4; ++i) resident += c.l1_has(i * 0x800) ? 1 : 0;
    EXPECT_EQ(resident, 3) << "exactly one of the four was evicted";
}

TEST(Cache486Test, FourWritesBufferAndTheFifthWaits) {
    Cache486 c;
    c.read(0x8000, 4, true, 0);   // L2 has the line, so each write costs 2T
    uint64_t t = kLater;
    for (int i = 0; i < 4; ++i) EXPECT_EQ(c.write(0x8000, 4, t), 0) << "write " << i;
    // Four 2T writes queue back to back; the fifth waits for the first.
    EXPECT_EQ(c.write(0x8000, 4, t), T(2));
}

TEST(Cache486Test, AMisalignedDwordWriteIsTwoBusCycles) {
    Cache486 c;
    c.read(0x9000, 4, true, 0);
    uint64_t t = kLater;
    EXPECT_EQ(c.write(0x9002, 4, t), 0);
    EXPECT_EQ(c.write(0x9000, 4, t), 0);
    EXPECT_EQ(c.write(0x9000, 4, t), 0);
    EXPECT_EQ(c.write(0x9000, 4, t), T(2)) << "the misaligned write used two of the four entries";
}

TEST(Cache486Test, AReadMissGoesAheadOfBufferedWriteHits) {
    Cache486 c;
    c.read(0xA000 + 0x10000, 4, true, 0);   // 1A000: an L1 line for the writes to hit
    uint64_t t = kLater;
    c.write(0x1A000, 4, t);
    c.write(0x1A000, 4, t);
    // An L2-hit line fill (8T), not delayed by the two buffered writes.
    c.read(0x1B000, 4, true, 0);
    c.invalidate_l1();
    c.read(0x1A000, 4, true, 2 * kLater);
    c.write(0x1A000, 4, 3 * kLater);
    EXPECT_EQ(c.read(0x1B000, 4, true, 3 * kLater), T(8));
}

TEST(Cache486Test, AReadMissWaitsBehindABufferedWriteMiss) {
    Cache486 c;
    c.read(0x1B000, 4, true, 0);   // into the L2
    c.invalidate_l1();
    uint64_t t = kLater;
    // A write to a line the L1 doesn't hold: DRAM, 3T + row miss.
    c.write(0x30000, 4, t);
    EXPECT_EQ(c.read(0x1B000, 4, true, t), T(3 + 5) + T(8));
}

TEST(Cache486Test, AWriteMissLeavesTheL2Alone) {
    Cache486 c;
    c.write(0x40000, 4, 0);
    EXPECT_FALSE(c.l2_has(0x40000)) << "no write-allocate on the SiS 85C471";
    EXPECT_FALSE(c.l1_has(0x40000)) << "nor on the 486";
}

TEST(Cache486Test, AnL2WriteHitMakesTheLineDirtyAndItsEvictionWritesItBack) {
    Cache486 c;
    c.read(0x50000, 4, true, 0);
    c.write(0x50000, 4, kLater);
    c.invalidate_l1();
    // 256KB later maps to the same direct-mapped L2 line, in another row.
    uint32_t alias = 0x50000 + 256 * 1024;
    // Victim write-back (4 x 3T, in the row still open from the first
    // read), then the DRAM burst with a row miss for the new line.
    EXPECT_EQ(c.read(alias, 4, true, 2 * kLater), T(4 * 3) + T(13 + 5));
    EXPECT_TRUE(c.l2_has(alias));
}

TEST(Cache486Test, VgaMemoryIsNeverCachedAndCostsVlBusCycles) {
    Cache486 c;
    EXPECT_EQ(c.read(0xA0000, 1, true, 0), T(2 + 3));
    EXPECT_FALSE(c.l1_has(0xA0000));
    EXPECT_EQ(c.read(0xA0000, 1, true, kLater), T(2 + 3));
    // Writes are buffered: four go out without a stall at 3T each.
    uint64_t t = 2 * kLater;
    for (int i = 0; i < 4; ++i) EXPECT_EQ(c.write(0xA0000 + uint32_t(i), 1, t), 0);
    EXPECT_EQ(c.write(0xA0004, 1, t), T(3));
}

TEST(Cache486Test, AnEightBitIsaPortCostsSixAndAHalfIsaClocks) {
    Cache486 c;
    // 4 wait states (6 ISA clocks) plus half a clock of command delay, at 4
    // bus clocks per 8.33 MHz ISA clock; less the 2 clocks the published
    // IN count already includes.
    EXPECT_EQ(c.io(0x60, 1, false, 0), T(26) - 2);
}

TEST(Cache486Test, ASixteenBitAccessToAnEightBitPortIsTwoIsaCycles) {
    Cache486 c;
    EXPECT_EQ(c.io(0x220, 2, false, 0), T(26 + 8 + 26) - 2) << "two cycles with recovery between";
}

TEST(Cache486Test, TheIdeDataPortIsASixteenBitIsaCycle) {
    Cache486 c;
    EXPECT_EQ(c.io(0x1F0, 2, false, 0), T(14) - 2);
}

TEST(Cache486Test, BackToBackIsaCyclesKeepTheirCommandRecovery) {
    Cache486 c;
    int first = c.io(0x61, 1, true, 0);
    uint64_t done = uint64_t(first + 2);
    EXPECT_EQ(c.io(0x61, 1, true, done), T(8 + 26) - 2);
}

TEST(Cache486Test, VgaPortsAreVlBusCycles) {
    Cache486 c;
    EXPECT_EQ(c.io(0x3C8, 1, true, 0), T(3) - 2);
    EXPECT_EQ(c.io(0x3DA, 1, false, kLater), T(5) - 2);
}

TEST(Cache486Test, PortIoWaitsForTheWriteBufferToDrain) {
    Cache486 c;
    c.read(0xC000, 4, true, 0);
    uint64_t t = kLater;
    c.write(0xC000, 4, t);   // 2T on the bus
    EXPECT_EQ(c.io(0x3C8, 1, true, t), T(2) + T(3) - 2);
}

TEST(Cache486Test, CodeFetchFillsTheL1AndHitsAfter) {
    Cache486 c;
    EXPECT_EQ(c.fetch(0xF0000, true, 0), T(13 + 5));
    EXPECT_EQ(c.fetch(0xF0008, true, kLater), 0);
    EXPECT_TRUE(c.l1_has(0xF0000));
}

TEST(Cache486Test, ResetEmptiesBothCaches) {
    Cache486 c;
    c.read(0xD000, 4, true, 0);
    c.reset();
    EXPECT_FALSE(c.l1_has(0xD000));
    EXPECT_FALSE(c.l2_has(0xD000));
}
