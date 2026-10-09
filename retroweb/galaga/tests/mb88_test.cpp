#include <gtest/gtest.h>

#include "mb88.h"

#include <array>
#include <cstdint>
#include <initializer_list>

using galaga::Mb88;

namespace {

// Places one instruction at PC and runs it.
int exec(Mb88& c, std::initializer_list<uint8_t> bytes) {
    int p = c.pc_full();
    for (uint8_t b : bytes) c.rom[unsigned(p++) & (Mb88::kRomBytes - 1)] = b;
    return c.step();
}

struct Ports {
    std::array<uint8_t, 4> r{};
    uint8_t k = 0;
    int p_writes = 0;
    uint8_t p = 0;
    uint8_t o = 0, o_mask = 0;
};

Mb88 cpu(Ports& io) {
    Mb88 c;
    c.reset();
    c.read_r = [&io](int n) { return io.r[unsigned(n)]; };
    c.write_r = [&io](int n, uint8_t v) { io.r[unsigned(n)] = v; };
    c.read_k = [&io] { return io.k; };
    c.write_p = [&io](uint8_t v) {
        io.p = v;
        io.p_writes++;
    };
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

// LXI x; LYI y; LI v; ST
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

TEST(Mb88Isa, ResetClearsRegistersAndSetsSt) {
    Mb88 c = cpu();
    EXPECT_EQ(c.pc_full(), 0);
    EXPECT_EQ(c.a, 0);
    EXPECT_EQ(c.x, 0);
    EXPECT_EQ(c.y, 0);
    EXPECT_EQ(c.st, 1);
    EXPECT_EQ(c.cf, 0);
    EXPECT_EQ(c.zf, 0);
    EXPECT_EQ(c.pio, 0);
    EXPECT_FALSE(c.halted_reset);
}

TEST(Mb88Isa, HeldInResetRunsNothing) {
    Mb88 c = cpu();
    c.halted_reset = true;
    c.rom[0] = 0x95;
    EXPECT_EQ(c.step(), 1);
    EXPECT_EQ(c.a, 0);
    EXPECT_EQ(c.pc_full(), 0);
}

TEST(Mb88Isa, ImmediateLoadsSetZf) {
    Mb88 c = cpu();
    exec(c, {0x90});
    EXPECT_EQ(c.a, 0);
    EXPECT_EQ(c.zf, 1);
    exec(c, {0x95});
    EXPECT_EQ(c.a, 5);
    EXPECT_EQ(c.zf, 0);
    exec(c, {0x80});
    EXPECT_EQ(c.y, 0);
    EXPECT_EQ(c.zf, 1);
    exec(c, {0x5D});
    EXPECT_EQ(c.x, 5);
    EXPECT_EQ(c.zf, 0);
    exec(c, {0x58});
    EXPECT_EQ(c.x, 0);
    EXPECT_EQ(c.zf, 1);
}

TEST(Mb88Isa, RegisterTransfers) {
    Mb88 c = cpu();
    exec(c, {0x99});
    exec(c, {0x04});
    EXPECT_EQ(c.y, 9);
    exec(c, {0x05});
    EXPECT_EQ(c.th, 9);
    exec(c, {0x96});
    exec(c, {0x06});
    EXPECT_EQ(c.tl, 6);
    exec(c, {0x9A});
    exec(c, {0x07});
    EXPECT_EQ(c.sb, 0xA);
    exec(c, {0x90});
    exec(c, {0x14});
    EXPECT_EQ(c.a, 9);
    exec(c, {0x15});
    EXPECT_EQ(c.a, 9);
    exec(c, {0x16});
    EXPECT_EQ(c.a, 6);
    exec(c, {0x17});
    EXPECT_EQ(c.a, 0xA);
    exec(c, {0x93});
    exec(c, {0x5D});
    exec(c, {0x1B});
    EXPECT_EQ(c.a, 5);
    EXPECT_EQ(c.x, 3);
}

TEST(Mb88Isa, IcyCarriesOutOfYAndDcyLeavesZf) {
    Mb88 c = cpu();
    exec(c, {0x8F});
    exec(c, {0x08});
    EXPECT_EQ(c.y, 0);
    EXPECT_EQ(c.st, 0);
    EXPECT_EQ(c.zf, 1);
    exec(c, {0x08});
    EXPECT_EQ(c.y, 1);
    EXPECT_EQ(c.st, 1);
    EXPECT_EQ(c.zf, 0);
    exec(c, {0x80});
    exec(c, {0x18});
    EXPECT_EQ(c.y, 15);
    EXPECT_EQ(c.st, 0);
    EXPECT_EQ(c.zf, 1) << "DCY only touches ST";
}

TEST(Mb88Isa, LoadStoreAndExchangeThroughXy) {
    Mb88 c = cpu();
    poke(c, 2, 3, 0xC);
    exec(c, {0x90});
    EXPECT_EQ(peek(c), 0xC);
    EXPECT_EQ(c.zf, 0);
    EXPECT_EQ(c.st, 1);
    exec(c, {0x95});
    exec(c, {0x0B});
    EXPECT_EQ(c.a, 0xC);
    EXPECT_EQ(peek(c), 5);
    exec(c, {0x5A});
    exec(c, {0x84});
    EXPECT_EQ(peek(c), 0) << "EA is X:Y";
}

TEST(Mb88Isa, IcmAndDcmCarryAndZero) {
    Mb88 c = cpu();
    poke(c, 1, 1, 0xF);
    exec(c, {0x09});
    EXPECT_EQ(c.st, 0);
    EXPECT_EQ(c.zf, 1);
    EXPECT_EQ(peek(c), 0);
    exec(c, {0x19});
    EXPECT_EQ(c.st, 0);
    EXPECT_EQ(c.zf, 0);
    EXPECT_EQ(peek(c), 0xF);
    exec(c, {0x19});
    EXPECT_EQ(c.st, 1);
    EXPECT_EQ(peek(c), 0xE);
}

TEST(Mb88Isa, SticAndStdcStepY) {
    Mb88 c = cpu();
    exec(c, {0x5A});
    exec(c, {0x83});
    exec(c, {0x97});
    exec(c, {0x0A});
    EXPECT_EQ(c.y, 4);
    EXPECT_EQ(c.st, 1);
    EXPECT_EQ(c.zf, 0);
    exec(c, {0x83});
    EXPECT_EQ(peek(c), 7);
    exec(c, {0x8F});
    exec(c, {0x96});
    exec(c, {0x0A});
    EXPECT_EQ(c.y, 0);
    EXPECT_EQ(c.st, 0);
    EXPECT_EQ(c.zf, 1);
    exec(c, {0x8F});
    EXPECT_EQ(peek(c), 6);
    exec(c, {0x80});
    exec(c, {0x94});
    exec(c, {0x1A});
    EXPECT_EQ(c.y, 15);
    EXPECT_EQ(c.st, 0);
    EXPECT_EQ(c.zf, 0);
    exec(c, {0x80});
    EXPECT_EQ(peek(c), 4);
}

TEST(Mb88Isa, XdAndXydSwapWithLowNibbles) {
    Mb88 c = cpu();
    exec(c, {0x94});
    exec(c, {0x51});
    EXPECT_EQ(c.a, 0);
    EXPECT_EQ(c.zf, 1);
    exec(c, {0x99});
    exec(c, {0x51});
    EXPECT_EQ(c.a, 4);
    EXPECT_EQ(c.zf, 0);
    exec(c, {0x58});
    exec(c, {0x81});
    EXPECT_EQ(peek(c), 9);
    exec(c, {0x86});
    exec(c, {0x54});
    EXPECT_EQ(c.y, 0);
    EXPECT_EQ(c.zf, 1);
    exec(c, {0x82});
    exec(c, {0x54});
    EXPECT_EQ(c.y, 6);
    exec(c, {0x84});
    EXPECT_EQ(peek(c), 2);
}

TEST(Mb88Isa, StsAndLsMoveTheSerialBuffer) {
    Mb88 c = cpu();
    exec(c, {0x9B});
    exec(c, {0x07});
    exec(c, {0x5B});
    exec(c, {0x82});
    exec(c, {0x2A});
    EXPECT_EQ(c.zf, 0);
    EXPECT_EQ(peek(c), 0xB);
    exec(c, {0x90});
    exec(c, {0x1D});
    exec(c, {0x2B});
    EXPECT_EQ(c.sb, 0);
    EXPECT_EQ(c.zf, 1);
}

TEST(Mb88Isa, MemoryBitSetResetTest) {
    Mb88 c = cpu();
    exec(c, {0x32});
    EXPECT_EQ(peek(c), 4);
    exec(c, {0x3A});
    EXPECT_EQ(c.st, 0);
    exec(c, {0x38});
    EXPECT_EQ(c.st, 1);
    exec(c, {0x36});
    EXPECT_EQ(peek(c), 0);
}

TEST(Mb88Isa, AdcAndSbcThroughCarry) {
    Mb88 c = cpu();
    poke(c, 0, 0, 9);
    exec(c, {0x98});
    exec(c, {0x21});
    exec(c, {0x0E});
    EXPECT_EQ(c.a, 2);
    EXPECT_EQ(c.cf, 1);
    EXPECT_EQ(c.st, 0);
    EXPECT_EQ(c.zf, 0);
    poke(c, 0, 0, 3);
    exec(c, {0x95});
    exec(c, {0x23});
    exec(c, {0x1E});
    EXPECT_EQ(c.a, 0xE);
    EXPECT_EQ(c.cf, 1);
    EXPECT_EQ(c.st, 0);
    poke(c, 0, 0, 7);
    exec(c, {0x92});
    exec(c, {0x21});
    exec(c, {0x1E});
    EXPECT_EQ(c.a, 4);
    EXPECT_EQ(c.cf, 0);
    EXPECT_EQ(c.st, 1);
}

TEST(Mb88Isa, AiCarriesAndSetsZf) {
    Mb88 c = cpu();
    exec(c, {0x99});
    exec(c, {0x77});
    EXPECT_EQ(c.a, 0);
    EXPECT_EQ(c.cf, 1);
    EXPECT_EQ(c.st, 0);
    EXPECT_EQ(c.zf, 1);
    exec(c, {0x91});
    exec(c, {0x72});
    EXPECT_EQ(c.a, 3);
    EXPECT_EQ(c.cf, 0);
    EXPECT_EQ(c.st, 1);
}

TEST(Mb88Isa, LogicOpsSetStFromTheResult) {
    Mb88 c = cpu();
    poke(c, 0, 0, 0xA);
    exec(c, {0x9C});
    exec(c, {0x0F});
    EXPECT_EQ(c.a, 8);
    EXPECT_EQ(c.st, 1);
    exec(c, {0x95});
    exec(c, {0x0F});
    EXPECT_EQ(c.a, 0);
    EXPECT_EQ(c.zf, 1);
    EXPECT_EQ(c.st, 0);
    exec(c, {0x91});
    exec(c, {0x1F});
    EXPECT_EQ(c.a, 0xB);
    EXPECT_EQ(c.st, 1);
    exec(c, {0x9A});
    exec(c, {0x2F});
    EXPECT_EQ(c.a, 0);
    EXPECT_EQ(c.st, 0);
    EXPECT_EQ(c.zf, 1);
    exec(c, {0x95});
    exec(c, {0x2F});
    EXPECT_EQ(c.a, 0xF);
    EXPECT_EQ(c.st, 1);
    EXPECT_EQ(c.zf, 0);
    poke(c, 0, 0, 0);
    exec(c, {0x1F});
    EXPECT_EQ(c.a, 0);
    EXPECT_EQ(c.st, 0);
}

TEST(Mb88Isa, RotatesGoThroughCarry) {
    Mb88 c = cpu();
    exec(c, {0x98});
    exec(c, {0x21});
    exec(c, {0x0C});
    EXPECT_EQ(c.a, 1);
    EXPECT_EQ(c.cf, 1);
    EXPECT_EQ(c.st, 0);
    exec(c, {0x90});
    exec(c, {0x23});
    exec(c, {0x0C});
    EXPECT_EQ(c.a, 0);
    EXPECT_EQ(c.cf, 0);
    EXPECT_EQ(c.zf, 1);
    exec(c, {0x91});
    exec(c, {0x21});
    exec(c, {0x1C});
    EXPECT_EQ(c.a, 8);
    EXPECT_EQ(c.cf, 1);
    EXPECT_EQ(c.st, 0);
    exec(c, {0x92});
    exec(c, {0x23});
    exec(c, {0x1C});
    EXPECT_EQ(c.a, 1);
    EXPECT_EQ(c.cf, 0);
    EXPECT_EQ(c.st, 1);
}

TEST(Mb88Isa, DecimalAdjust) {
    Mb88 c = cpu();
    exec(c, {0x9B});
    exec(c, {0x23});
    exec(c, {0x10});
    EXPECT_EQ(c.a, 1);
    EXPECT_EQ(c.cf, 1);
    exec(c, {0x95});
    exec(c, {0x23});
    exec(c, {0x10});
    EXPECT_EQ(c.a, 5);
    EXPECT_EQ(c.cf, 0);
    exec(c, {0x92});
    exec(c, {0x21});
    exec(c, {0x10});
    EXPECT_EQ(c.a, 8);
    EXPECT_EQ(c.cf, 0);
    exec(c, {0x9B});
    exec(c, {0x23});
    exec(c, {0x11});
    EXPECT_EQ(c.a, 5);
    EXPECT_EQ(c.cf, 1);
    exec(c, {0x93});
    exec(c, {0x21});
    exec(c, {0x11});
    EXPECT_EQ(c.a, 0xD);
    EXPECT_EQ(c.cf, 0);
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

TEST(Mb88Isa, ComparesLeaveAAlone) {
    Mb88 c = cpu();
    poke(c, 0, 0, 5);
    exec(c, {0x95});
    exec(c, {0x2E});
    EXPECT_EQ(c.cf, 0);
    EXPECT_EQ(c.st, 0);
    EXPECT_EQ(c.zf, 1);
    poke(c, 0, 0, 3);
    exec(c, {0x95});
    exec(c, {0x2E});
    EXPECT_EQ(c.cf, 1);
    EXPECT_EQ(c.st, 1);
    EXPECT_EQ(c.zf, 0);
    EXPECT_EQ(c.a, 5);
    exec(c, {0x94});
    exec(c, {0xB4});
    EXPECT_EQ(c.cf, 0);
    EXPECT_EQ(c.st, 0);
    EXPECT_EQ(c.zf, 1);
    exec(c, {0x95});
    exec(c, {0xB3});
    EXPECT_EQ(c.cf, 1);
    EXPECT_EQ(c.st, 1);
    exec(c, {0x82});
    exec(c, {0xA7});
    EXPECT_EQ(c.cf, 0);
    EXPECT_EQ(c.st, 1);
    EXPECT_EQ(c.zf, 0);
    exec(c, {0xA2});
    EXPECT_EQ(c.st, 0);
    EXPECT_EQ(c.zf, 1);
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
    exec(c, {0x91});
    exec(c, {0x29});
    EXPECT_EQ(c.st, 1);
    exec(c, {0x94});
    exec(c, {0x4E});
    EXPECT_EQ(c.st, 0);
    exec(c, {0x4C});
    EXPECT_EQ(c.st, 1);
}

TEST(Mb88Isa, JmpTakenOnlyWhenStIsSet) {
    Mb88 c = cpu();
    c.rom[0] = 0x91;
    c.rom[1] = 0x7F;  // AI F: carry, ST=0
    c.rom[2] = 0xC8;  // JMP $08, skipped
    c.rom[3] = 0xCA;  // JMP $0A
    c.step();
    c.step();
    EXPECT_EQ(c.st, 0);
    c.step();
    EXPECT_EQ(c.pc_full(), 3);
    EXPECT_EQ(c.st, 1);
    c.step();
    EXPECT_EQ(c.pc_full(), 0x0A);
}

TEST(Mb88Isa, JplLoadsPageAndKeepsItForShortJumps) {
    Mb88 c = cpu();
    c.rom[0] = 0x6B;
    c.rom[1] = 0x85;  // JPL $385
    c.rom[0x385] = 0xD0;  // JMP $10 within page $0E
    EXPECT_EQ(c.step(), 2);
    EXPECT_EQ(c.pc_full(), 0x385);
    c.step();
    EXPECT_EQ(c.pc_full(), 0x390);
}

TEST(Mb88Isa, JplNotTakenSkipsItsOperand) {
    Mb88 c = cpu();
    c.rom[0] = 0x91;
    c.rom[1] = 0x7F;
    c.rom[2] = 0x6B;
    c.rom[3] = 0x85;
    c.step();
    c.step();
    EXPECT_EQ(c.step(), 2);
    EXPECT_EQ(c.pc_full(), 4);
    EXPECT_EQ(c.st, 1);
}

TEST(Mb88Isa, CallAndRtsUseAFourDeepStack) {
    Mb88 c = cpu();
    c.rom[0] = 0x60;
    c.rom[1] = 0x20;
    c.rom[0x20] = 0x2C;
    EXPECT_EQ(c.step(), 2);
    EXPECT_EQ(c.pc_full(), 0x20);
    EXPECT_EQ(c.si, 1);
    c.step();
    EXPECT_EQ(c.pc_full(), 2);
    EXPECT_EQ(c.si, 0);

    Mb88 d = cpu();
    for (int i = 0; i < 5; i++) {
        d.rom[unsigned(i * 8)] = 0x60;
        d.rom[unsigned(i * 8 + 1)] = uint8_t((i + 1) * 8);
    }
    d.rom[40] = 0x2C;
    for (int i = 0; i < 5; i++) d.step();
    EXPECT_EQ(d.si, 1) << "SI wraps after four";
    d.step();
    EXPECT_EQ(d.pc_full(), 34) << "the fifth return address overwrote the first";
}

TEST(Mb88Isa, JpaJumpsToFourTimesA) {
    Mb88 c = cpu();
    exec(c, {0x93});
    EXPECT_EQ(exec(c, {0x3D, 0x05}), 2);
    EXPECT_EQ(c.pc_full(), 5 * 64 + 12);
}

TEST(Mb88Isa, PcCarriesIntoThePage) {
    Mb88 c = cpu();
    c.rom[0] = 0xFF;  // JMP $3F
    c.step();
    c.step();
    EXPECT_EQ(c.pc_full(), 0x40);
    EXPECT_EQ(c.pa, 1);
}

TEST(Mb88Isa, OutoSelectsTheNibbleWithCarry) {
    Ports io;
    Mb88 c = cpu(io);
    exec(c, {0x95});
    exec(c, {0x01});
    EXPECT_EQ(io.o, 0x05);
    EXPECT_EQ(io.o_mask, 0x0F);
    exec(c, {0x9A});
    exec(c, {0x21});
    exec(c, {0x01});
    EXPECT_EQ(io.o, 0xA5);
    EXPECT_EQ(io.o_mask, 0xF0);
    EXPECT_EQ(c.o_output, 0xA5);
}

TEST(Mb88Isa, OutpAndOutrAndInputs) {
    Ports io;
    Mb88 c = cpu(io);
    exec(c, {0x96});
    exec(c, {0x02});
    EXPECT_EQ(io.p, 6);
    EXPECT_EQ(io.p_writes, 1);
    exec(c, {0x82});
    exec(c, {0x97});
    exec(c, {0x03});
    EXPECT_EQ(io.r[2], 7);
    io.r[1] = 0x3C;
    exec(c, {0x81});
    exec(c, {0x13});
    EXPECT_EQ(c.a, 0xC) << "R ports are four bits";
    io.k = 0x10;
    exec(c, {0x12});
    EXPECT_EQ(c.a, 0);
    EXPECT_EQ(c.zf, 1);
    io.k = 0x09;
    exec(c, {0x12});
    EXPECT_EQ(c.a, 9);
}

TEST(Mb88Isa, RBitOpsAddressPortYOver4) {
    Ports io;
    Mb88 c = cpu(io);
    exec(c, {0x86});
    exec(c, {0x20});
    EXPECT_EQ(io.r[1], 4);
    exec(c, {0x24});
    EXPECT_EQ(c.st, 0);
    exec(c, {0x22});
    EXPECT_EQ(io.r[1], 0);
    exec(c, {0x24});
    EXPECT_EQ(c.st, 1);
}

TEST(Mb88Isa, DBitOpsUsePortsZeroAndTwo) {
    Ports io;
    Mb88 c = cpu(io);
    exec(c, {0x41});
    EXPECT_EQ(io.r[0], 2);
    exec(c, {0x43});
    EXPECT_EQ(io.r[0], 0xA);
    exec(c, {0x45});
    EXPECT_EQ(io.r[0], 8);
    io.r[2] = 4;
    exec(c, {0x4A});
    EXPECT_EQ(c.st, 0);
    exec(c, {0x48});
    EXPECT_EQ(c.st, 1);
}

TEST(Mb88Isa, EnAndDisEditPio) {
    Mb88 c = cpu();
    EXPECT_EQ(exec(c, {0x3E, 0x84}), 2);
    EXPECT_EQ(c.pio, 0x84);
    exec(c, {0x3F, 0x80});
    EXPECT_EQ(c.pio, 0x04);
}

TEST(Mb88Timer, InternalClockCountsEvery32Cycles) {
    Mb88 c = cpu();
    exec(c, {0x3E, 0x80});
    for (int i = 0; i < 29; i++) exec(c, {0x00});
    EXPECT_EQ(c.tl, 0);
    exec(c, {0x00});
    EXPECT_EQ(c.tl, 1);
}

TEST(Mb88Timer, OverflowSetsVfAndTstvClearsIt) {
    Mb88 c = cpu();
    exec(c, {0x9F});
    exec(c, {0x05});
    exec(c, {0x06});
    exec(c, {0x3E, 0x80});
    for (int i = 0; i < 30; i++) exec(c, {0x00});
    EXPECT_EQ(c.th, 0);
    EXPECT_EQ(c.tl, 0);
    EXPECT_EQ(c.vf, 1);
    exec(c, {0x26});
    EXPECT_EQ(c.st, 0);
    EXPECT_EQ(c.vf, 0);
    exec(c, {0x26});
    EXPECT_EQ(c.st, 1);
}

TEST(Mb88Timer, OverflowInterruptVectorsTo04) {
    Mb88 c = cpu();
    exec(c, {0x9F});
    exec(c, {0x05});
    exec(c, {0x06});
    exec(c, {0x3E, 0x82});
    int n = 0;
    while (!c.in_irq && n++ < 40) exec(c, {0x00});
    ASSERT_TRUE(c.in_irq);
    EXPECT_EQ(c.pc_full(), 0x04);
}

TEST(Mb88Timer, ExternalClockCountsFallingTcEdges) {
    Mb88 c = cpu();
    c.set_tc(true);
    c.set_tc(false);
    EXPECT_EQ(c.tl, 0) << "TC counts only when enabled";
    exec(c, {0x3E, 0x40});
    c.set_tc(true);
    EXPECT_EQ(c.tl, 0);
    c.set_tc(false);
    EXPECT_EQ(c.tl, 1);
}

TEST(Mb88Irq, ExternalRisingEdgeVectorsTo02AndRtiRestoresFlags) {
    Mb88 c = cpu();
    c.rom[0] = 0x3E;
    c.rom[1] = 0x04;
    c.rom[2] = 0x90;  // LI 0 in the handler
    c.rom[3] = 0x3C;  // RTI
    c.rom[0x10] = 0x21;  // SETC
    c.rom[0x11] = 0x00;
    c.rom[0x12] = 0x00;
    c.step();
    c.pc = 0x10;
    c.step();
    c.step();
    ASSERT_EQ(c.cf, 1);
    c.set_irq(true);
    EXPECT_EQ(c.step(), 1 + 3) << "interrupt entry costs three cycles";
    EXPECT_TRUE(c.in_irq);
    EXPECT_EQ(c.pc_full(), 0x02);
    c.step();
    c.cf = 0;
    EXPECT_EQ(c.zf, 1);
    c.step();
    EXPECT_FALSE(c.in_irq);
    EXPECT_EQ(c.pc_full(), 0x13);
    EXPECT_EQ(c.cf, 1);
    EXPECT_EQ(c.zf, 0);
    c.step();
    EXPECT_FALSE(c.in_irq) << "a held line does not retrigger";
}

TEST(Mb88Irq, EdgeIsIgnoredWhileDisabled) {
    Mb88 c = cpu();
    c.set_irq(true);
    exec(c, {0x3E, 0x04});
    exec(c, {0x00});
    EXPECT_FALSE(c.in_irq);
    exec(c, {0x25});
    EXPECT_EQ(c.st, 0) << "TSTI still reads the pin";
    c.set_irq(false);
    exec(c, {0x25});
    EXPECT_EQ(c.st, 1);
}

TEST(Mb88Irq, PendingWaitsForRti) {
    Mb88 c = cpu();
    c.rom[0] = 0x3E;
    c.rom[1] = 0x06;
    c.rom[2] = 0x00;
    c.rom[3] = 0x3C;
    c.rom[4] = 0x00;
    c.step();
    c.set_irq(true);
    c.step();
    ASSERT_TRUE(c.in_irq);
    ASSERT_EQ(c.pc_full(), 0x02);
    c.th = c.tl = 0xF;
    c.set_tc(true);
    c.pio |= 0x40;
    c.set_tc(false);
    ASSERT_EQ(c.vf, 1);
    c.step();
    EXPECT_EQ(c.pc_full(), 0x03) << "no nesting";
    c.step();
    EXPECT_TRUE(c.in_irq) << "the timer interrupt fires as RTI completes";
    EXPECT_EQ(c.pc_full(), 0x04);
}
