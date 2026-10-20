#include <gtest/gtest.h>

#include "mb88.h"

#include <array>
#include <cstdint>
#include <initializer_list>

using galaga::Mb88;

namespace {

int exec(Mb88& c, std::initializer_list<uint8_t> bytes) {
    int p = c.pc_full();
    for (uint8_t b : bytes) c.rom[unsigned(p++) % Mb88::kRomBytes] = b;
    return c.step();
}

struct Ports {
    std::array<uint8_t, 4> r{};
    uint8_t k = 0;
    uint8_t p = 0;
    uint8_t o = 0, o_mask = 0;
};

Mb88 cpu(Ports& io) {
    Mb88 c;
    c.reset();
    c.read_r = [&io](int n) { return io.r[unsigned(n)]; };
    c.write_r = [&io](int n, uint8_t v) { io.r[unsigned(n)] = v; };
    c.read_k = [&io] { return io.k; };
    c.write_p = [&io](uint8_t v) { io.p = v; };
    c.write_o = [&io](uint8_t v, uint8_t mask) {
        io.o = v;
        io.o_mask = mask;
    };
    return c;
}

Mb88 cpu() {
    Mb88 c;
    c.reset();
    return c;
}

void poke(Mb88& c, int x, int y, int v) {
    exec(c, {uint8_t(0x58 | x)});
    exec(c, {uint8_t(0x80 | y)});
    exec(c, {uint8_t(0x90 | v)});
    exec(c, {0x1D});
}

int peek(Mb88& c) {
    exec(c, {0x0D});
    return c.a;
}

}  // namespace

TEST(Mb88Isa, ResetStartsAtZeroWithOutputsHigh) {
    Mb88 c;
    c.halted_reset = true;
    c.a = 7;
    c.pa = 3;
    c.pc = 9;
    c.st = 0;
    c.pio = 0xFF;
    c.o_output = 0;
    c.reset();
    EXPECT_FALSE(c.halted_reset);
    EXPECT_EQ(c.pc_full(), 0);
    EXPECT_EQ(c.a, 0);
    EXPECT_EQ(c.st, 1);
    EXPECT_EQ(c.pio, 0);
    EXPECT_EQ(c.o_output, 0xFF);
}

TEST(Mb88Isa, HeldInResetRunsNothing) {
    Mb88 c;
    c.rom[0] = 0x95;
    EXPECT_EQ(c.step(), 1);
    EXPECT_EQ(c.pc_full(), 0);
    EXPECT_EQ(c.a, 0);
}

TEST(Mb88Isa, ImmediatesSetZf) {
    Mb88 c = cpu();
    exec(c, {0x90});
    EXPECT_EQ(c.a, 0);
    EXPECT_EQ(c.zf, 1);
    exec(c, {0x9C});
    EXPECT_EQ(c.a, 0xC);
    EXPECT_EQ(c.zf, 0);
    exec(c, {0x8B});
    EXPECT_EQ(c.y, 0xB);
    exec(c, {0x5F});
    EXPECT_EQ(c.x, 7);
    EXPECT_EQ(c.zf, 0);
    exec(c, {0x58});
    EXPECT_EQ(c.zf, 1);
}

TEST(Mb88Isa, RegisterTransfers) {
    Mb88 c = cpu();
    exec(c, {0x96});
    exec(c, {0x04});
    EXPECT_EQ(c.y, 6);
    exec(c, {0x05});
    EXPECT_EQ(c.th, 6);
    exec(c, {0x93});
    exec(c, {0x06});
    EXPECT_EQ(c.tl, 3);
    exec(c, {0x07});
    EXPECT_EQ(c.sb, 3);
    exec(c, {0x90});
    exec(c, {0x15});
    EXPECT_EQ(c.a, 6);
    exec(c, {0x16});
    EXPECT_EQ(c.a, 3);
    exec(c, {0x90});
    exec(c, {0x17});
    EXPECT_EQ(c.a, 3);
    exec(c, {0x80});
    exec(c, {0x14});
    EXPECT_EQ(c.a, 0);
    EXPECT_EQ(c.zf, 1);
    exec(c, {0x5D});
    exec(c, {0x99});
    exec(c, {0x1B});
    EXPECT_EQ(c.a, 5);
    EXPECT_EQ(c.x, 9);
    EXPECT_EQ(c.zf, 0);
}

