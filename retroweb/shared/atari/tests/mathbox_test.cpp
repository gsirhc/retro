#include <gtest/gtest.h>

#include "mathbox.h"

#include <cstdint>
#include <vector>

namespace {

enum Src { AQ, AB, ZQ, ZB, ZA, DA, DQ, DZ };
enum Func { ADD, SUBR, SUBS, OR, AND, NOTRS, EXOR, EXNOR };
enum Dest { QREG, NOP, RAMA, RAMF, RAMQD, RAMD, RAMQU, RAMU };

struct U {
    Func func = ADD;
    Src src = ZQ;
    Dest dest = NOP;
    int a = 0, b = 0;
    int src_lo = -1;
    bool stall = false, ldab = false, sign = false, jmp = false, mult = false;
    int cin = 0;
};

uint32_t word(const U& u) {
    int lo = u.src_lo < 0 ? u.src : u.src_lo;
    return uint32_t(u.a & 0xF) << 20 | uint32_t(u.b & 0xF) << 16 |
           uint32_t(u.src >> 2) << 15 | uint32_t(lo >> 2) << 14 | uint32_t(u.src & 3) << 12 |
           uint32_t(u.stall) << 11 | uint32_t(u.func) << 8 | uint32_t(u.ldab) << 7 |
           uint32_t(u.dest) << 4 | uint32_t(u.sign) << 3 | uint32_t(u.jmp) << 2 |
           uint32_t(u.mult) << 1 | uint32_t(u.cin & 1);
}

// Entry n of the mapping PROM points at routine n.
atari::Mathbox box(const std::vector<std::vector<U>>& routines) {
    atari::Mathbox m;
    m.reset();
    int at = 0;
    for (size_t n = 0; n < routines.size(); n++) {
        m.map[n] = uint8_t(at);
        for (const U& u : routines[n]) m.ucode[size_t(at++)] = word(u);
    }
    return m;
}

uint16_t run(atari::Mathbox& m, int offset, uint8_t data = 0) {
    m.write(uint8_t(offset), data);
    m.run(1000);
    return m.y;
}

std::vector<U> load_low(int reg) {
    return {{ADD, ZB, RAMF, 0, reg, DZ}, {ADD, ZB, NOP, 0, reg, -1, true}};
}
std::vector<U> load_high(int reg) {
    return {{ADD, DZ, RAMF, 0, reg, ZB}, {ADD, ZB, NOP, 0, reg, -1, true}};
}

}  // namespace

TEST(Mathbox, HalfWordLoadsKeepTheOtherByte) {
    auto m = box({load_low(3), load_high(3)});
    run(m, 0, 0x34);
    EXPECT_EQ(run(m, 1, 0x12), 0x1234);
    EXPECT_EQ(run(m, 0, 0xCD), 0x12CD);
    EXPECT_EQ(m.r[3], 0x12CD);
}

TEST(Mathbox, BusyUntilStallAtTwoTicksPerCpuCycle) {
    std::vector<U> slow(9, U{ADD, ZQ});
    slow.back().stall = true;
    auto m = box({slow});
    m.write(0, 0);
    EXPECT_TRUE(m.busy());
    EXPECT_EQ(m.status(), 0x80);
    m.run(4);
    EXPECT_TRUE(m.busy());
    m.run(1);
    EXPECT_FALSE(m.busy());
    EXPECT_EQ(m.status(), 0x00);
    EXPECT_EQ(m.steps, 9u);
}

TEST(Mathbox, AWriteRestartsTheSequencer) {
    std::vector<U> slow(40, U{ADD, ZQ});
    slow.back().stall = true;
    auto m = box({slow, load_low(1)});
    m.write(0, 0);
    m.run(3);
    ASSERT_TRUE(m.busy());
    EXPECT_EQ(run(m, 1, 0x55), 0x0055);
    EXPECT_FALSE(m.busy());
}

TEST(Mathbox, SubtractAndLogicFunctions) {
    auto m = box({load_low(0), load_low(1),
                  {{SUBS, AB, NOP, 0, 1, -1, true, false, false, false, false, 1}},
                  {{SUBR, AB, NOP, 0, 1, -1, true, false, false, false, false, 1}},
                  {{AND, AB, NOP, 0, 1, -1, true}},
                  {{OR, AB, NOP, 0, 1, -1, true}},
                  {{NOTRS, AB, NOP, 0, 1, -1, true}},
                  {{EXOR, AB, NOP, 0, 1, -1, true}},
                  {{EXNOR, AB, NOP, 0, 1, -1, true}}});
    run(m, 0, 0x3C);
    run(m, 1, 0x0F);
    EXPECT_EQ(run(m, 2), 0x002D);
    EXPECT_EQ(run(m, 3), uint16_t(0x0F - 0x3C));
    EXPECT_EQ(run(m, 4), 0x000C);
    EXPECT_EQ(run(m, 5), 0x003F);
    EXPECT_EQ(run(m, 6), 0x0003);
    EXPECT_EQ(run(m, 7), 0x0033);
    EXPECT_EQ(run(m, 8), 0xFFCC);
}

