// GoogleTest suite for z80::Cpu. Named regressions for UM0080 groups so a
// failure shows an opcode, not "trapped somewhere in zexdoc". The
// independent correctness gate is Frank Cringle's zexdoc exerciser
// (`make -C .. zexdoc`).

#include <gtest/gtest.h>

#include "cpu_z80.h"

#include <array>
#include <memory>
#include <utility>
#include <vector>

namespace {

class Z80 : public ::testing::Test {
protected:
    std::array<uint8_t, 0x10000> mem{};
    std::unique_ptr<z80::Cpu> cpu;
    uint8_t irq_byte = 0xFF;
    uint8_t in_val = 0xFF;
    uint8_t last_out_port = 0;
    uint8_t last_out_val = 0;
    std::vector<std::pair<uint8_t, uint8_t>> outs;

    void SetUp() override {
        z80::Bus bus;
        bus.read = [this](uint16_t a) { return mem[a]; };
        bus.write = [this](uint16_t a, uint8_t v) { mem[a] = v; };
        bus.in = [this](uint8_t) { return in_val; };
        bus.out = [this](uint8_t p, uint8_t v) {
            last_out_port = p;
            last_out_val = v;
            outs.push_back({p, v});
        };
        bus.irq_data = [this] { return irq_byte; };
        cpu = std::make_unique<z80::Cpu>(bus);
        cpu->reset();
    }

    void load(std::initializer_list<uint8_t> code, uint16_t addr = 0) {
        uint16_t a = addr;
        for (uint8_t b : code) mem[a++] = b;
    }

