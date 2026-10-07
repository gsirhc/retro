// GoogleTest suite for Cpu::interrupt() and the EI one-instruction delay.

#include <gtest/gtest.h>

#include "i8080.h"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>

namespace {

constexpr uint8_t NOP  = 0x00;
constexpr uint8_t EI   = 0xFB;
constexpr uint8_t DI   = 0xF3;
constexpr uint8_t HLT  = 0x76;
constexpr uint8_t RST7 = 0xFF;   // the conventional Altair 88-2SIO vector: RST 7 (0x38)

class Interrupt : public ::testing::Test {
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

    void load(std::initializer_list<uint8_t> code) {
        uint16_t addr = 0;
        for (uint8_t byte : code) mem[addr++] = byte;
        cpu->pc = 0;
    }

    uint16_t popWord() {
        uint16_t v = static_cast<uint16_t>(mem[cpu->sp]) | (static_cast<uint16_t>(mem[cpu->sp + 1]) << 8);
        return v;
    }
};

TEST_F(Interrupt, IgnoredWhileDisabled) {
    load({DI});
    cpu->step();                          // DI: int_enabled = false
    EXPECT_EQ(cpu->interrupt(RST7), 0);   // not serviced
    EXPECT_EQ(cpu->pc, 1);                // untouched -- still just after DI
}

TEST_F(Interrupt, TakenWhileEnabled) {
    load({NOP, NOP});
    cpu->int_enabled = true;              // enabled with no EI delay in the way
    cpu->sp = 0x2000;
    cpu->pc = 0x1234;
    EXPECT_EQ(cpu->interrupt(RST7), 11);  // serviced, 11 T-states (RST timing)
    EXPECT_EQ(cpu->pc, 0x38);             // RST7 vectors to n*8 = 0x38
    EXPECT_EQ(popWord(), 0x1234);         // return address pushed
}

TEST_F(Interrupt, VectorIsOpcodeAndTriplet) {
    // RST n = 11rrr111; the vector is opcode & 0x38
    struct { uint8_t opcode; uint16_t vector; } cases[] = {
        {0xC7, 0x00}, {0xCF, 0x08}, {0xD7, 0x10}, {0xDF, 0x18},
        {0xE7, 0x20}, {0xEF, 0x28}, {0xF7, 0x30}, {0xFF, 0x38},
    };
    for (auto c : cases) {
        cpu->reset();
        cpu->int_enabled = true;
        cpu->pc = 0x5000;
        cpu->interrupt(c.opcode);
        EXPECT_EQ(cpu->pc, c.vector) << "opcode " << std::hex << int(c.opcode);
    }
}

TEST_F(Interrupt, ClearsIntEnabledOnAccept) {
    cpu->int_enabled = true;
    cpu->interrupt(RST7);
    EXPECT_FALSE(cpu->int_enabled);       // a real 8080 disables further nesting until EI again
}

TEST_F(Interrupt, WakesHalt) {
    load({HLT});
    cpu->step();                          // executes HLT -> cpu->halted = true
    ASSERT_TRUE(cpu->halted);
    cpu->int_enabled = true;
    EXPECT_EQ(cpu->interrupt(RST7), 11);
    EXPECT_FALSE(cpu->halted);
    EXPECT_EQ(cpu->pc, 0x38);
}

TEST_F(Interrupt, DisabledAndHaltedStaysHalted) {
    // HLT with DI never wakes (until RESET)
    load({HLT});
    cpu->step();
    ASSERT_TRUE(cpu->halted);
    EXPECT_EQ(cpu->interrupt(RST7), 0);   // int_enabled is false: ignored
    EXPECT_TRUE(cpu->halted);
}

// EI/RET idiom: no interrupt is taken until the instruction after EI retires
TEST_F(Interrupt, EiDelaysOneInstruction) {
    load({EI, NOP, NOP});
    cpu->step();                          // EI retires, delay window open
    EXPECT_EQ(cpu->interrupt(RST7), 0)    // requested immediately after EI: too soon
        << "an interrupt right after EI must wait for the next instruction";
    EXPECT_TRUE(cpu->int_enabled);        // still enabled -- just not yet acceptable

    cpu->step();                          // the instruction *following* EI retires
    EXPECT_EQ(cpu->interrupt(RST7), 11)   // now it's taken
        << "the delay should be over after exactly one more instruction";
}

TEST_F(Interrupt, EiThenHaltSatisfiesTheDelayOnHaltsOwnRetirement) {
    // HLT's own retirement satisfies the EI delay
    load({EI, HLT});
    cpu->step();                          // EI
    cpu->step();                          // HLT retires and halts
    ASSERT_TRUE(cpu->halted);
    EXPECT_EQ(cpu->interrupt(RST7), 11);
}

} // namespace
