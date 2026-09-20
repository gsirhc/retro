#include <gtest/gtest.h>

#include "cpu_z80.h"

#include <array>
#include <memory>

namespace {

class Z80 : public ::testing::Test {
protected:
    std::array<uint8_t, 0x10000> mem{};
    std::unique_ptr<z80::Cpu> cpu;
    uint8_t irq_byte = 0xFF;

    void SetUp() override {
        z80::Bus bus;
        bus.read = [this](uint16_t a) { return mem[a]; };
        bus.write = [this](uint16_t a, uint8_t v) { mem[a] = v; };
        bus.in = [](uint8_t) { return uint8_t(0xFF); };
        bus.out = [](uint8_t, uint8_t) {};
        bus.irq_data = [this] { return irq_byte; };
        cpu = std::make_unique<z80::Cpu>(bus);
        cpu->reset();
    }

    void run(std::initializer_list<uint8_t> code) {
        uint16_t a = 0;
        for (uint8_t b : code) mem[a++] = b;
        cpu->pc = 0;
        cpu->step();
    }
};

TEST_F(Z80, NopIsFourTStates) {
    run({0x00});
    EXPECT_EQ(cpu->cycles, 4u);
    EXPECT_EQ(cpu->pc, 1);
}

TEST_F(Z80, AddBSetsOverflowNotParity) {
    cpu->a = 0x7F;
    cpu->b = 0x01;
    run({0x80});
    EXPECT_EQ(cpu->a, 0x80);
    EXPECT_TRUE(cpu->flag(z80::FLAG_S));
    EXPECT_TRUE(cpu->flag(z80::FLAG_PV));
    EXPECT_FALSE(cpu->flag(z80::FLAG_C));
}

TEST_F(Z80, SubSetsNAndBorrow) {
    cpu->a = 0x00;
    cpu->b = 0x01;
    run({0x90});
    EXPECT_EQ(cpu->a, 0xFF);
    EXPECT_TRUE(cpu->flag(z80::FLAG_C));
    EXPECT_TRUE(cpu->flag(z80::FLAG_N));
}

TEST_F(Z80, DjnzTakenIs13) {
    cpu->b = 2;
    mem[0] = 0x10;
    mem[1] = 0xFE;  // DJNZ -2 → self
    cpu->pc = 0;
    cpu->step();
    EXPECT_EQ(cpu->b, 1);
    EXPECT_EQ(cpu->pc, 0);
    EXPECT_EQ(cpu->cycles, 13u);
}

TEST_F(Z80, LdirCopiesAndRepeats) {
    mem[0x1000] = 0xAA;
    mem[0x1001] = 0xBB;
    cpu->set_hl(0x1000);
    cpu->set_de(0x2000);
    cpu->set_bc(2);
    mem[0] = 0xED;
    mem[1] = 0xB0;
    cpu->pc = 0;
    while (cpu->bc() != 0) cpu->step();
    EXPECT_EQ(mem[0x2000], 0xAA);
    EXPECT_EQ(mem[0x2001], 0xBB);
    EXPECT_EQ(cpu->bc(), 0);
}

TEST_F(Z80, Im1InterruptVectorsTo38) {
    cpu->im = 1;
    cpu->iff1 = cpu->iff2 = true;
    cpu->sp = 0x8000;
    cpu->pc = 0x1234;
    EXPECT_EQ(cpu->interrupt(), 13);
    EXPECT_EQ(cpu->pc, 0x0038);
    EXPECT_FALSE(cpu->iff1);
}

TEST_F(Z80, IxLoadIndexed) {
    cpu->ix = 0x4000;
    mem[0x4005] = 0x42;
    mem[0] = 0xDD;
    mem[1] = 0x7E;  // LD A,(IX+d)
    mem[2] = 0x05;
    cpu->pc = 0;
    cpu->step();
    EXPECT_EQ(cpu->a, 0x42);
    EXPECT_EQ(cpu->cycles, 19u);
}

TEST_F(Z80, EiDelaysInterruptOneInstruction) {
    cpu->iff1 = cpu->iff2 = false;
    cpu->im = 1;
    cpu->sp = 0x8000;
    mem[0] = 0xFB;  // EI
    mem[1] = 0x00;  // NOP
    cpu->pc = 0;
    cpu->step();
    EXPECT_TRUE(cpu->iff1);
    EXPECT_EQ(cpu->interrupt(), 0);  // delayed
    cpu->step();                     // NOP
    EXPECT_EQ(cpu->interrupt(), 13);
    EXPECT_EQ(cpu->pc, 0x0038);
}

TEST_F(Z80, Im2InterruptReadsVectorTableAtIConcatData) {
    irq_byte = 0xFC;
    cpu->i = 0x3F;
    cpu->im = 2;
    cpu->iff1 = cpu->iff2 = true;
    cpu->sp = 0x8000;
    cpu->pc = 0x1234;
    mem[0x3FFC] = 0x8D;
    mem[0x3FFD] = 0x00;
    EXPECT_EQ(cpu->interrupt(), 19);
    EXPECT_EQ(cpu->pc, 0x008D);
    EXPECT_FALSE(cpu->iff1);
}

TEST_F(Z80, NmiVectorsTo66AndCopiesIff1ToIff2) {
    cpu->iff1 = true;
    cpu->iff2 = false;
    cpu->sp = 0x8000;
    cpu->pc = 0x1234;
    EXPECT_EQ(cpu->nmi(), 11);
    EXPECT_EQ(cpu->pc, 0x0066);
    EXPECT_FALSE(cpu->iff1);
    EXPECT_TRUE(cpu->iff2);
}

}  // namespace