TEST(Mb88Isa, MemoryAddressedByXAndY) {
    Mb88 c = cpu();
    poke(c, 3, 0xE, 0xA);
    poke(c, 0, 0xE, 0x4);
    exec(c, {0x5B});
    EXPECT_EQ(peek(c), 0xA);
    exec(c, {0x95});
    exec(c, {0x0B});
    EXPECT_EQ(c.a, 0xA);
    EXPECT_EQ(peek(c), 5);
    exec(c, {0x58});
    EXPECT_EQ(peek(c), 4);
}

TEST(Mb88Isa, XdAndXydUsePageZero) {
    Mb88 c = cpu();
    poke(c, 0, 2, 9);
    poke(c, 0, 6, 0);
    exec(c, {0x5A});
    exec(c, {0x93});
    exec(c, {0x52});
    EXPECT_EQ(c.a, 9);
    EXPECT_EQ(c.zf, 0);
    exec(c, {0x87});
    exec(c, {0x56});
    EXPECT_EQ(c.y, 0);
    EXPECT_EQ(c.zf, 1);
    exec(c, {0x58});
    exec(c, {0x82});
    EXPECT_EQ(peek(c), 3);
    exec(c, {0x86});
    EXPECT_EQ(peek(c), 7);
}

TEST(Mb88Isa, SticAndStdcStoreThenStepY) {
    Mb88 c = cpu();
    exec(c, {0x8F});
    exec(c, {0x97});
    exec(c, {0x0A});
    EXPECT_EQ(c.y, 0);
    EXPECT_EQ(c.zf, 1);
    EXPECT_EQ(c.st, 0);
    exec(c, {0x0A});
    EXPECT_EQ(c.y, 1);
    EXPECT_EQ(c.st, 1);
    exec(c, {0x1A});
    EXPECT_EQ(c.y, 0);
    EXPECT_EQ(c.st, 1);
    exec(c, {0x1A});
    EXPECT_EQ(c.y, 0xF);
    EXPECT_EQ(c.st, 0);
    exec(c, {0x81});
    EXPECT_EQ(peek(c), 7);
}

TEST(Mb88Isa, IncrementsClearStOnCarryDecrementsOnBorrow) {
    Mb88 c = cpu();
    exec(c, {0x8E});
    exec(c, {0x08});
    EXPECT_EQ(c.y, 0xF);
    EXPECT_EQ(c.st, 1);
    exec(c, {0x08});
    EXPECT_EQ(c.y, 0);
    EXPECT_EQ(c.zf, 1);
    EXPECT_EQ(c.st, 0);
    exec(c, {0x90});
    exec(c, {0x18});
    EXPECT_EQ(c.y, 0xF);
    EXPECT_EQ(c.st, 0);
    EXPECT_EQ(c.zf, 1) << "DCY leaves ZF";
    exec(c, {0x18});
    EXPECT_EQ(c.st, 1);
    poke(c, 0, 0, 1);
    exec(c, {0x19});
    EXPECT_EQ(c.zf, 1);
    EXPECT_EQ(c.st, 1);
    exec(c, {0x19});
    EXPECT_EQ(c.zf, 0);
    EXPECT_EQ(c.st, 0);
    EXPECT_EQ(peek(c), 0xF);
    exec(c, {0x09});
    EXPECT_EQ(c.st, 0);
    EXPECT_EQ(c.zf, 1);
    EXPECT_EQ(peek(c), 0);
}

TEST(Mb88Isa, AdcAndAiCarry) {
    Mb88 c = cpu();
    poke(c, 0, 0, 9);
    exec(c, {0x98});
    exec(c, {0x21});
    exec(c, {0x0E});
    EXPECT_EQ(c.a, 2);
    EXPECT_EQ(c.cf, 1);
    EXPECT_EQ(c.st, 0);
    exec(c, {0x23});
    exec(c, {0x90});
    exec(c, {0x0E});
    EXPECT_EQ(c.a, 9);
    EXPECT_EQ(c.cf, 0);
    EXPECT_EQ(c.st, 1);
    exec(c, {0x77});
    EXPECT_EQ(c.a, 0);
    EXPECT_EQ(c.zf, 1);
    EXPECT_EQ(c.cf, 1);
    exec(c, {0x7F});
    EXPECT_EQ(c.a, 0xF);
    EXPECT_EQ(c.cf, 0);
    EXPECT_EQ(c.st, 1);
    exec(c, {0x7F});
    EXPECT_EQ(c.a, 0xE);
    EXPECT_EQ(c.cf, 1);
}

