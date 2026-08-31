// GoogleTest suite for the 8080 arithmetic group: ADD, ADC, SUB, SBB
// (register and immediate forms) and the Zero / Sign / Parity / Carry flags
// they produce. Auxiliary Carry is checked too where the value is well known.
//
// Reference values are taken from the "Intel 8080/8085 Assembly Language
// Programming Manual" flag descriptions and worked examples.

#include <gtest/gtest.h>

#include "i8080.h"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>

namespace {

// Opcodes under test (register operand = B unless noted).
constexpr uint8_t ADD_B = 0x80, ADC_B = 0x88, SUB_B = 0x90, SBB_B = 0x98;
constexpr uint8_t ADD_A = 0x87, SUB_A = 0x97;
constexpr uint8_t ADI = 0xC6, ACI = 0xCE, SUI = 0xD6, SBI = 0xDE;

class Arith : public ::testing::Test {
protected:
    std::array<uint8_t, 0x10000> mem{};
    std::unique_ptr<i8080::Cpu> cpu;

    void SetUp() override {
        i8080::Bus bus;
        bus.read  = [this](uint16_t a) { return mem[a]; };
        bus.write = [this](uint16_t a, uint8_t v) { mem[a] = v; };
        bus.in    = [](uint8_t) -> uint8_t { return 0xFF; };
        bus.out   = [](uint8_t, uint8_t) {};
        cpu = std::make_unique<i8080::Cpu>(bus);
        cpu->reset();
    }

    // Assemble `code` at 0x0000 and execute exactly one instruction.
    void run(std::initializer_list<uint8_t> code) {
        uint16_t addr = 0;
        for (uint8_t byte : code) mem[addr++] = byte;
        cpu->pc = 0;
        cpu->step();
    }

    void setCarry(bool on) {
        if (on) cpu->f |= i8080::FLAG_C;
        else    cpu->f &= static_cast<uint8_t>(~i8080::FLAG_C);
    }

