#include <gtest/gtest.h>

#include "avg.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

constexpr uint16_t HALT = 0x2000;
constexpr uint16_t RTSL = 0xC000;
constexpr uint16_t CNTR = 0x8040;

std::vector<uint16_t> vctr(int dx, int dy, int z) {
    return {uint16_t(dy & 0x1FFF), uint16_t(((z & 7) << 13) | (dx & 0x1FFF))};
}
uint16_t svec(int dx, int dy, int z) {
    return uint16_t(0x4000 | ((dy & 0x1F) << 8) | ((z & 7) << 5) | (dx & 0x1F));
}
uint16_t stat(int intensity) { return uint16_t(0x6400 | (intensity << 4)); }
uint16_t scal(int bin, int lin) { return uint16_t(0x7000 | (bin << 8) | lin); }
uint16_t jsrl(int word) { return uint16_t(0xA000 | word); }
uint16_t jmpl(int word) { return uint16_t(0xE000 | word); }

struct Rig {
    std::array<uint8_t, 0x8000> mem{};
    atari::Avg avg;
    atari::Avg::MemRead read = [this](uint16_t a) { return mem[a & 0x7FFF]; };

    Rig() { avg.reset(); }

    void put(int word, const std::vector<uint16_t>& words) {
        for (size_t i = 0; i < words.size(); i++) {
            mem[0x2000 + 2 * (word + i)] = uint8_t(words[i]);
            mem[0x2000 + 2 * (word + i) + 1] = uint8_t(words[i] >> 8);
        }
    }

    int run_to_halt(int limit = 200000) {
        avg.go();
        int n = 0;
        while (!avg.halted() && n < limit) {
            avg.run(1, read);
            n++;
        }
        return n;
    }
};

std::vector<uint16_t> cat(std::initializer_list<std::vector<uint16_t>> parts) {
    std::vector<uint16_t> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

constexpr float kCx = atari::Avg::kWidth / 2.0f;
constexpr float kCy = atari::Avg::kHeight / 2.0f;

}  // namespace

TEST(Avg, ResetLeavesItHalted) {
    atari::Avg avg;
    avg.reset();
    EXPECT_TRUE(avg.halted());
}

TEST(Avg, HaltTakesThreeStates) {
    Rig r;
    r.put(0, {HALT});
    r.avg.go();
    r.avg.run(2, r.read);
    EXPECT_FALSE(r.avg.halted());
    r.avg.run(1, r.read);
    EXPECT_TRUE(r.avg.halted());
}

TEST(Avg, VctrMovesOneUnitPerCountAtFullScale) {
    Rig r;
    r.put(0, cat({{CNTR}, vctr(100, 50, 7), {HALT}}));
    r.run_to_halt();
    ASSERT_EQ(r.avg.frame.size(), 1u);
    const auto& s = r.avg.frame[0];
    EXPECT_NEAR(s.x0, kCx, 0.01);
    EXPECT_NEAR(s.y0, kCy, 0.01);
    EXPECT_NEAR(s.x1 - s.x0, 100 * 255.0 / 256, 0.5);
    EXPECT_NEAR(s.y0 - s.y1, 50 * 255.0 / 256, 0.5);  // +dy is up the screen
    EXPECT_EQ(s.z, 14);
}

TEST(Avg, ZeroIntensityIsABlankMove) {
    Rig r;
    r.put(0, cat({{CNTR}, vctr(40, 0, 0), vctr(40, 0, 3), {HALT}}));
    r.run_to_halt();
    ASSERT_EQ(r.avg.frame.size(), 1u);
    EXPECT_NEAR(r.avg.frame[0].x0, kCx + 40 * 255.0 / 256, 0.5);
    EXPECT_EQ(r.avg.frame[0].z, 6);
}

TEST(Avg, IntensityOneUsesTheStatLevel) {
    Rig r;
    r.put(0, cat({{CNTR, stat(9)}, vctr(30, 0, 1), {HALT}}));
    r.run_to_halt();
    ASSERT_EQ(r.avg.frame.size(), 1u);
    EXPECT_EQ(r.avg.frame[0].z, 9);
}

TEST(Avg, SvecCountsDouble) {
    Rig r;
    r.put(0, {CNTR, svec(10, -6, 7), HALT});
    r.run_to_halt();
    ASSERT_EQ(r.avg.frame.size(), 1u);
    EXPECT_NEAR(r.avg.frame[0].x1 - kCx, 20 * 255.0 / 256, 0.5);
    EXPECT_NEAR(r.avg.frame[0].y1 - kCy, 12 * 255.0 / 256, 0.5);
}

TEST(Avg, LinearAndBinaryScaleShrinkVectors) {
    Rig r;
    r.put(0, cat({{CNTR, scal(0, 128)}, vctr(200, 0, 7), {CNTR, scal(2, 0)}, vctr(200, 0, 7), {HALT}}));
    r.run_to_halt();
    ASSERT_EQ(r.avg.frame.size(), 2u);
    EXPECT_NEAR(r.avg.frame[0].x1 - r.avg.frame[0].x0, 200 * 127.0 / 256, 0.5);
    EXPECT_NEAR(r.avg.frame[1].x1 - r.avg.frame[1].x0, 200 * 255.0 / 256 / 4, 0.5);
}

TEST(Avg, CntrReturnsTheBeamToCentre) {
    Rig r;
    r.put(0, cat({vctr(120, 80, 0), {CNTR}, vctr(10, 0, 7), {HALT}}));
    r.run_to_halt();
    ASSERT_EQ(r.avg.frame.size(), 1u);
    EXPECT_NEAR(r.avg.frame[0].x0, kCx, 0.01);
}