TEST(Mb88Isa, SbcBorrowsAndDasAdjustsBcd) {
    Mb88 c = cpu();
    poke(c, 0, 0, 0);
    poke(c, 0, 1, 2);
    exec(c, {0x80});
    exec(c, {0x91});
    exec(c, {0x23});
    exec(c, {0x1E});
    EXPECT_EQ(c.a, 0xF);
    EXPECT_EQ(c.cf, 1);
    EXPECT_EQ(c.st, 0);
    exec(c, {0x11});
    EXPECT_EQ(c.a, 9);
    EXPECT_EQ(c.cf, 1);
    exec(c, {0x81});
    exec(c, {0x90});
    exec(c, {0x1E});
    EXPECT_EQ(c.a, 1);
    EXPECT_EQ(c.cf, 0);
    EXPECT_EQ(c.st, 1);
}

TEST(Mb88Isa, DaaAdjustsBcdAdd) {
    Mb88 c = cpu();
    poke(c, 0, 0, 8);
    exec(c, {0x95});
    exec(c, {0x23});
    exec(c, {0x0E});
    EXPECT_EQ(c.a, 0xD);
    exec(c, {0x10});
    EXPECT_EQ(c.a, 3);
    EXPECT_EQ(c.cf, 1);
    EXPECT_EQ(c.st, 0);
    exec(c, {0x23});
    exec(c, {0x94});
    exec(c, {0x10});
    EXPECT_EQ(c.a, 4);
    EXPECT_EQ(c.cf, 0);
    EXPECT_EQ(c.st, 1);
}

TEST(Mb88Isa, LogicOpsClearStOnZeroAndLeaveCarry) {
    Mb88 c = cpu();
    poke(c, 0, 0, 0xC);
    exec(c, {0x21});
    exec(c, {0x93});
    exec(c, {0x0F});
    EXPECT_EQ(c.a, 0);
    EXPECT_EQ(c.zf, 1);
    EXPECT_EQ(c.st, 0);
    EXPECT_EQ(c.cf, 1);
    exec(c, {0x93});
    exec(c, {0x1F});
    EXPECT_EQ(c.a, 0xF);
    EXPECT_EQ(c.st, 1);
    exec(c, {0x2F});
    EXPECT_EQ(c.a, 3);
    exec(c, {0x9C});
    exec(c, {0x2F});
    EXPECT_EQ(c.a, 0);
    EXPECT_EQ(c.st, 0);
    EXPECT_EQ(c.cf, 1);
}

TEST(Mb88Isa, ComparesLeaveOperandsAndClearStWhenEqual) {
    Mb88 c = cpu();
    poke(c, 0, 0, 5);
    exec(c, {0x95});
    exec(c, {0x2E});
    EXPECT_EQ(c.a, 5);
    EXPECT_EQ(c.zf, 1);
    EXPECT_EQ(c.st, 0);
    EXPECT_EQ(c.cf, 0);
    exec(c, {0x96});
    exec(c, {0x2E});
    EXPECT_EQ(c.st, 1);
    EXPECT_EQ(c.cf, 1);
    exec(c, {0xB6});
    EXPECT_EQ(c.st, 0);
    exec(c, {0xB2});
    EXPECT_EQ(c.cf, 1);
    EXPECT_EQ(c.st, 1);
    EXPECT_EQ(c.a, 6);
    exec(c, {0x84});
    exec(c, {0xA4});
    EXPECT_EQ(c.st, 0);
    exec(c, {0xA9});
    EXPECT_EQ(c.cf, 0);
    EXPECT_EQ(c.st, 1);
    EXPECT_EQ(c.y, 4);
}

TEST(Mb88Isa, NegIsTwosComplement) {
    Mb88 c = cpu();
    exec(c, {0x93});
    exec(c, {0x2D});
    EXPECT_EQ(c.a, 0xD);
    EXPECT_EQ(c.st, 1);
    exec(c, {0x90});
    exec(c, {0x2D});
    EXPECT_EQ(c.a, 0);
    EXPECT_EQ(c.st, 0);
}

