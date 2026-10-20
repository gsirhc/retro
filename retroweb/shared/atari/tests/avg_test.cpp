#include <gtest/gtest.h>

#include "avg.h"

#include <array>
#include <cstdint>
#include <vector>

namespace {

using Words = std::vector<uint16_t>;

constexpr uint16_t HALT = 0x2000;
constexpr uint16_t RTSL = 0xC000;
constexpr uint16_t CNTR = 0x8040;

Words vctr(int dx, int dy, int z) {
    return {uint16_t(dy & 0x1FFF), uint16_t(((z & 7) << 13) | (dx & 0x1FFF))};
}
Words svec(int fx, int fy, int z) {
    return {uint16_t(0x4000 | ((fy & 0x1F) << 8) | ((z & 7) << 5) | (fx & 0x1F))};
}
Words stat(int z) { return {uint16_t(0x6400 | (z << 4))}; }
Words corner(bool upper_right, bool outside) {
    return {uint16_t(0x6000 | (upper_right ? 0x200 : 0) | (outside ? 0x100 : 0) | 0xF0)};
}
Words scal(int bin, int lin) { return {uint16_t(0x7000 | (bin << 8) | lin)}; }
Words jsrl(int word) { return {uint16_t(0xA000 | word)}; }
Words jmpl(int word) { return {uint16_t(0xE000 | word)}; }

Words list(std::initializer_list<Words> parts) {
    Words out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

struct Rig {
    std::array<uint8_t, 0x8000> mem{};
    atari::Avg avg;
    atari::Avg::MemRead read = [this](uint16_t a) { return mem[a & 0x7FFF]; };

    Rig() { avg.reset(); }

    void put(int word, const Words& w) {
        for (size_t i = 0; i < w.size(); i++) {
            mem[0x2000 + 2 * (word + i)] = uint8_t(w[i]);
            mem[0x2000 + 2 * (word + i) + 1] = uint8_t(w[i] >> 8);
        }
    }

    int run_to_halt(int limit = 400000) {
        avg.go();
        int n = 0;
        while (!avg.halted() && n < limit) {
            avg.run(1, read);
            n++;
        }
        return n;
    }

    int cycles_for(const Words& w) {
        put(0, w);
        return run_to_halt();
    }
};

constexpr float kCx = atari::Avg::kWidth / 2.0f;
constexpr float kCy = atari::Avg::kHeight / 2.0f;

}  // namespace

TEST(Avg, ResetLeavesItHalted) {
    Rig r;
    EXPECT_TRUE(r.avg.halted());
    r.avg.run(1000, r.read);
    EXPECT_EQ(r.avg.frames, 0);
}

TEST(Avg, HaltPassesFourStates) {
    Rig r;
    r.put(0, {HALT});
    r.avg.go();
    r.avg.run(3, r.read);
    EXPECT_FALSE(r.avg.halted());
    r.avg.run(1, r.read);
    EXPECT_TRUE(r.avg.halted());
}

TEST(Avg, VctrMovesItsCountAtFullSize) {
    Rig r;
    r.put(0, list({{CNTR}, vctr(100, -40, 0), {HALT}}));
    r.run_to_halt();
    EXPECT_EQ(r.avg.x(), 100);
    EXPECT_EQ(r.avg.y(), -40);
}

TEST(Avg, DacTakesBitsTwelveToThree) {
    Rig r;
    r.put(0, list({{CNTR}, vctr(4095, -4096, 0), {HALT}}));
    r.run_to_halt();
    EXPECT_EQ(r.avg.x(), 4088);
    EXPECT_EQ(r.avg.y(), -4096);
}

TEST(Avg, SvecMatchesAVctrOfTwiceItsFields) {
    Rig r;
    r.put(0, list({{CNTR}, svec(5, -3, 0), {HALT}}));
    r.run_to_halt();
    EXPECT_EQ(r.avg.x(), 10);
    EXPECT_EQ(r.avg.y(), -6);
}

TEST(Avg, ZeroZIsABlankMove) {
    Rig r;
    r.put(0, list({{CNTR}, vctr(50, 0, 0), {HALT}}));
    r.run_to_halt();
    EXPECT_TRUE(r.avg.frame.empty());
}

TEST(Avg, ZOneTakesTheStatLevel) {
    Rig r;
    r.put(0, list({{CNTR}, stat(9), vctr(50, 0, 1), {HALT}}));
    r.run_to_halt();
    ASSERT_EQ(r.avg.frame.size(), 1u);
    EXPECT_EQ(r.avg.frame[0].z, 9);
    EXPECT_EQ(r.avg.intensity(), 9);
}

TEST(Avg, OtherZDrivesTheTopThreeBits) {
    Rig r;
    r.put(0, list({{CNTR}, stat(3), vctr(50, 0, 5), vctr(50, 0, 7), {HALT}}));
    r.run_to_halt();
    ASSERT_EQ(r.avg.frame.size(), 2u);
    EXPECT_EQ(r.avg.frame[0].z, 10);
    EXPECT_EQ(r.avg.frame[1].z, 14);
}

TEST(Avg, SegmentsLandInPictureCoordinates) {
    Rig r;
    r.put(0, list({{CNTR}, vctr(-20, 30, 0), vctr(60, 0, 7), {HALT}}));
    r.run_to_halt();
    ASSERT_EQ(r.avg.frame.size(), 1u);
    const atari::AvgSeg& s = r.avg.frame[0];
    EXPECT_FLOAT_EQ(s.x0, kCx - 20);
    EXPECT_FLOAT_EQ(s.y0, kCy - 30);
    EXPECT_FLOAT_EQ(s.x1, kCx + 40);
    EXPECT_FLOAT_EQ(s.y1, kCy - 30);
}

TEST(Avg, BinaryAndLinearScaleShrinkLengths) {
    Rig r;
    r.put(0, list({{CNTR}, scal(2, 0x80), vctr(400, -200, 0), {HALT}}));
    r.run_to_halt();
    EXPECT_EQ(r.avg.x(), 50);
    EXPECT_EQ(r.avg.y(), -25);
    EXPECT_EQ(r.avg.bin_scale(), 2);
    EXPECT_EQ(r.avg.scale(), 0x80);
}

TEST(Avg, CntrReturnsTheBeamToCentre) {
    Rig r;
    r.put(0, list({vctr(300, 200, 0), {CNTR}, {HALT}}));
    r.run_to_halt();
    EXPECT_EQ(r.avg.x(), 0);
    EXPECT_EQ(r.avg.y(), 0);
}

TEST(Avg, BeamTimeFollowsLength) {
    Rig r;
    int lng = r.cycles_for(list({vctr(2048, 0, 0), {HALT}}));
    int sht = r.cycles_for(list({vctr(64, 0, 0), {HALT}}));
    // 12 states, then 2^15 clocks of 12 MHz normalized, or 2^10 after five shifts.
    EXPECT_NEAR(lng, 12 + 32768 / 8, 2);
    EXPECT_NEAR(sht, 12 + 1029 / 8, 2);
}

TEST(Avg, BinaryScaleShortensBeamTimeLinearDoesNot) {
    Rig r;
    int full = r.cycles_for(list({vctr(2048, 0, 0), {HALT}}));
    int bin = r.cycles_for(list({scal(1, 0), vctr(2048, 0, 0), {HALT}}));
    int lin = r.cycles_for(list({scal(0, 0x80), vctr(2048, 0, 0), {HALT}}));
    EXPECT_NEAR(double(bin), full / 2.0, 16);
    EXPECT_NEAR(double(lin), double(full), 16);
}

TEST(Avg, JsrlRtslAndJmplFollowTheList) {
    Rig r;
    r.put(0, list({{CNTR}, jsrl(0x40), jmpl(0x20)}));
    r.put(0x20, list({vctr(0, 25, 0), {HALT}}));
    r.put(0x40, list({vctr(30, 0, 0), {RTSL}}));
    r.run_to_halt();
    EXPECT_EQ(r.avg.x(), 30);
    EXPECT_EQ(r.avg.y(), 25);
}

TEST(Avg, FourNestedSubroutinesReturnInOrder) {
    Rig r;
    r.put(0, list({{CNTR}, jsrl(0x10), vctr(0, 7, 0), {HALT}}));
    r.put(0x10, list({jsrl(0x20), vctr(1, 0, 0), {RTSL}}));
    r.put(0x20, list({jsrl(0x30), vctr(2, 0, 0), {RTSL}}));
    r.put(0x30, list({jsrl(0x40), vctr(4, 0, 0), {RTSL}}));
    r.put(0x40, list({vctr(8, 0, 0), {RTSL}}));
    r.run_to_halt();
    EXPECT_EQ(r.avg.x(), 15);
    EXPECT_EQ(r.avg.y(), 7);
}

TEST(Avg, FrameIsPublishedAtHalt) {
    Rig r;
    r.put(0, list({{CNTR}, vctr(80, 0, 7), {HALT}}));
    r.run_to_halt();
    EXPECT_EQ(r.avg.frames, 1);
    EXPECT_EQ(r.avg.frame.size(), 1u);
    r.run_to_halt();
    EXPECT_EQ(r.avg.frames, 2);
    EXPECT_EQ(r.avg.frame.size(), 1u);
}

TEST(Avg, VgrstHaltsAndClearsStatAndScale) {
    Rig r;
    r.put(0, list({stat(12), scal(3, 0x40), vctr(2048, 0, 0), {HALT}}));
    r.avg.go();
    r.avg.run(40, r.read);
    ASSERT_FALSE(r.avg.halted());
    EXPECT_EQ(r.avg.intensity(), 12);
    r.avg.vgrst();
    EXPECT_TRUE(r.avg.halted());
    EXPECT_EQ(r.avg.intensity(), 0);
    EXPECT_EQ(r.avg.bin_scale(), 0);
    EXPECT_EQ(r.avg.scale(), 0);
    EXPECT_EQ(r.avg.frames, 0);
}

TEST(Avg, GoRestartsAtWordZero) {
    Rig r;
    r.put(0, list({{CNTR}, vctr(10, 0, 0), {HALT}}));
    r.run_to_halt();
    r.run_to_halt();
    EXPECT_EQ(r.avg.x(), 10);
    EXPECT_EQ(r.avg.pc(), 4u);
}

namespace {

Words window(bool outside) {
    return list({{CNTR}, vctr(-50, -50, 0), corner(false, outside),
                 {CNTR}, vctr(50, 50, 0), corner(true, outside)});
}

}  // namespace

TEST(Avg, StatSamplesTheBeamAsAWindowCorner) {
    Rig r;
    r.put(0, list({window(false), {CNTR}, vctr(-100, 0, 0), vctr(200, 0, 7), {HALT}}));
    r.run_to_halt();
    ASSERT_EQ(r.avg.frame.size(), 1u);
    EXPECT_NEAR(r.avg.frame[0].x0, kCx - 50, 0.01);
    EXPECT_NEAR(r.avg.frame[0].x1, kCx + 50, 0.01);
}

TEST(Avg, VectorsOutsideTheWindowVanish) {
    Rig r;
    r.put(0, list({window(false), {CNTR}, vctr(0, 80, 0), vctr(100, 0, 7), {HALT}}));
    r.run_to_halt();
    EXPECT_TRUE(r.avg.frame.empty());
}

TEST(Avg, InOutOneLightsOnlyOutside) {
    Rig r;
    r.put(0, list({window(true), {CNTR}, vctr(-100, 0, 0), vctr(200, 0, 7), {HALT}}));
    r.run_to_halt();
    ASSERT_EQ(r.avg.frame.size(), 2u);
    EXPECT_NEAR(r.avg.frame[0].x0, kCx - 100, 0.01);
    EXPECT_NEAR(r.avg.frame[0].x1, kCx - 50, 0.01);
    EXPECT_NEAR(r.avg.frame[1].x0, kCx + 50, 0.01);
    EXPECT_NEAR(r.avg.frame[1].x1, kCx + 100, 0.01);
}

TEST(Avg, StatWithD10SetLeavesTheWindowAlone) {
    Rig r;
    r.put(0, list({window(false), {CNTR}, vctr(80, 80, 0), stat(15),
                   {CNTR}, vctr(-100, 0, 0), vctr(200, 0, 7), {HALT}}));
    r.run_to_halt();
    ASSERT_EQ(r.avg.frame.size(), 1u);
    EXPECT_NEAR(r.avg.frame[0].x1, kCx + 50, 0.01);
}

TEST(Avg, WindowHoldsAcrossGo) {
    Rig r;
    r.put(0, list({window(false), {HALT}}));
    r.run_to_halt();
    r.put(0, list({{CNTR}, vctr(-100, 0, 0), vctr(200, 0, 7), {HALT}}));
    r.run_to_halt();
    ASSERT_EQ(r.avg.frame.size(), 1u);
    EXPECT_NEAR(r.avg.frame[0].x0, kCx - 50, 0.01);
}