TEST(Avg, JsrlRtslAndJmplFollowTheList) {
    Rig r;
    r.put(0, {CNTR, jsrl(0x40), jmpl(0x80)});
    r.put(0x40, cat({vctr(10, 0, 7), {RTSL}}));
    r.put(0x80, cat({vctr(0, 10, 7), {HALT}}));
    r.run_to_halt();
    ASSERT_EQ(r.avg.frame.size(), 2u);
    EXPECT_NEAR(r.avg.frame[1].x0, kCx + 10 * 255.0 / 256, 0.5);
    EXPECT_LT(r.avg.frame[1].y1, r.avg.frame[1].y0);
}

TEST(Avg, NestedSubroutinesReturnInOrder) {
    Rig r;
    r.put(0, cat({{CNTR, jsrl(0x40)}, vctr(0, 20, 7), {HALT}}));
    r.put(0x40, {jsrl(0x60), RTSL});
    r.put(0x60, cat({vctr(20, 0, 7), {RTSL}}));
    r.run_to_halt();
    ASSERT_EQ(r.avg.frame.size(), 2u);
    EXPECT_NEAR(r.avg.frame[0].x1 - kCx, 20 * 255.0 / 256, 0.5);
    EXPECT_NEAR(r.avg.frame[1].x0, r.avg.frame[0].x1, 0.01);
}

TEST(Avg, LongVectorsTakeBeamTime) {
    Rig shortv, longv;
    shortv.put(0, cat({vctr(8, 0, 7), {HALT}}));
    longv.put(0, cat({vctr(4000, 0, 7), {HALT}}));
    int a = shortv.run_to_halt();
    int b = longv.run_to_halt();
    // A full-length vector is 0x8000 master clocks, 4096 CPU cycles.
    EXPECT_GT(b, a + 4000 / 2);
    EXPECT_LT(b, 0x8000 / 8 + 64);
}

TEST(Avg, ScaledDownVectorsDrawFaster) {
    Rig full, quarter;
    full.put(0, cat({vctr(1000, 0, 7), {HALT}}));
    quarter.put(0, cat({{scal(2, 0)}, vctr(1000, 0, 7), {HALT}}));
    EXPECT_GT(full.run_to_halt(), quarter.run_to_halt() * 3);
}

TEST(Avg, FrameIsPublishedAtHalt) {
    Rig r;
    r.put(0, cat({vctr(50, 0, 7), {HALT}}));
    r.run_to_halt();
    EXPECT_EQ(r.avg.frames, 1);
    r.put(0, cat({vctr(50, 0, 7), vctr(0, 50, 7), {HALT}}));
    r.avg.go();
    r.avg.run(4, r.read);
    EXPECT_EQ(r.avg.frame.size(), 1u);  // previous picture until this one halts
    while (!r.avg.halted()) r.avg.run(1, r.read);
    EXPECT_EQ(r.avg.frame.size(), 2u);
}

TEST(Avg, VgrstStopsItMidList) {
    Rig r;
    r.put(0, cat({vctr(4000, 0, 7), {HALT}}));
    r.avg.go();
    r.avg.run(10, r.read);
    ASSERT_FALSE(r.avg.halted());
    r.avg.vgrst();
    EXPECT_TRUE(r.avg.halted());
    EXPECT_EQ(r.avg.scale(), 0);
}

TEST(Avg, ClipWindowComesFromTheBeamAtTheNextOpcode) {
    Rig r;
    // Upper-right then lower-left corner, each latched by the opcode after STAT.
    r.put(0, cat({{CNTR}, vctr(100, 100, 0), {uint16_t(0x6200 | 0xF0), CNTR},
                  vctr(-100, -100, 0), {uint16_t(0x6000 | 0xF0), CNTR},
                  vctr(-200, 0, 0), vctr(400, 0, 7), {HALT}}));
    r.run_to_halt();
    ASSERT_EQ(r.avg.frame.size(), 1u);
    EXPECT_NEAR(r.avg.frame[0].x0, kCx - 100 * 255.0 / 256, 0.5);
    EXPECT_NEAR(r.avg.frame[0].x1, kCx + 100 * 255.0 / 256, 0.5);
}

TEST(Avg, VectorsOutsideTheClipWindowVanish) {
    Rig r;
    r.put(0, cat({{CNTR}, vctr(50, 50, 0), {uint16_t(0x6200 | 0xF0), CNTR},
                  vctr(-50, -50, 0), {uint16_t(0x6000 | 0xF0), CNTR},
                  vctr(0, 100, 0), vctr(30, 0, 7), {HALT}}));
    r.run_to_halt();
    EXPECT_TRUE(r.avg.frame.empty());
}

TEST(Avg, GoClearsTheClipWindow) {
    Rig r;
    r.put(0, cat({{CNTR}, vctr(10, 10, 0), {uint16_t(0x6200 | 0xF0), CNTR},
                  vctr(-10, -10, 0), {uint16_t(0x6000 | 0xF0), CNTR},
                  vctr(0, 100, 0), vctr(30, 0, 7), {HALT}}));
    r.run_to_halt();
    EXPECT_TRUE(r.avg.frame.empty());
    r.put(0, cat({{CNTR}, vctr(0, 100, 0), vctr(30, 0, 7), {HALT}}));
    r.run_to_halt();
    EXPECT_EQ(r.avg.frame.size(), 1u);
}

TEST(Avg, StatWithD10SetLeavesClipAlone) {
    Rig r;
    r.put(0, cat({{CNTR, stat(8), CNTR}, vctr(0, 150, 0), vctr(30, 0, 7), {HALT}}));
    r.run_to_halt();
    EXPECT_EQ(r.avg.frame.size(), 1u);
    EXPECT_EQ(r.avg.intensity(), 8);
}