TEST(Mb88Isa, RotatesGoThroughCarry) {
    Mb88 c = cpu();
    exec(c, {0x99});
    exec(c, {0x23});
    exec(c, {0x0C});
    EXPECT_EQ(c.a, 2);
    EXPECT_EQ(c.cf, 1);
    EXPECT_EQ(c.st, 0);
    exec(c, {0x0C});
    EXPECT_EQ(c.a, 5);
    EXPECT_EQ(c.cf, 0);
    EXPECT_EQ(c.st, 1);
    exec(c, {0x1C});
    EXPECT_EQ(c.a, 2);
    EXPECT_EQ(c.cf, 1);
    exec(c, {0x1C});
    EXPECT_EQ(c.a, 9);
    EXPECT_EQ(c.cf, 0);
}

TEST(Mb88Isa, BitOpsOnMemoryAndA) {
    Mb88 c = cpu();
    poke(c, 1, 2, 0);
    exec(c, {0x32});
    EXPECT_EQ(peek(c), 4);
    exec(c, {0x3A});
    EXPECT_EQ(c.st, 0);
    exec(c, {0x39});
    EXPECT_EQ(c.st, 1);
    exec(c, {0x36});
    EXPECT_EQ(peek(c), 0);
    exec(c, {0x98});
    exec(c, {0x4F});
    EXPECT_EQ(c.st, 0);
    exec(c, {0x4E});
    EXPECT_EQ(c.st, 1);
}

TEST(Mb88Isa, FlagTests) {
    Mb88 c = cpu();
    exec(c, {0x21});
    exec(c, {0x28});
    EXPECT_EQ(c.st, 0);
    exec(c, {0x23});
    exec(c, {0x28});
    EXPECT_EQ(c.st, 1);
    exec(c, {0x90});
    exec(c, {0x29});
    EXPECT_EQ(c.st, 0);
    c.sf = 1;
    exec(c, {0x27});
    EXPECT_EQ(c.st, 0);
    EXPECT_EQ(c.sf, 0);
    c.vf = 1;
    exec(c, {0x26});
    EXPECT_EQ(c.st, 0);
    EXPECT_EQ(c.vf, 0);
    exec(c, {0x25});
    EXPECT_EQ(c.st, 1);
    c.set_irq(true);
    exec(c, {0x25});
    EXPECT_EQ(c.st, 0) << "IF follows the pin";
}

TEST(Mb88Isa, OtherInstructionsSetSt) {
    Mb88 c = cpu();
    exec(c, {0x90});
    exec(c, {0x2D});
    EXPECT_EQ(c.st, 0);
    exec(c, {0x00});
    EXPECT_EQ(c.st, 1);
}

TEST(Mb88Isa, JmpStaysInPageAndIsSkippedWhenStIsClear) {
    Mb88 c = cpu();
    c.pa = 2;
    c.pc = 0x10;
    exec(c, {0xC5});
    EXPECT_EQ(c.pc_full(), 0x85);
    exec(c, {0xB0});
    EXPECT_EQ(c.st, 0);
    exec(c, {0xD0});
    EXPECT_EQ(c.pc_full(), 0x87);
    EXPECT_EQ(c.st, 1);
    exec(c, {0xD0});
    EXPECT_EQ(c.pc_full(), 0x90);
}

TEST(Mb88Isa, JmpOnTheLastByteOfAPageUsesTheNextPage) {
    Mb88 c = cpu();
    c.pc = 0x3F;
    exec(c, {0xC9});
    EXPECT_EQ(c.pc_full(), 0x49);
}

TEST(Mb88Isa, PcCarriesIntoThePage) {
    Mb88 c = cpu();
    c.pc = 0x3F;
    exec(c, {0x00});
    EXPECT_EQ(c.pc_full(), 0x40);
}

TEST(Mb88Isa, JplAndCallAreTwoCyclesAndSkipTheirOperand) {
    Mb88 c = cpu();
    EXPECT_EQ(exec(c, {0x6B, 0x45}), 2);
    EXPECT_EQ(c.pc_full(), 0x345);
    exec(c, {0xB0});
    EXPECT_EQ(exec(c, {0x69, 0x00}), 2);
    EXPECT_EQ(c.pc_full(), 0x348);
    exec(c, {0xB0});
    EXPECT_EQ(exec(c, {0x60, 0x10}), 2);
    EXPECT_EQ(c.pc_full(), 0x34B);
    EXPECT_EQ(c.st, 1);
}