TEST(Mathbox, RamaOutputsTheARegister) {
    auto m = box({load_low(2), {{ADD, ZB, RAMA, 2, 5, -1, false}, {ADD, ZB, NOP, 0, 5, -1, true}}});
    run(m, 0, 0x77);
    m.write(1, 0);
    m.step();
    EXPECT_EQ(m.y, 0x0077);
    EXPECT_EQ(m.r[5], 0x0000);
}

TEST(Mathbox, ArithmeticShiftRightUsesTrueSignNotF15) {
    // 0x7000 + 0x7000 overflows to 0xE000; N^V says positive, so 0 shifts in.
    auto m = box({load_high(0), load_high(1),
                  {{ADD, AB, RAMD, 0, 1, -1, false, false, true}, {ADD, ZB, NOP, 0, 1, -1, true}},
                  {{ADD, AB, RAMD, 0, 1, -1, false, false, false}, {ADD, ZB, NOP, 0, 1, -1, true}}});
    run(m, 0, 0x70);
    run(m, 1, 0x70);
    EXPECT_EQ(run(m, 2), 0x7000);
    run(m, 1, 0x80);
    EXPECT_EQ(run(m, 2), 0xF800);  // 0x7000 + 0x8000: no overflow, negative
    m.r[0] = 0x8000;
    m.r[1] = 0x7000;
    EXPECT_EQ(run(m, 3), 0x7800);  // UNSIGN shifts in 0 regardless
}

TEST(Mathbox, RamqdShiftsBThroughQAndLatchesQ0) {
    auto m = box({{{ADD, ZB, QREG, 0, 1}, {ADD, ZB, RAMQD, 0, 2}, {ADD, ZQ, NOP, 0, 0, -1, true}}});
    m.r[1] = 0x0003;
    m.r[2] = 0x0001;
    EXPECT_EQ(run(m, 0), 0x8001);
    EXPECT_EQ(m.r[2], 0x0000);
}

TEST(Mathbox, RamquShiftsQ15IntoBAndForcesQ0Low) {
    auto m = box({{{ADD, ZB, QREG, 0, 1}, {ADD, ZB, RAMQU, 0, 2}, {ADD, ZB, NOP, 0, 2, -1, true}}});
    m.r[1] = 0x8001;
    m.r[2] = 0x4000;
    EXPECT_EQ(run(m, 0), 0x8001);
    EXPECT_EQ(m.q, 0x0002);
}

TEST(Mathbox, CaddSkipsTheAddWhenAZeroFellOutOfQ) {
    // Unsigned 16x16 multiply by repeated CADD/RAMQD, as MBUDOC describes.
    std::vector<U> mul = {{ADD, ZB, QREG, 0, 1}, {AND, ZB, RAMQD, 0, 2}};
    for (int i = 0; i < 16; i++) mul.push_back({ADD, AB, RAMQD, 0, 2, -1, false, false, true, false, true});
    mul.push_back({ADD, ZB, NOP, 0, 2, -1, true});
    auto m = box({mul, {{ADD, ZQ, NOP, 0, 0, -1, true}}});
    m.r[0] = 0x0BCD;
    m.r[1] = 0x00F1;
    uint32_t want = 0x0BCDu * 0x00F1u;
    EXPECT_EQ(run(m, 0), uint16_t(want >> 16));
    EXPECT_EQ(run(m, 1), uint16_t(want));
}

TEST(Mathbox, JplJumpsOnlyWhenTheTrueSignIsPositive) {
    auto m = box({{{ADD, ZQ, NOP, 0, 4, -1, false, true},
                   {SUBS, AB, NOP, 0, 1, -1, false, false, true, true, false, 1},
                   {ADD, ZB, NOP, 0, 1, -1, true},
                   {OR, ZQ, NOP},
                   {ADD, ZB, NOP, 0, 0, -1, true}}});
    m.r[0] = 5;
    m.r[1] = 9;
    EXPECT_EQ(run(m, 0), 9);
    m.r[0] = 0x7FFF;
    m.r[1] = 0x8000;  // overflowing subtract still counts as positive
    EXPECT_EQ(run(m, 0), 0x7FFF);
}

TEST(Mathbox, UnconditionalJumpIgnoresTheSign) {
    auto m = box({{{ADD, ZQ, NOP, 0, 3, -1, false, true},
                   {SUBS, AB, NOP, 0, 1, -1, false, false, false, true, false, 1},
                   {ADD, ZB, NOP, 0, 1, -1, true},
                   {ADD, ZB, NOP, 0, 0, -1, true}}});
    m.r[0] = 1;
    m.r[1] = 9;
    EXPECT_EQ(run(m, 0), 1);
}

TEST(Mathbox, PromsLoadM1AsTheTopNibble) {
    atari::Mathbox m;
    uint8_t map[32] = {7};
    static uint8_t nib[6][256];
    for (int p = 0; p < 6; p++) nib[p][7] = uint8_t(p + 1);
    const uint8_t* ptrs[6] = {nib[0], nib[1], nib[2], nib[3], nib[4], nib[5]};
    m.load_proms(map, ptrs);
    EXPECT_EQ(m.map[0], 7);
    EXPECT_EQ(m.ucode[7], 0x123456u);
}