    void run(std::initializer_list<uint8_t> code) {
        load(code);
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

// --- 8-bit load / ALU (UM0080) -------------------------------------------

TEST_F(Z80, LdImmediateAndHl) {
    cpu->set_hl(0x4000);
    load({0x3E, 0x5A, 0x77});  // LD A,n / LD (HL),A
    cpu->pc = 0;
    cpu->step();
    cpu->step();
    EXPECT_EQ(cpu->a, 0x5A);
    EXPECT_EQ(mem[0x4000], 0x5A);
}

TEST_F(Z80, LdFromBcDeAndAbsolute) {
    mem[0x1234] = 0x11;
    mem[0x5678] = 0x22;
    mem[0x9ABC] = 0x33;
    cpu->set_bc(0x1234);
    cpu->set_de(0x5678);
    load({0x0A, 0x1A, 0x3A, 0xBC, 0x9A});  // LD A,(BC) / (DE) / (nn)
    cpu->pc = 0;
    cpu->step();
    EXPECT_EQ(cpu->a, 0x11);
    cpu->step();
    EXPECT_EQ(cpu->a, 0x22);
    cpu->step();
    EXPECT_EQ(cpu->a, 0x33);
    EXPECT_EQ(cpu->cycles, 7u + 7u + 13u);
}

TEST_F(Z80, AndSetsHClearsNCUsesParity) {
    cpu->a = 0xFF;
    run({0xE6, 0x0F});  // AND n
    EXPECT_EQ(cpu->a, 0x0F);
    EXPECT_TRUE(cpu->flag(z80::FLAG_H));
    EXPECT_FALSE(cpu->flag(z80::FLAG_N));
    EXPECT_FALSE(cpu->flag(z80::FLAG_C));
    EXPECT_TRUE(cpu->flag(z80::FLAG_PV));  // 4 bits, even parity
}

TEST_F(Z80, XorAClearsAccumulator) {
    cpu->a = 0xA5;
    run({0xAF});
    EXPECT_EQ(cpu->a, 0);
    EXPECT_TRUE(cpu->flag(z80::FLAG_Z));
    EXPECT_FALSE(cpu->flag(z80::FLAG_H));
    EXPECT_FALSE(cpu->flag(z80::FLAG_N));
    EXPECT_TRUE(cpu->flag(z80::FLAG_PV));
}

TEST_F(Z80, OrSetsBitsAndParity) {
    cpu->a = 0x0F;
    cpu->b = 0xF0;
    run({0xB0});
    EXPECT_EQ(cpu->a, 0xFF);
    EXPECT_TRUE(cpu->flag(z80::FLAG_S));
    EXPECT_TRUE(cpu->flag(z80::FLAG_PV));
}

TEST_F(Z80, CpDoesNotChangeA) {
    cpu->a = 0x10;
    cpu->b = 0x10;
    run({0xB8});
    EXPECT_EQ(cpu->a, 0x10);
    EXPECT_TRUE(cpu->flag(z80::FLAG_Z));
    EXPECT_TRUE(cpu->flag(z80::FLAG_N));
}

TEST_F(Z80, AdcUsesCarry) {
    cpu->a = 0x10;
    cpu->b = 0x20;
    cpu->f = z80::FLAG_C;
    run({0x88});
    EXPECT_EQ(cpu->a, 0x31);
    EXPECT_FALSE(cpu->flag(z80::FLAG_C));
}

TEST_F(Z80, SbcUsesBorrow) {
    cpu->a = 0x10;
    cpu->b = 0x01;
    cpu->f = z80::FLAG_C;
    run({0x98});
    EXPECT_EQ(cpu->a, 0x0E);
    EXPECT_TRUE(cpu->flag(z80::FLAG_N));
}

TEST_F(Z80, IncOverflowAt7F) {
    cpu->b = 0x7F;
    run({0x04});
    EXPECT_EQ(cpu->b, 0x80);
    EXPECT_TRUE(cpu->flag(z80::FLAG_PV));
    EXPECT_TRUE(cpu->flag(z80::FLAG_S));
    EXPECT_FALSE(cpu->flag(z80::FLAG_N));
}

TEST_F(Z80, DecSetsNAndHalfBorrow) {
    cpu->b = 0x10;
    run({0x05});
    EXPECT_EQ(cpu->b, 0x0F);
    EXPECT_TRUE(cpu->flag(z80::FLAG_N));
    EXPECT_TRUE(cpu->flag(z80::FLAG_H));
}

TEST_F(Z80, DaaAfterAddAdjustsDecimal) {
    cpu->a = 0x15;
    cpu->b = 0x27;
    load({0x80, 0x27});  // ADD A,B / DAA
    cpu->pc = 0;
    cpu->step();
    EXPECT_EQ(cpu->a, 0x3C);
    cpu->step();
    EXPECT_EQ(cpu->a, 0x42);
}

TEST_F(Z80, CplInvertsASetsHN) {
    cpu->a = 0xAA;
    run({0x2F});
    EXPECT_EQ(cpu->a, 0x55);
    EXPECT_TRUE(cpu->flag(z80::FLAG_H));
    EXPECT_TRUE(cpu->flag(z80::FLAG_N));
}

TEST_F(Z80, ScfSetsCarryOnly) {
    cpu->a = 0x00;
    cpu->f = z80::FLAG_N | z80::FLAG_H;
    run({0x37});
    EXPECT_TRUE(cpu->flag(z80::FLAG_C));
    EXPECT_FALSE(cpu->flag(z80::FLAG_N));
    EXPECT_FALSE(cpu->flag(z80::FLAG_H));
}

TEST_F(Z80, CcfTogglesCarry) {
    cpu->f = z80::FLAG_C;
    run({0x3F});
    EXPECT_FALSE(cpu->flag(z80::FLAG_C));
    EXPECT_TRUE(cpu->flag(z80::FLAG_H));  // previous C copied to H
}

// --- 16-bit / stack / exchange -------------------------------------------

TEST_F(Z80, LdHlImmediateAndAddBc) {
    cpu->set_bc(0x0002);
    load({0x21, 0x00, 0x10, 0x09});  // LD HL,nn / ADD HL,BC
    cpu->pc = 0;
    cpu->step();
    cpu->step();
    EXPECT_EQ(cpu->hl(), 0x1002);
    EXPECT_EQ(cpu->cycles, 10u + 11u);
}

TEST_F(Z80, PushPopAfRoundTrip) {
    cpu->a = 0x12;
    cpu->f = 0x34;
    cpu->sp = 0x8000;
    load({0xF5, 0x3E, 0x00, 0xF1});  // PUSH AF / LD A,0 / POP AF
    cpu->pc = 0;
    cpu->step();
    cpu->step();
    cpu->step();
    EXPECT_EQ(cpu->a, 0x12);
    EXPECT_EQ(cpu->f, 0x34);
    EXPECT_EQ(cpu->sp, 0x8000);
}

TEST_F(Z80, ExDeHl) {
    cpu->set_de(0x1111);
    cpu->set_hl(0x2222);
    run({0xEB});
    EXPECT_EQ(cpu->de(), 0x2222);
    EXPECT_EQ(cpu->hl(), 0x1111);
}

TEST_F(Z80, ExxSwapsBcDeHl) {
    cpu->set_bc(0x0102);
    cpu->set_de(0x0304);
    cpu->set_hl(0x0506);
    cpu->b_ = 0xA1; cpu->c_ = 0xA2;
    cpu->d_ = 0xB1; cpu->e_ = 0xB2;
    cpu->h_ = 0xC1; cpu->l_ = 0xC2;
    run({0xD9});
    EXPECT_EQ(cpu->bc(), 0xA1A2);
    EXPECT_EQ(cpu->de(), 0xB1B2);
    EXPECT_EQ(cpu->hl(), 0xC1C2);
    EXPECT_EQ(cpu->b_, 0x01);
    EXPECT_EQ(cpu->c_, 0x02);
}

TEST_F(Z80, ExAfAfPrime) {
    cpu->a = 0x11; cpu->f = 0x22;
    cpu->a_ = 0x33; cpu->f_ = 0x44;
    run({0x08});
    EXPECT_EQ(cpu->a, 0x33);
    EXPECT_EQ(cpu->f, 0x44);
    EXPECT_EQ(cpu->a_, 0x11);
}

TEST_F(Z80, ExSpHl) {
    cpu->set_hl(0xABCD);
    cpu->sp = 0x4000;
    mem[0x4000] = 0x34;
    mem[0x4001] = 0x12;
    run({0xE3});
    EXPECT_EQ(cpu->hl(), 0x1234);
    EXPECT_EQ(mem[0x4000], 0xCD);
    EXPECT_EQ(mem[0x4001], 0xAB);
    EXPECT_EQ(cpu->cycles, 19u);
}

TEST_F(Z80, LdSpHl) {
    cpu->set_hl(0xBEEF);
    run({0xF9});
    EXPECT_EQ(cpu->sp, 0xBEEF);
    EXPECT_EQ(cpu->cycles, 6u);
}

// --- control flow --------------------------------------------------------

TEST_F(Z80, JrTakenIs12UntakenIs7) {
    load({0x18, 0x02, 0x00, 0x00, 0x00});  // JR +2
    cpu->pc = 0;
    cpu->step();
    EXPECT_EQ(cpu->pc, 4);
    EXPECT_EQ(cpu->cycles, 12u);

    cpu->reset();
    cpu->f = 0;  // NZ
    load({0x28, 0x02});  // JR Z,+2 not taken
    cpu->pc = 0;
    cpu->step();
    EXPECT_EQ(cpu->pc, 2);
    EXPECT_EQ(cpu->cycles, 7u);
}

TEST_F(Z80, DjnzNotTakenIs8) {
    cpu->b = 1;
    load({0x10, 0x00});
    cpu->pc = 0;
    cpu->step();
    EXPECT_EQ(cpu->b, 0);
    EXPECT_EQ(cpu->pc, 2);
    EXPECT_EQ(cpu->cycles, 8u);
}

TEST_F(Z80, CallAndRet) {
    cpu->sp = 0x8000;
    load({0xCD, 0x00, 0x20});  // CALL $2000
    mem[0x2000] = 0xC9;        // RET
    cpu->pc = 0;
    cpu->step();
    EXPECT_EQ(cpu->pc, 0x2000);
    EXPECT_EQ(cpu->sp, 0x7FFE);
    EXPECT_EQ(cpu->cycles, 17u);
    cpu->step();
    EXPECT_EQ(cpu->pc, 3);
    EXPECT_EQ(cpu->sp, 0x8000);
}

TEST_F(Z80, Rst38PushesAndVectors) {
    cpu->sp = 0x8000;
    cpu->pc = 0x1234;
    mem[0x1234] = 0xFF;  // RST 38
    cpu->step();
    EXPECT_EQ(cpu->pc, 0x0038);
    EXPECT_EQ(mem[0x7FFE], 0x35);
    EXPECT_EQ(mem[0x7FFF], 0x12);
    EXPECT_EQ(cpu->cycles, 11u);
}

TEST_F(Z80, JpHl) {
    cpu->set_hl(0x4000);
    run({0xE9});
    EXPECT_EQ(cpu->pc, 0x4000);
}

TEST_F(Z80, HaltStopsUntilInterrupt) {
    cpu->im = 1;
    cpu->iff1 = cpu->iff2 = true;
    cpu->sp = 0x8000;
    load({0x76});
    cpu->pc = 0;
    EXPECT_EQ(cpu->step(), 4);
    EXPECT_TRUE(cpu->halted);
    EXPECT_EQ(cpu->step(), 4);  // NOP-like while halted
    EXPECT_EQ(cpu->interrupt(), 13);
    EXPECT_FALSE(cpu->halted);
    EXPECT_EQ(cpu->pc, 0x0038);
}

// --- CB rotates / bit ----------------------------------------------------

TEST_F(Z80, RlcAThroughCarry) {
    cpu->a = 0x80;
    run({0xCB, 0x07});
    EXPECT_EQ(cpu->a, 0x01);
    EXPECT_TRUE(cpu->flag(z80::FLAG_C));
    EXPECT_EQ(cpu->cycles, 8u);
}

TEST_F(Z80, SrlClearsSign) {
    cpu->a = 0x81;
    run({0xCB, 0x3F});
    EXPECT_EQ(cpu->a, 0x40);
    EXPECT_TRUE(cpu->flag(z80::FLAG_C));
    EXPECT_FALSE(cpu->flag(z80::FLAG_S));
}

TEST_F(Z80, Bit7ASetsSignWhenSet) {
    cpu->a = 0x80;
    run({0xCB, 0x7F});
    EXPECT_TRUE(cpu->flag(z80::FLAG_S));
    EXPECT_FALSE(cpu->flag(z80::FLAG_Z));
    EXPECT_TRUE(cpu->flag(z80::FLAG_H));
    EXPECT_EQ(cpu->a, 0x80);
}

TEST_F(Z80, SetAndResOnHl) {
    cpu->set_hl(0x4000);
    mem[0x4000] = 0x00;
    load({0xCB, 0xC6, 0xCB, 0x86});  // SET 0,(HL) / RES 0,(HL)
    cpu->pc = 0;
    cpu->step();
    EXPECT_EQ(mem[0x4000], 0x01);
    EXPECT_EQ(cpu->cycles, 15u);
    cpu->step();
    EXPECT_EQ(mem[0x4000], 0x00);
}

TEST_F(Z80, SlaShiftsZeroIn) {
    cpu->a = 0x40;
    run({0xCB, 0x27});
    EXPECT_EQ(cpu->a, 0x80);
    EXPECT_FALSE(cpu->flag(z80::FLAG_C));
}

TEST_F(Z80, SraPreservesSign) {
    cpu->a = 0x80;
    run({0xCB, 0x2F});
    EXPECT_EQ(cpu->a, 0xC0);
    EXPECT_FALSE(cpu->flag(z80::FLAG_C));
}

TEST_F(Z80, RlcaDoesNotAffectSzp) {
    cpu->a = 0x80;
    cpu->f = z80::FLAG_Z | z80::FLAG_PV | z80::FLAG_S;
    run({0x07});
    EXPECT_EQ(cpu->a, 0x01);
    EXPECT_TRUE(cpu->flag(z80::FLAG_C));
    EXPECT_TRUE(cpu->flag(z80::FLAG_Z));  // preserved
    EXPECT_EQ(cpu->cycles, 4u);
}

// --- ED block / 16-bit ALU / I/O -----------------------------------------

TEST_F(Z80, AdcHlBc) {
    cpu->set_hl(0x7FFF);
    cpu->set_bc(0x0001);
    cpu->f = 0;
    run({0xED, 0x4A});
    EXPECT_EQ(cpu->hl(), 0x8000);
    EXPECT_TRUE(cpu->flag(z80::FLAG_PV));
    EXPECT_TRUE(cpu->flag(z80::FLAG_S));
    EXPECT_EQ(cpu->cycles, 15u);
}

TEST_F(Z80, SbcHlBc) {
    cpu->set_hl(0x0000);
    cpu->set_bc(0x0001);
    cpu->f = 0;
    run({0xED, 0x42});
    EXPECT_EQ(cpu->hl(), 0xFFFF);
    EXPECT_TRUE(cpu->flag(z80::FLAG_C));
    EXPECT_TRUE(cpu->flag(z80::FLAG_N));
}

TEST_F(Z80, Neg) {
    cpu->a = 0x01;
    run({0xED, 0x44});
    EXPECT_EQ(cpu->a, 0xFF);
    EXPECT_TRUE(cpu->flag(z80::FLAG_C));
    EXPECT_TRUE(cpu->flag(z80::FLAG_N));
}

TEST_F(Z80, LdiCopiesOnceWithoutRepeat) {
    mem[0x1000] = 0x99;
    cpu->set_hl(0x1000);
    cpu->set_de(0x2000);
    cpu->set_bc(2);
    run({0xED, 0xA0});
    EXPECT_EQ(mem[0x2000], 0x99);
    EXPECT_EQ(cpu->hl(), 0x1001);
    EXPECT_EQ(cpu->de(), 0x2001);
    EXPECT_EQ(cpu->bc(), 1);
    EXPECT_EQ(cpu->cycles, 16u);
}

TEST_F(Z80, CpiComparesAndLeavesA) {
    cpu->a = 0x42;
    mem[0x1000] = 0x42;
    cpu->set_hl(0x1000);
    cpu->set_bc(1);
    run({0xED, 0xA1});
    EXPECT_EQ(cpu->a, 0x42);
    EXPECT_TRUE(cpu->flag(z80::FLAG_Z));
    EXPECT_EQ(cpu->hl(), 0x1001);
    EXPECT_EQ(cpu->bc(), 0);
}

TEST_F(Z80, RldRotatesNibbles) {
    cpu->a = 0x12;
    cpu->set_hl(0x4000);
    mem[0x4000] = 0x34;
    run({0xED, 0x6F});
    EXPECT_EQ(cpu->a, 0x13);
    EXPECT_EQ(mem[0x4000], 0x42);
    EXPECT_EQ(cpu->cycles, 18u);
}

TEST_F(Z80, InAFromPort) {
    in_val = 0xAB;
    run({0xDB, 0x40});
    EXPECT_EQ(cpu->a, 0xAB);
    EXPECT_EQ(cpu->cycles, 11u);
}

TEST_F(Z80, OutAToPort) {
    cpu->a = 0xCD;
    run({0xD3, 0x12});
    EXPECT_EQ(last_out_port, 0x12);
    EXPECT_EQ(last_out_val, 0xCD);
    EXPECT_EQ(cpu->cycles, 11u);
}

TEST_F(Z80, InCSetsFlags) {
    cpu->c = 0x10;
    in_val = 0x00;
    run({0xED, 0x78});  // IN A,(C)
    EXPECT_EQ(cpu->a, 0x00);
    EXPECT_TRUE(cpu->flag(z80::FLAG_Z));
    EXPECT_EQ(cpu->cycles, 12u);
}

TEST_F(Z80, LdIAAndLdAI) {
    cpu->a = 0x3F;
    load({0xED, 0x47, 0x3E, 0x00, 0xED, 0x57});  // LD I,A / LD A,0 / LD A,I
    cpu->pc = 0;
    cpu->iff2 = true;
    cpu->step();
    EXPECT_EQ(cpu->i, 0x3F);
    cpu->step();
    cpu->step();
    EXPECT_EQ(cpu->a, 0x3F);
    EXPECT_TRUE(cpu->flag(z80::FLAG_PV));  // iff2
}

TEST_F(Z80, RetnRestoresIff1FromIff2) {
    cpu->iff1 = false;
    cpu->iff2 = true;
    cpu->sp = 0x4000;
    mem[0x4000] = 0x00;
    mem[0x4001] = 0x20;
    run({0xED, 0x45});
    EXPECT_TRUE(cpu->iff1);
    EXPECT_EQ(cpu->pc, 0x2000);
    EXPECT_EQ(cpu->cycles, 14u);
}

TEST_F(Z80, Im0JamsRstFromBus) {
    irq_byte = 0xFF;  // RST 38
    cpu->im = 0;
    cpu->iff1 = cpu->iff2 = true;
    cpu->sp = 0x8000;
    cpu->pc = 0x1234;
    EXPECT_EQ(cpu->interrupt(), 13);
    EXPECT_EQ(cpu->pc, 0x0038);
}

TEST_F(Z80, LdNnBcAndBack) {
    cpu->set_bc(0xBEEF);
    load({0xED, 0x43, 0x00, 0x40, 0x01, 0x00, 0x00, 0xED, 0x4B, 0x00, 0x40});
    cpu->pc = 0;
    cpu->step();
    EXPECT_EQ(mem[0x4000], 0xEF);
    EXPECT_EQ(mem[0x4001], 0xBE);
    cpu->set_bc(0);
    cpu->pc = 7;
    cpu->step();
    EXPECT_EQ(cpu->bc(), 0xBEEF);
}

// --- IX / IY -------------------------------------------------------------

TEST_F(Z80, AddIxBc) {
    cpu->ix = 0x1000;
    cpu->set_bc(0x0005);
    run({0xDD, 0x09});
    EXPECT_EQ(cpu->ix, 0x1005);
    EXPECT_EQ(cpu->cycles, 15u);  // 11 + 4 prefix
}

TEST_F(Z80, LdIyNnAndJpIy) {
    load({0xFD, 0x21, 0x00, 0x30, 0xFD, 0xE9});  // LD IY,nn / JP (IY)
    cpu->pc = 0;
    cpu->step();
    EXPECT_EQ(cpu->iy, 0x3000);
    cpu->step();
    EXPECT_EQ(cpu->pc, 0x3000);
}

TEST_F(Z80, LdIndexedWrite) {
    cpu->ix = 0x4000;
    cpu->a = 0x77;
    load({0xDD, 0x77, 0x03});  // LD (IX+3),A
    cpu->pc = 0;
    cpu->step();
    EXPECT_EQ(mem[0x4003], 0x77);
    EXPECT_EQ(cpu->cycles, 19u);
}

TEST_F(Z80, IncIxh) {
    cpu->ix = 0x7F00;
    run({0xDD, 0x24});  // INC IXH
    EXPECT_EQ(cpu->ix, 0x8000);
    EXPECT_TRUE(cpu->flag(z80::FLAG_PV));
}

TEST_F(Z80, DiClearsIff) {
    cpu->iff1 = cpu->iff2 = true;
    run({0xF3});
    EXPECT_FALSE(cpu->iff1);
    EXPECT_FALSE(cpu->iff2);
}

}  // namespace