TEST(Mb88Isa, CallAndRtsNestFourDeep) {
    Mb88 c = cpu();
    c.rom[0x000] = 0x61;
    c.rom[0x001] = 0x00;
    c.rom[0x100] = 0x62;
    c.rom[0x101] = 0x00;
    c.rom[0x200] = 0x63;
    c.rom[0x201] = 0x00;
    c.rom[0x300] = 0x60;
    c.rom[0x301] = 0x80;
    c.rom[0x080] = 0x2C;
    c.rom[0x302] = 0x2C;
    c.rom[0x202] = 0x2C;
    c.rom[0x102] = 0x2C;
    for (int i = 0; i < 4; i++) c.step();
    EXPECT_EQ(c.pc_full(), 0x080);
    c.step();
    EXPECT_EQ(c.pc_full(), 0x302);
    c.step();
    EXPECT_EQ(c.pc_full(), 0x202);
    c.step();
    EXPECT_EQ(c.pc_full(), 0x102);
    c.step();
    EXPECT_EQ(c.pc_full(), 0x002);
}

TEST(Mb88Isa, JpaJumpsToFourTimesAOnPageN) {
    Mb88 c = cpu();
    exec(c, {0x93});
    exec(c, {0xB0});
    EXPECT_EQ(exec(c, {0x3D, 0x05}), 2);
    EXPECT_EQ(c.pc_full(), 5 * 64 + 12);
}

TEST(Mb88Isa, OutoWritesTheNibbleCarrySelects) {
    Ports io;
    Mb88 c = cpu(io);
    exec(c, {0x96});
    exec(c, {0x23});
    exec(c, {0x01});
    EXPECT_EQ(io.o, 0xF6);
    EXPECT_EQ(io.o_mask, 0x0F);
    exec(c, {0x92});
    exec(c, {0x21});
    exec(c, {0x01});
    EXPECT_EQ(io.o, 0x26);
    EXPECT_EQ(io.o_mask, 0xF0);
    EXPECT_EQ(c.o_output, 0x26);
}

TEST(Mb88Isa, PortsKPAndR) {
    Ports io;
    Mb88 c = cpu(io);
    io.k = 0x0A;
    exec(c, {0x12});
    EXPECT_EQ(c.a, 0xA);
    exec(c, {0x02});
    EXPECT_EQ(io.p, 0xA);
    exec(c, {0x82});
    exec(c, {0x93});
    exec(c, {0x03});
    EXPECT_EQ(io.r[2], 3);
    io.r[1] = 0;
    exec(c, {0x81});
    exec(c, {0x13});
    EXPECT_EQ(c.a, 0);
    EXPECT_EQ(c.zf, 1);
}

TEST(Mb88Isa, SetrRstrTstrAddressLineY) {
    Ports io;
    Mb88 c = cpu(io);
    exec(c, {0x8E});
    exec(c, {0x22});
    EXPECT_EQ(io.r[3], 0xB);
    exec(c, {0x24});
    EXPECT_EQ(c.st, 1);
    exec(c, {0x20});
    EXPECT_EQ(io.r[3], 0xF);
    exec(c, {0x24});
    EXPECT_EQ(c.st, 0);
}

TEST(Mb88Isa, SetdRstdOnPortZeroTstdOnPortTwo) {
    Ports io;
    Mb88 c = cpu(io);
    exec(c, {0x45});
    EXPECT_EQ(io.r[0], 0xD);
    exec(c, {0x41});
    EXPECT_EQ(io.r[0], 0xF);
    io.r[2] = 0x8;
    exec(c, {0x4B});
    EXPECT_EQ(c.st, 0);
    exec(c, {0x4A});
    EXPECT_EQ(c.st, 1);
}

TEST(Mb88Isa, EnAndDisEditPioInTwoCycles) {
    Mb88 c = cpu();
    EXPECT_EQ(exec(c, {0x3E, 0x64}), 2);
    EXPECT_EQ(c.pio, 0x64);
    EXPECT_EQ(exec(c, {0x3F, 0x04}), 2);
    EXPECT_EQ(c.pio, 0x60);
}

TEST(Mb88Timer, InternalClockCountsEvery32Cycles) {
    Mb88 c = cpu();
    exec(c, {0x3E, 0x80});
    for (int i = 0; i < 29; i++) c.step();
    EXPECT_EQ(c.tl, 0);
    c.step();
    EXPECT_EQ(c.tl, 1);
}