    bool Z()  const { return cpu->flag(i8080::FLAG_Z); }
    bool S()  const { return cpu->flag(i8080::FLAG_S); }
    bool P()  const { return cpu->flag(i8080::FLAG_P); }
    bool CY() const { return cpu->flag(i8080::FLAG_C); }
    bool AC() const { return cpu->flag(i8080::FLAG_AC); }
};

// ---------------------------------------------------------------------------
// ADD
// ---------------------------------------------------------------------------

TEST_F(Arith, AddBasicNoFlags) {
    cpu->a = 0x14;
    cpu->b = 0x22;
    run({ADD_B});
    EXPECT_EQ(cpu->a, 0x36);
    EXPECT_FALSE(Z());
    EXPECT_FALSE(S());
    EXPECT_FALSE(CY());
    EXPECT_TRUE(P());          // 0x36 = 0b0011'0110 -> 4 one-bits -> even
}

TEST_F(Arith, AddZeroResultSetsZeroAndParity) {
    cpu->a = 0x00;
    cpu->b = 0x00;
    run({ADD_B});
    EXPECT_EQ(cpu->a, 0x00);
    EXPECT_TRUE(Z());
    EXPECT_TRUE(P());          // zero one-bits -> even
    EXPECT_FALSE(S());
    EXPECT_FALSE(CY());
}

TEST_F(Arith, AddWrapSetsCarryZeroAndAux) {
    cpu->a = 0xFF;
    cpu->b = 0x01;
    run({ADD_B});
    EXPECT_EQ(cpu->a, 0x00);
    EXPECT_TRUE(CY());         // carry out of bit 7
    EXPECT_TRUE(Z());
    EXPECT_TRUE(AC());         // 0xF + 1 carries out of bit 3
    EXPECT_FALSE(S());
    EXPECT_TRUE(P());
}

TEST_F(Arith, AddIntoBit7SetsSignClearsParity) {
    cpu->a = 0x7F;
    cpu->b = 0x01;
    run({ADD_B});
    EXPECT_EQ(cpu->a, 0x80);
    EXPECT_TRUE(S());
    EXPECT_FALSE(Z());
    EXPECT_FALSE(CY());
    EXPECT_FALSE(P());         // 0x80 -> one bit -> odd
    EXPECT_TRUE(AC());
}

// Worked example from the programming manual: 0x2E + 0x6C = 0x9A.
TEST_F(Arith, AddManualExample) {
    cpu->a = 0x2E;
    cpu->b = 0x6C;
    run({ADD_B});
    EXPECT_EQ(cpu->a, 0x9A);
    EXPECT_TRUE(S());
    EXPECT_FALSE(Z());
    EXPECT_TRUE(P());          // 0x9A -> 4 bits -> even
    EXPECT_FALSE(CY());
    EXPECT_TRUE(AC());         // 0xE + 0xC -> carry out of bit 3
}

TEST_F(Arith, AddAccumulatorToItself) {
    cpu->a = 0xC0;
    run({ADD_A});             // ADD A: A += A
    EXPECT_EQ(cpu->a, 0x80);
    EXPECT_TRUE(CY());
    EXPECT_TRUE(S());
    EXPECT_FALSE(Z());
    EXPECT_FALSE(P());
}

TEST_F(Arith, AddClearsStaleCarry) {
    setCarry(true);
    cpu->a = 0x01;
    cpu->b = 0x01;
    run({ADD_B});
    EXPECT_EQ(cpu->a, 0x02);
    EXPECT_FALSE(CY());       // ADD ignores carry-in and recomputes carry-out
}

TEST_F(Arith, AdiImmediate) {
    cpu->a = 0x14;
    run({ADI, 0x42});
    EXPECT_EQ(cpu->a, 0x56);
    EXPECT_FALSE(CY());
    EXPECT_FALSE(Z());
    EXPECT_FALSE(S());
}

// ---------------------------------------------------------------------------
// ADC
// ---------------------------------------------------------------------------

TEST_F(Arith, AdcAddsCarryIn) {
    cpu->a = 0x14;
    cpu->b = 0x01;
    setCarry(true);
    run({ADC_B});
    EXPECT_EQ(cpu->a, 0x16);
    EXPECT_FALSE(CY());
}

TEST_F(Arith, AdcWithoutCarryIn) {
    cpu->a = 0x14;
    cpu->b = 0x01;
    setCarry(false);
    run({ADC_B});
    EXPECT_EQ(cpu->a, 0x15);
}

TEST_F(Arith, AdcCarryInProducesCarryOut) {
    cpu->a = 0xFF;
    cpu->b = 0x00;
    setCarry(true);
    run({ADC_B});
    EXPECT_EQ(cpu->a, 0x00);
    EXPECT_TRUE(CY());
    EXPECT_TRUE(Z());
    EXPECT_TRUE(AC());
}

// Programming manual example: 0x42 + 0x3D + carry = 0x80.
TEST_F(Arith, AdcManualExample) {
    cpu->a = 0x42;
    cpu->b = 0x3D;
    setCarry(true);
    run({ADC_B});
    EXPECT_EQ(cpu->a, 0x80);
    EXPECT_TRUE(S());
    EXPECT_FALSE(Z());
    EXPECT_FALSE(P());
    EXPECT_FALSE(CY());
    EXPECT_TRUE(AC());        // 0x2 + 0xD + 1 -> carry out of bit 3
}

TEST_F(Arith, AciImmediateWithCarry) {
    cpu->a = 0x00;
    setCarry(true);
    run({ACI, 0xFF});
    EXPECT_EQ(cpu->a, 0x00);
    EXPECT_TRUE(CY());
    EXPECT_TRUE(Z());
}

// ---------------------------------------------------------------------------
// SUB
// ---------------------------------------------------------------------------

TEST_F(Arith, SubBasic) {
    cpu->a = 0x22;
    cpu->b = 0x11;
    run({SUB_B});
    EXPECT_EQ(cpu->a, 0x11);
    EXPECT_FALSE(Z());
    EXPECT_FALSE(S());
    EXPECT_FALSE(CY());        // no borrow
    EXPECT_TRUE(P());          // 0x11 -> two bits -> even
}

TEST_F(Arith, SubEqualOperandsSetsZero) {
    cpu->a = 0x3E;
    cpu->b = 0x3E;
    run({SUB_B});
    EXPECT_EQ(cpu->a, 0x00);
    EXPECT_TRUE(Z());
    EXPECT_TRUE(P());
    EXPECT_FALSE(S());
    EXPECT_FALSE(CY());
}

// Programming manual: "SUB A" zeroes the accumulator, clears carry,
// sets zero and parity, and leaves the auxiliary carry set.
TEST_F(Arith, SubAccumulatorFromItself) {
    cpu->a = 0x3E;
    run({SUB_A});
    EXPECT_EQ(cpu->a, 0x00);
    EXPECT_TRUE(Z());
    EXPECT_TRUE(P());
    EXPECT_FALSE(S());
    EXPECT_FALSE(CY());
    EXPECT_TRUE(AC());
}

TEST_F(Arith, SubUnderflowSetsCarryAndSign) {
    cpu->a = 0x05;
    cpu->b = 0x10;
    run({SUB_B});
    EXPECT_EQ(cpu->a, 0xF5);
    EXPECT_TRUE(CY());         // borrow
    EXPECT_TRUE(S());
    EXPECT_FALSE(Z());
    EXPECT_TRUE(P());          // 0xF5 -> 6 bits -> even
}

// Programming manual SUI example: 0x00 - 0x01 = 0xFF with a borrow.
TEST_F(Arith, SuiZeroMinusOne) {
    cpu->a = 0x00;
    run({SUI, 0x01});
    EXPECT_EQ(cpu->a, 0xFF);
    EXPECT_TRUE(CY());
    EXPECT_TRUE(S());
    EXPECT_TRUE(P());
    EXPECT_FALSE(Z());
    EXPECT_FALSE(AC());        // 0x0 - 0x1 borrows -> no half-carry in the internal add
}

// ---------------------------------------------------------------------------
// SBB
// ---------------------------------------------------------------------------

TEST_F(Arith, SbbSubtractsBorrowIn) {
    cpu->a = 0x10;
    cpu->b = 0x02;
    setCarry(true);
    run({SBB_B});
    EXPECT_EQ(cpu->a, 0x0D);   // 0x10 - 0x02 - 1
    EXPECT_FALSE(CY());
    EXPECT_FALSE(Z());
    EXPECT_FALSE(S());
}

TEST_F(Arith, SbbWithoutBorrowIn) {
    cpu->a = 0x10;
    cpu->b = 0x02;
    setCarry(false);
    run({SBB_B});
    EXPECT_EQ(cpu->a, 0x0E);
}

TEST_F(Arith, SbbBorrowInProducesBorrowOut) {
    cpu->a = 0x00;
    cpu->b = 0x00;
    setCarry(true);
    run({SBB_B});
    EXPECT_EQ(cpu->a, 0xFF);   // 0 - 0 - 1
    EXPECT_TRUE(CY());
    EXPECT_TRUE(S());
    EXPECT_TRUE(P());
    EXPECT_FALSE(Z());
}

// Programming manual SBB example: 0x04 - 0x02 - carry = 0x01.
TEST_F(Arith, SbbManualExample) {
    cpu->a = 0x04;
    cpu->b = 0x02;
    setCarry(true);
    run({SBB_B});
    EXPECT_EQ(cpu->a, 0x01);
    EXPECT_FALSE(CY());
    EXPECT_FALSE(Z());
    EXPECT_FALSE(S());
    EXPECT_FALSE(P());         // one bit -> odd
    EXPECT_TRUE(AC());
}

TEST_F(Arith, SbiImmediate) {
    cpu->a = 0x20;
    setCarry(true);
    run({SBI, 0x10});
    EXPECT_EQ(cpu->a, 0x0F);   // 0x20 - 0x10 - 1
    EXPECT_FALSE(CY());
    EXPECT_FALSE(Z());
    EXPECT_FALSE(S());
}

// ---------------------------------------------------------------------------
// Flag-focused parameterised checks
// ---------------------------------------------------------------------------

TEST_F(Arith, ParityIsEvenParityOfResult) {
    struct Case { uint8_t addend; uint8_t result; bool parityFlag; };
    const Case cases[] = {
        {0x00, 0x00, true},    // 0 one-bits
        {0x01, 0x01, false},   // 1
        {0x03, 0x03, true},    // 2
        {0x07, 0x07, false},   // 3
        {0x0F, 0x0F, true},    // 4
        {0xAA, 0xAA, true},    // 4
        {0x1F, 0x1F, false},   // 5
        {0xFF, 0xFF, true},    // 8
    };
    for (const Case &c : cases) {
        cpu->reset();
        cpu->a = 0x00;
        cpu->b = c.addend;
        run({ADD_B});
        EXPECT_EQ(cpu->a, c.result);
        EXPECT_EQ(P(), c.parityFlag)
            << "addend=0x" << std::hex << static_cast<int>(c.addend);
    }
}

TEST_F(Arith, SignFlagTracksBit7) {
    cpu->a = 0x7F;
    cpu->b = 0x00;
    run({ADD_B});
    EXPECT_FALSE(S());

    cpu->reset();
    cpu->a = 0x80;
    cpu->b = 0x00;
    run({ADD_B});
    EXPECT_TRUE(S());
}

TEST_F(Arith, ZeroFlagOnlyWhenAllBitsClear) {
    cpu->a = 0x01;
    cpu->b = 0xFF;
    run({ADD_B});             // 0x100 -> 0x00
    EXPECT_TRUE(Z());

    cpu->reset();
    cpu->a = 0x01;
    cpu->b = 0xFE;
    run({ADD_B});             // 0xFF
    EXPECT_FALSE(Z());
}

TEST_F(Arith, CarryFlagIsBorrowForSubtraction) {
    cpu->a = 0x00;
    cpu->b = 0x01;
    run({SUB_B});
    EXPECT_TRUE(CY());        // 0 - 1 borrows

    cpu->reset();
    cpu->a = 0x01;
    cpu->b = 0x00;
    run({SUB_B});
    EXPECT_FALSE(CY());       // 1 - 0 does not
}

} // namespace
