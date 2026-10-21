#include <gtest/gtest.h>

#include "galaxian/timer.h"

namespace {

// 4096 crystal clocks is 512 sound T-states.
constexpr uint64_t kStep = 512;

}  // namespace

TEST(Timer, UnconnectedBitsReadHigh) {
    EXPECT_EQ(galaxian::scramble_port_b(0), 0x0F);
    EXPECT_EQ(galaxian::frogger_port_b(0), 0x27);
}

TEST(Timer, LS93RippleToggles2048ClocksApart) {
    EXPECT_EQ(galaxian::scramble_port_b(255) & 0x10, 0);
    EXPECT_EQ(galaxian::scramble_port_b(256) & 0x10, 0x10);
    EXPECT_EQ(galaxian::scramble_port_b(512) & 0x10, 0);
}

TEST(Timer, BiquinaryStageCountsEvery4096Clocks) {
    EXPECT_EQ(galaxian::scramble_port_b(1 * kStep), 0x0F);
    EXPECT_EQ(galaxian::scramble_port_b(2 * kStep), 0x2F);
    EXPECT_EQ(galaxian::scramble_port_b(3 * kStep), 0x2F);
    EXPECT_EQ(galaxian::scramble_port_b(4 * kStep), 0x4F);
    EXPECT_EQ(galaxian::scramble_port_b(5 * kStep), 0x8F);
    EXPECT_EQ(galaxian::scramble_port_b(9 * kStep), 0xCF);
    EXPECT_EQ(galaxian::scramble_port_b(10 * kStep), 0x0F);
}

TEST(Timer, FroggerWiresStageCOnBit3) {
    EXPECT_EQ(galaxian::frogger_port_b(1 * kStep), 0x27);
    EXPECT_EQ(galaxian::frogger_port_b(2 * kStep), 0x2F);
    EXPECT_EQ(galaxian::frogger_port_b(4 * kStep), 0x67);
    EXPECT_EQ(galaxian::frogger_port_b(5 * kStep), 0xA7);
}