TEST(Mb88Timer, OverflowSetsVfAndInterruptsTo04) {
    Mb88 c = cpu();
    exec(c, {0x9F});
    exec(c, {0x05});
    exec(c, {0x06});
    exec(c, {0x3E, 0x42});
    c.set_tc(false);
    EXPECT_EQ(c.th, 0);
    EXPECT_EQ(c.vf, 1);
    EXPECT_EQ(c.step(), 3);
    EXPECT_EQ(c.pc_full(), 0x04);
    EXPECT_TRUE(c.in_irq);
}

TEST(Mb88Timer, CounterTakesTcFallingEdgesOnly) {
    Mb88 c = cpu();
    c.set_tc(false);
    EXPECT_EQ(c.tl, 0) << "counter off";
    c.set_tc(true);
    exec(c, {0x3E, 0x40});
    c.set_tc(true);
    EXPECT_EQ(c.tl, 0);
    c.set_tc(false);
    EXPECT_EQ(c.tl, 1);
    c.set_tc(false);
    EXPECT_EQ(c.tl, 1);
}

TEST(Mb88Irq, FallingIrqVectorsTo02AndRtiRestoresFlags) {
    Mb88 c = cpu();
    c.rom[0x02] = 0x3C;
    c.pa = 1;
    exec(c, {0x3E, 0x04});
    exec(c, {0x21});
    exec(c, {0x28});
    int ret = c.pc_full();
    c.set_irq(true);
    EXPECT_EQ(c.step(), 3);
    EXPECT_EQ(c.pc_full(), 0x02);
    EXPECT_TRUE(c.in_irq);
    c.cf = 0;
    c.st = 1;
    c.step();
    EXPECT_EQ(c.pc_full(), ret);
    EXPECT_EQ(c.cf, 1);
    EXPECT_EQ(c.st, 0);
    EXPECT_FALSE(c.in_irq);
}

TEST(Mb88Irq, EdgeIsLostWhileDisabled) {
    Mb88 c = cpu();
    c.set_irq(true);
    exec(c, {0x3E, 0x04});
    c.step();
    EXPECT_FALSE(c.in_irq);
    c.set_irq(false);
    c.set_irq(true);
    c.step();
    EXPECT_TRUE(c.in_irq);
}

TEST(Mb88Irq, HeldLowLineRequestsOnce) {
    Mb88 c = cpu();
    c.rom[0x02] = 0x3C;
    c.pc = 0x10;
    exec(c, {0x3E, 0x04});
    c.set_irq(true);
    c.step();
    c.step();
    EXPECT_FALSE(c.in_irq);
    c.set_irq(true);
    c.step();
    EXPECT_FALSE(c.in_irq);
}

TEST(Mb88Irq, SecondRequestWaitsForRti) {
    Mb88 c = cpu();
    c.rom[0x02] = 0x00;
    c.rom[0x03] = 0x3C;
    c.pc = 0x10;
    exec(c, {0x3E, 0x04});
    c.set_irq(true);
    c.step();
    c.set_irq(false);
    c.set_irq(true);
    c.step();
    EXPECT_EQ(c.pc_full(), 0x03);
    c.step();
    EXPECT_FALSE(c.in_irq);
    EXPECT_EQ(c.step(), 3);
    EXPECT_EQ(c.pc_full(), 0x02);
}

TEST(Mb88Irq, ExternalBeatsTimer) {
    Mb88 c = cpu();
    exec(c, {0x9F});
    exec(c, {0x05});
    exec(c, {0x06});
    exec(c, {0x3E, 0x46});
    c.set_tc(false);
    c.set_irq(true);
    c.step();
    EXPECT_EQ(c.pc_full(), 0x02);
}

TEST(Mb88Serial, InternalClockShiftsOncePerCycle) {
    Mb88 c = cpu();
    bool si = true;
    c.read_si = [&si] { return si; };
    exec(c, {0x3E, 0x20});
    EXPECT_EQ(c.sb, 0xC);
    EXPECT_EQ(c.sf, 0);
    c.step();
    EXPECT_EQ(c.sf, 0);
    c.step();
    EXPECT_EQ(c.sb, 0xF);
    EXPECT_EQ(c.sf, 1);
}

TEST(Mb88Serial, FullBufferInterruptsTo06) {
    Mb88 c = cpu();
    exec(c, {0x3E, 0x21});
    c.step();
    c.step();
    EXPECT_EQ(c.step(), 3);
    EXPECT_EQ(c.pc_full(), 0x06);
}
