// GoogleTest suite for cpu65c02::Cpu. This is a light sanity/regression
// suite -- the real correctness gate is Klaus Dormann's 6502/65C02
// functional test suites, run via `make -C .. dormann` (see
// dormann/dormann_host.cpp): both the base 6502 opcode/addressing-mode
// suite and the 65C02-extended-opcode suite (which includes a decimal-mode
// ADC/SBC self-check) pass in full. These tests pin specific, easy-to-read
// behaviors -- especially the 65C02-only additions and the documented CMOS
// fixes over NMOS -- so a regression shows up with a clear opcode name
// instead of "trapped somewhere in a 20-million-instruction suite".

#include <gtest/gtest.h>

#include "../cpu65c02.h"

#include <array>
#include <memory>

using namespace cpu65c02;

namespace {

class Cpu65C02 : public ::testing::Test {
protected:
    std::array<uint8_t, 0x10000> mem{};
    std::unique_ptr<Cpu> cpu;

    void SetUp() override {
        Bus bus;
        bus.read = [this](uint16_t a) { return mem[a]; };
        bus.write = [this](uint16_t a, uint8_t v) { mem[a] = v; };
        cpu = std::make_unique<Cpu>(bus);
        mem[0xFFFC] = 0x00; mem[0xFFFD] = 0x10;   // reset vector -> $1000
        cpu->reset();
    }

    void load(uint16_t addr, std::initializer_list<uint8_t> code) {
        uint16_t a = addr;
        for (uint8_t b : code) mem[a++] = b;
    }
    int run_at(uint16_t addr) { cpu->pc = addr; return cpu->step(); }
};

TEST_F(Cpu65C02, ResetReadsVectorAndClearsDecimal) {
    mem[0x1000] = 0xEA;   // NOP, just to prove PC landed correctly
    EXPECT_EQ(cpu->pc, 0x1000);
    EXPECT_FALSE(cpu->flag(FLAG_D));
    EXPECT_TRUE(cpu->flag(FLAG_I));
    EXPECT_EQ(cpu->sp, 0xFD);
}

TEST_F(Cpu65C02, Bra65C02Unconditional) {
    load(0x1000, {0x80, 0x05});   // BRA +5
    run_at(0x1000);
    EXPECT_EQ(cpu->pc, 0x1007);
}

TEST_F(Cpu65C02, StzZeroesMemoryWithoutTouchingA) {
    cpu->a = 0x42;
    mem[0x0010] = 0xFF;
    load(0x1000, {0x64, 0x10});   // STZ $10
    run_at(0x1000);
    EXPECT_EQ(mem[0x0010], 0x00);
    EXPECT_EQ(cpu->a, 0x42);
}

TEST_F(Cpu65C02, IndirectZpNoIndex) {
    mem[0x0020] = 0x00; mem[0x0021] = 0x30;   // (zp) -> $3000
    mem[0x3000] = 0x77;
    load(0x1000, {0xB2, 0x20});   // LDA ($20)
    run_at(0x1000);
    EXPECT_EQ(cpu->a, 0x77);
}

TEST_F(Cpu65C02, JmpIndirectFixesNmosPageWrapBug) {
    // NMOS reads the high byte from $xx00 instead of wrapping to the next
    // page; the 65C02 fixes this and costs an extra cycle to do it.
    mem[0x30FF] = 0x00; mem[0x3100] = 0x40;   // correct (65C02) target: $4000
    mem[0x3200] = 0xFF;                        // NMOS bug would read this instead -> $FF00
    load(0x1000, {0x6C, 0xFF, 0x30});           // JMP ($30FF)
    int c = run_at(0x1000);
    EXPECT_EQ(cpu->pc, 0x4000);
    EXPECT_EQ(c, 6);
}

TEST_F(Cpu65C02, PhxPlxRoundTrip) {
    cpu->x = 0x99;
    load(0x1000, {0xDA, 0xA2, 0x00, 0xFA});   // PHX; LDX #0; PLX
    run_at(0x1000);   // PHX (1 byte)
    run_at(0x1001);   // LDX #0 (2 bytes)
    run_at(0x1003);   // PLX
    EXPECT_EQ(cpu->x, 0x99);
}

TEST_F(Cpu65C02, BbrBranchesWhenBitClear) {
    mem[0x0030] = 0b11111011;   // bit 2 clear
    load(0x1000, {0x2F, 0x30, 0x05});   // BBR2 $30, +5
    run_at(0x1000);
    EXPECT_EQ(cpu->pc, 0x1008);
}

TEST_F(Cpu65C02, RmbClearsOneBitOnly) {
    mem[0x0040] = 0xFF;
    load(0x1000, {0x47, 0x40});   // RMB4 $40
    run_at(0x1000);
    EXPECT_EQ(mem[0x0040], 0xEF);
}

TEST_F(Cpu65C02, DecimalAdcSetsFlagsFromDecimalResult) {
    // 65C02 fix: N/Z/V reflect the decimal (BCD) result, not the raw
    // pre-adjustment binary sum -- see cpu65c02.cpp's Cpu::adc().
    cpu->p = uint8_t(cpu->p | FLAG_D);
    cpu->a = 0x99;   // decimal 99
    load(0x1000, {0x69, 0x01});   // ADC #$01 (decimal 1) -> 100, so BCD wraps to 00 with carry
    run_at(0x1000);
    EXPECT_EQ(cpu->a, 0x00);
    EXPECT_TRUE(cpu->flag(FLAG_C));
    EXPECT_TRUE(cpu->flag(FLAG_Z));
}

TEST_F(Cpu65C02, IrqRespectsIFlagAndNmiDoesNot) {
    mem[0xFFFE] = 0x00; mem[0xFFFF] = 0x20;   // IRQ vector
    mem[0xFFFA] = 0x00; mem[0xFFFB] = 0x30;   // NMI vector
    load(0x1000, {0xEA});
    cpu->pc = 0x1000;
    cpu->p = uint8_t(cpu->p | FLAG_I);        // mask IRQ
    cpu->irq_line = true;
    cpu->step();                               // NOP retires, IRQ still masked
    EXPECT_EQ(cpu->pc, 0x1001);
    cpu->nmi();                                 // NMI ignores I entirely
    cpu->step();
    EXPECT_EQ(cpu->pc, 0x3000);
}

TEST_F(Cpu65C02, WaiSuspendsUntilIrq) {
    load(0x1000, {0xCB});   // WAI
    run_at(0x1000);
    EXPECT_TRUE(cpu->waiting);
    EXPECT_EQ(cpu->step(), 1);   // still idling
    cpu->irq_line = true;
    cpu->p = uint8_t(cpu->p & ~FLAG_I);
    mem[0xFFFE] = 0x00; mem[0xFFFF] = 0x40;
    cpu->step();
    EXPECT_FALSE(cpu->waiting);
    EXPECT_EQ(cpu->pc, 0x4000);
}

} // namespace
