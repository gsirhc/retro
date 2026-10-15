#include <gtest/gtest.h>

#include "dvg.h"

#include <array>
#include <cstdint>

TEST(Dvg, LabsVecHaltDrawsBox) {
    // Word memory: LABS (312,312) scale 9, four VECs, HALT.
    std::array<uint16_t, 16> mem{};
    mem[0] = 0xA000 | 312;
    mem[1] = 0x9000 | 312;
    auto vec = [](int dx, int dy) {
        uint16_t wy = 0x0000;  // local scale 0 — global is already 9 from LABS
        if (dy < 0) wy |= 0x400 | uint16_t(-dy);
        else wy |= uint16_t(dy);
        uint16_t wx = 0xF000;
        if (dx < 0) wx |= 0x400 | uint16_t(-dx);
        else wx |= uint16_t(dx);
        return std::pair{wy, wx};
    };
    int i = 2;
    for (auto [dx, dy] : {std::pair{400, 0}, {0, 400}, {-400, 0}, {0, -400}}) {
        auto [w0, w1] = vec(dx, dy);
        mem[i++] = w0;
        mem[i++] = w1;
    }
    mem[i] = 0xB000;

    atari::Dvg dvg;
    dvg.go([&](uint16_t a) { return mem[a & 15]; });
    EXPECT_TRUE(dvg.halted());
    int drawn = 0;
    for (auto& s : dvg.segments)
        if (s.intensity) drawn++;
    EXPECT_EQ(drawn, 4);
    EXPECT_NEAR(dvg.x, 312, 2);
    EXPECT_NEAR(dvg.y, 312, 2);
}

TEST(Dvg, JsrFromVectorRomStyle) {
    // Word $0000 list JSRs to $0004 (in-RAM stand-in for vector ROM).
    std::array<uint16_t, 32> mem{};
    mem[0] = 0xA000 | 100;
    mem[1] = 0x9000 | 100;
    mem[2] = 0xC000 | 4;
    mem[3] = 0xB000;
    mem[4] = 0x0000 | 50;   // VEC local0 dy=+50
    mem[5] = 0xF000 | 0;    // bri 15 dx=0
    mem[6] = 0xD000;
    atari::Dvg dvg;
    dvg.go([&](uint16_t a) { return mem[a & 31]; });
    EXPECT_TRUE(dvg.halted());
    EXPECT_EQ(dvg.y, 150);
}

TEST(Dvg, SvecRemapsScaleLikeVectorRom) {
    // computerarcheology VectorROM.html @ $1050: LABS scale 0 at (500,508),
    // then SVEC ss=2 bri=13 x=+3 → dx 24 (3<<8 / 32). Bytes DB F0.
    std::array<uint16_t, 8> mem{};
    mem[0] = 0xA000 | 508;
    mem[1] = 0x0000 | 500;  // global scale 0
    mem[2] = 0xF0DB;
    mem[3] = 0xB000;
    atari::Dvg dvg;
    dvg.go([&](uint16_t a) { return mem[a & 7]; });
    EXPECT_TRUE(dvg.halted());
    ASSERT_FALSE(dvg.segments.empty());
    const auto& s = dvg.segments.front();
    EXPECT_EQ(s.intensity, 13);
    EXPECT_EQ(s.x0, 500);
    EXPECT_EQ(s.y0, 508);
    EXPECT_EQ(s.x1, 524);  // +24
    EXPECT_EQ(s.y1, 508);
}

TEST(Dvg, Scale14PlusLocalWrapsToShipSizedDeltas) {
    // In-game ship uses LABS scale 14 then SVEC/VEC locals; 4-bit sum
    // 14+5 → 3 → /64, not a ×32 blow-up. Ship tip SVEC ss=3 x=2:
    // (2<<8)/64 = 8.
    std::array<uint16_t, 8> mem{};
    mem[0] = 0xA000 | 508;
    mem[1] = 0xE000 | 508;  // global scale 14
    mem[2] = 0xF87A;        // SVEC ss=3 bri=7 x=+2
    mem[3] = 0xB000;
    atari::Dvg dvg;
    dvg.go([&](uint16_t a) { return mem[a & 7]; });
    ASSERT_FALSE(dvg.segments.empty());
    const auto& s = dvg.segments.front();
    EXPECT_EQ(s.x1 - s.x0, 8);
    EXPECT_EQ(s.y1 - s.y0, 0);
}

TEST(Dvg, OffscreenVertexKeepsCursorSoOutlineDoesNotCollapse) {
    // LABS near the left edge, then VEC left (off) and VEC right (back).
    // Scale 9 is 1:1. The second stroke must resume from x=-30, not from
    // a clamped x=0, or the rock's outline slides along the bezel.
    std::array<uint16_t, 16> mem{};
    mem[0] = 0xA000 | 500;
    mem[1] = 0x9000 | 10;
    mem[2] = 0x0000;           // VEC local 0, dy=0
    mem[3] = 0xF000 | 0x400 | 40;  // bri 15, dx=-40 → x=-30
    mem[4] = 0x0000;
    mem[5] = 0xF000 | 80;      // bri 15, dx=+80 → x=50
    mem[6] = 0xB000;
    atari::Dvg dvg;
    dvg.go([&](uint16_t a) { return mem[a & 15]; });
    EXPECT_EQ(dvg.x, 50);
    EXPECT_EQ(dvg.y, 500);
    ASSERT_EQ(dvg.segments.size(), 2u);
    EXPECT_EQ(dvg.segments[0].x0, 10);
    EXPECT_EQ(dvg.segments[0].x1, 0);
    EXPECT_EQ(dvg.segments[1].x0, 0);
    EXPECT_EQ(dvg.segments[1].x1, 50);
}

TEST(Dvg, ZeroLengthBrightVecIsPhotonSpot) {
    // Ship shots are VEC with dx=dy=0 and high brightness.
    std::array<uint16_t, 8> mem{};
    mem[0] = 0xA000 | 400;
    mem[1] = 0x9000 | 500;
    mem[2] = 0x7000;  // VEC local 7, dy=0
    mem[3] = 0xF000;  // bri 15, dx=0
    mem[4] = 0xB000;
    atari::Dvg dvg;
    dvg.go([&](uint16_t a) { return mem[a & 7]; });
    ASSERT_EQ(dvg.segments.size(), 1u);
    EXPECT_EQ(dvg.segments[0].x0, 500);
    EXPECT_EQ(dvg.segments[0].y0, 400);
    EXPECT_EQ(dvg.segments[0].x1, 500);
    EXPECT_EQ(dvg.segments[0].y1, 400);
    EXPECT_EQ(dvg.segments[0].intensity, 15);
}

