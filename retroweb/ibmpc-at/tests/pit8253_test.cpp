// GoogleTest suite for the 8254 PIT (Intel 8254 data sheet 231164, Mode Definitions).
// Exact-count tests use cpu_hz == the PIT's 1.193182 MHz so the clock ratio is 1.0.

#include <gtest/gtest.h>

#include <string>

#include "pit8253.h"

namespace {

using ibmpcat::Pit8253;
constexpr double kPitHz = 1193182.0;

// Steps the PIT one clock at a time, tracking the absolute clock count.
struct Clock {
    explicit Clock(Pit8253 &p) : pit(p) {}
    Pit8253 &pit;
    uint64_t now = 0;
    int step(int n = 1) {
        int rises = 0;
        for (int i = 0; i < n; ++i) rises += pit.tick(++now, kPitHz);
        return rises;
    }
    // Channel 2's OUT, one character per clock: H or L.
    std::string trace2(int n) {
        std::string s;
        for (int i = 0; i < n; ++i) {
            step();
            s += pit.channel2_output() ? 'H' : 'L';
        }
        return s;
    }
};

uint16_t ReadCount(Pit8253 &pit, uint16_t port) {
    uint16_t lo = pit.in(port);
    return uint16_t(lo | (pit.in(port) << 8));
}

void Program2(Pit8253 &pit, uint8_t mode, uint16_t count) {
    pit.out(0x43, uint8_t(0xB0 | (mode << 1)));  // channel 2, LSB then MSB, binary
    pit.out(0x42, uint8_t(count & 0xFF));
    pit.out(0x42, uint8_t(count >> 8));
}

// ---- mode 3 (square wave) ----

TEST(Pit8253Test, Mode3EvenCountRisesEveryNClocksAfterTheLoadClock) {
    Pit8253 pit;
    Clock c{pit};
    pit.out(0x43, 0x36);  // channel 0, LSB then MSB, mode 3
    pit.out(0x40, 4);
    pit.out(0x40, 0);
    // Clock 1 loads the count; low at clock 3, high again at clock 5.
    EXPECT_EQ(c.step(4), 0);
    EXPECT_EQ(c.step(), 1);
    EXPECT_EQ(c.step(3), 0);
    EXPECT_EQ(c.step(), 1);
}

TEST(Pit8253Test, Channel0DivisorZeroMeansSixtyFiveThousandFiveThirtySix) {
    // The AT BIOS programs channel 0 with divisor 0 (65536): 1193182/65536 = 18.2 Hz.
    Pit8253 pit;
    pit.out(0x43, 0x36);
    pit.out(0x40, 0);
    pit.out(0x40, 0);
    EXPECT_EQ(pit.tick(65536, kPitHz), 0);  // load clock + 65535
    EXPECT_EQ(pit.tick(65537, kPitHz), 1);
    EXPECT_EQ(pit.tick(65537 + 65535, kPitHz), 0);
    EXPECT_EQ(pit.tick(65537 + 65536, kPitHz), 1);
}

TEST(Pit8253Test, Mode3OddCountIsHighOneClockLongerThanLow) {
    // Data sheet: "OUT will be high for (N+1)/2 counts and low for (N-1)/2".
    Pit8253 pit;
    Clock c{pit};
    Program2(pit, 3, 5);
    EXPECT_EQ(c.trace2(1 + 15), "H" "HHLL" "HHHLL" "HHHLL" "H");
}

TEST(Pit8253Test, Mode3CounterDecrementsByTwo) {
    Pit8253 pit;
    Clock c{pit};
    Program2(pit, 3, 10);
    c.step();  // load
    EXPECT_EQ(ReadCount(pit, 0x42), 10);
    c.step();
    EXPECT_EQ(ReadCount(pit, 0x42), 8);
    c.step(4);
    EXPECT_EQ(ReadCount(pit, 0x42), 10);  // expired and reloaded, now low
    EXPECT_FALSE(pit.channel2_output());
}

TEST(Pit8253Test, Mode3OddCountReadsBackEvenValues) {
    // Odd counts load N-1, so the counter only ever shows even values.
    Pit8253 pit;
    Clock c{pit};
    Program2(pit, 3, 7);
    c.step();
    EXPECT_EQ(ReadCount(pit, 0x42), 6);
    c.step();
    EXPECT_EQ(ReadCount(pit, 0x42), 4);
}

TEST(Pit8253Test, Mode3NewCountTakesEffectAtTheEndOfTheHalfCycle) {
    Pit8253 pit;
    Clock c{pit};
    Program2(pit, 3, 8);
    c.step(2);  // loaded, one decrement in: 6 left
    pit.out(0x42, 4);
    pit.out(0x42, 0);
    // The current half-cycle finishes at the old count, then halves of 2.
    EXPECT_EQ(c.trace2(7), "HHL" "LHH" "L");
}

TEST(Pit8253Test, Gate2LowFreezesModeThreeAndRisingEdgeRestartsIt) {
    Pit8253 pit;
    Clock c{pit};
    pit.set_gate2(false);
    Program2(pit, 3, 4);
    c.step(100);
    EXPECT_TRUE(pit.channel2_output());
    pit.set_gate2(true);
    // The rising edge reloads on the next clock, then halves of 2.
    EXPECT_EQ(c.trace2(5), "HHLLH");
}

TEST(Pit8253Test, Gate2LowForcesOutputHighEvenMidCycle) {
    // Modes 2 and 3 force OUT high when GATE drops; pcspeaker.h direct-toggle playback relies on it.
    Pit8253 pit;
    Clock c{pit};
    Program2(pit, 3, 4);
    c.step(3);
    ASSERT_FALSE(pit.channel2_output());
    pit.set_gate2(false);
    EXPECT_TRUE(pit.channel2_output());
}

TEST(Pit8253Test, Gate2RisingEdgeReloadsInsteadOfResumingMidPhase) {
    Pit8253 pit;
    Clock c{pit};
    Program2(pit, 3, 4);
    c.step(2);  // loaded, then counter at 2: one clock from going low
    pit.set_gate2(false);
    pit.set_gate2(true);
    EXPECT_EQ(c.trace2(3), "HHL");  // reload clock, then a full half
}

TEST(Pit8253Test, LsbOnlyAccessProgramsTheCountFromOneByte) {
    Pit8253 pit;
    Clock c{pit};
    pit.out(0x43, 0x16);  // channel 0, LSB only, mode 3
    pit.out(0x40, 8);
    EXPECT_EQ(c.step(8), 0);
    EXPECT_EQ(c.step(), 1);
}

// ---- mode 0 (interrupt on terminal count) ----

TEST(Pit8253Test, Mode0OutGoesLowOnControlWordAndHighNPlusOneClocksAfterTheCount) {
    Pit8253 pit;
    Clock c{pit};
    pit.out(0x43, 0x30);  // channel 0, LSB then MSB, mode 0
    pit.out(0x40, 10);
    pit.out(0x40, 0);
    EXPECT_EQ(c.step(10), 0);
    EXPECT_EQ(c.step(), 1);
    // One shot: the counter wraps and keeps going, OUT stays high.
    EXPECT_EQ(c.step(200000), 0);
}

TEST(Pit8253Test, Mode0WrapsToFFFFAfterTerminalCount) {
    Pit8253 pit;
    Clock c{pit};
    Program2(pit, 0, 3);
    EXPECT_EQ(c.trace2(5), "LLLHH");
    EXPECT_EQ(ReadCount(pit, 0x42), 0xFFFF);
}

TEST(Pit8253Test, Mode0FirstByteStopsCountingAndDropsOut) {
    Pit8253 pit;
    Clock c{pit};
    Program2(pit, 0, 3);
    c.step(5);
    ASSERT_TRUE(pit.channel2_output());
    uint16_t before = ReadCount(pit, 0x42);
    pit.out(0x42, 5);  // LSB only so far
    EXPECT_FALSE(pit.channel2_output());
    c.step(10);
    EXPECT_EQ(ReadCount(pit, 0x42), before);
    pit.out(0x42, 0);
    EXPECT_EQ(c.trace2(6), "LLLLLH");
}

TEST(Pit8253Test, Mode0GateLowPausesCountingWithoutTouchingOut) {
    Pit8253 pit;
    Clock c{pit};
    Program2(pit, 0, 4);
    c.step(2);  // loaded, 3 left
    pit.set_gate2(false);
    c.step(50);
    EXPECT_FALSE(pit.channel2_output());
    EXPECT_EQ(ReadCount(pit, 0x42), 3);
    pit.set_gate2(true);
    EXPECT_EQ(c.trace2(3), "LLH");
}

// ---- mode 1 (hardware retriggerable one-shot) ----

TEST(Pit8253Test, Mode1WaitsForAGateTriggerThenPulsesLowForNClocks) {
    Pit8253 pit;
    Clock c{pit};
    Program2(pit, 1, 3);
    EXPECT_EQ(c.trace2(10), "HHHHHHHHHH");
    pit.set_gate2(false);
    pit.set_gate2(true);
    EXPECT_EQ(c.trace2(6), "LLLHHH");
}

TEST(Pit8253Test, Mode1RetriggerRestartsTheShot) {
    Pit8253 pit;
    Clock c{pit};
    Program2(pit, 1, 4);
    pit.set_gate2(false);
    pit.set_gate2(true);
    c.step(2);
    pit.set_gate2(false);
    pit.set_gate2(true);
    EXPECT_EQ(c.trace2(6), "LLLLHH");
}

TEST(Pit8253Test, Mode1OnChannel0NeverFiresBecauseItsGateIsTiedHigh) {
    Pit8253 pit;
    Clock c{pit};
    pit.out(0x43, 0x32);  // channel 0, mode 1
    pit.out(0x40, 10);
    pit.out(0x40, 0);
    EXPECT_EQ(c.step(1000), 0);
}

// ---- mode 2 (rate generator) ----

TEST(Pit8253Test, Mode2OutIsLowForOneClockPerPeriod) {
    Pit8253 pit;
    Clock c{pit};
    Program2(pit, 2, 4);
    EXPECT_EQ(c.trace2(1 + 12), "H" "HHLH" "HHLH" "HHLH");
}

TEST(Pit8253Test, Mode2Channel0RaisesOneEdgePerPeriod) {
    Pit8253 pit;
    Clock c{pit};
    pit.out(0x43, 0x34);  // channel 0, LSB then MSB, mode 2
    pit.out(0x40, 100);
    pit.out(0x40, 0);
    EXPECT_EQ(c.step(1 + 1000), 10);
}

TEST(Pit8253Test, Mode2NewCountTakesEffectAtTheNextReload) {
    Pit8253 pit;
    Clock c{pit};
    Program2(pit, 2, 5);
    c.step(2);  // loaded, 4 left
    pit.out(0x42, 3);
    pit.out(0x42, 0);
    // The old count runs out (low at 1), then periods of 3.
    EXPECT_EQ(c.trace2(9), "HHL" "HHL" "HHL");
}

TEST(Pit8253Test, ModeSixAndSevenAliasTwoAndThree) {
    Pit8253 pit;
    Clock c{pit};
    pit.out(0x43, 0xBC);  // channel 2, LSB then MSB, M=110
    pit.out(0x42, 4);
    pit.out(0x42, 0);
    EXPECT_EQ(c.trace2(5), "HHHLH");
    pit.out(0x43, 0xE8);  // read-back status, channel 2
    EXPECT_EQ(pit.in(0x42) & 0x3F, 0x3C);  // status keeps the bits as written
}

// ---- modes 4 and 5 (strobes) ----

TEST(Pit8253Test, Mode4StrobesLowForOneClockOnce) {
    Pit8253 pit;
    Clock c{pit};
    Program2(pit, 4, 3);
    EXPECT_EQ(c.trace2(8), "HHHLHHHH");
    c.step(70000);  // wraps through zero again with no second strobe
    EXPECT_TRUE(pit.channel2_output());
    Program2(pit, 4, 2);  // a new count retriggers
    EXPECT_EQ(c.trace2(4), "HHLH");
}

TEST(Pit8253Test, Mode5StrobesAfterAGateTrigger) {
    Pit8253 pit;
    Clock c{pit};
    Program2(pit, 5, 3);
    EXPECT_EQ(c.trace2(10), "HHHHHHHHHH");
    pit.set_gate2(false);
    pit.set_gate2(true);
    EXPECT_EQ(c.trace2(6), "HHHLHH");
}

// ---- latch, read-back, BCD, flip-flops ----

TEST(Pit8253Test, CounterLatchFreezesAConsistentSnapshot) {
    Pit8253 pit;
    Clock c{pit};
    pit.out(0x43, 0x70);  // channel 1, LSB then MSB, mode 0
    pit.out(0x41, 100);
    pit.out(0x41, 0);
    c.step();  // load
    pit.out(0x43, 0x40);
    c.step(10);
    EXPECT_EQ(ReadCount(pit, 0x41), 100);
    pit.out(0x43, 0x40);
    EXPECT_EQ(ReadCount(pit, 0x41), 90);
}

TEST(Pit8253Test, SecondLatchBeforeReadIsIgnored) {
    Pit8253 pit;
    Clock c{pit};
    Program2(pit, 0, 100);
    c.step();
    pit.out(0x43, 0x80);  // latch channel 2
    c.step(5);
    pit.out(0x43, 0x80);
    EXPECT_EQ(ReadCount(pit, 0x42), 100);
    EXPECT_EQ(ReadCount(pit, 0x42), 95);  // live again
}

TEST(Pit8253Test, ReadBackStatusReportsOutNullCountAndControlBits) {
    Pit8253 pit;
    Clock c{pit};
    pit.out(0x43, 0x30);  // channel 0, LSB then MSB, mode 0, binary
    pit.out(0x43, 0xE2);  // read-back status, channel 0
    EXPECT_EQ(pit.in(0x40), 0x40 | 0x30);  // OUT low, null count, RW=11 M=000
    pit.out(0x40, 2);
    pit.out(0x40, 0);
    c.step();  // count reaches the counting element
    pit.out(0x43, 0xE2);
    EXPECT_EQ(pit.in(0x40), 0x30);
    c.step(2);
    pit.out(0x43, 0xE2);
    EXPECT_EQ(pit.in(0x40), 0x80 | 0x30);
}

TEST(Pit8253Test, ReadBackLatchesStatusThenCountForSeveralChannels) {
    Pit8253 pit;
    Clock c{pit};
    pit.out(0x43, 0x30);
    pit.out(0x40, 50);
    pit.out(0x40, 0);
    Program2(pit, 0, 70);
    c.step(5);
    pit.out(0x43, 0xC0 | 0x08 | 0x02);  // count and status, channels 0 and 2
    c.step(5);
    EXPECT_EQ(pit.in(0x40), 0x30);
    EXPECT_EQ(ReadCount(pit, 0x40), 46);
    EXPECT_EQ(pit.in(0x42), 0x30);
    EXPECT_EQ(ReadCount(pit, 0x42), 66);
}

TEST(Pit8253Test, ReadBackCountOnlyLeavesStatusUnlatched) {
    Pit8253 pit;
    Clock c{pit};
    Program2(pit, 0, 70);
    c.step();
    pit.out(0x43, 0xD8);  // latch count only, channel 2
    c.step(3);
    EXPECT_EQ(ReadCount(pit, 0x42), 70);
}

TEST(Pit8253Test, BcdCountsInDecimalAndWrapsAtNineThousandNineNinetyNine) {
    Pit8253 pit;
    Clock c{pit};
    pit.out(0x43, 0xB1);  // channel 2, LSB then MSB, mode 0, BCD
    pit.out(0x42, 0x12);
    pit.out(0x42, 0x00);  // 12 decimal
    c.step(4);
    EXPECT_EQ(ReadCount(pit, 0x42), 0x0009);
    c.step(10);
    EXPECT_TRUE(pit.channel2_output());
    EXPECT_EQ(ReadCount(pit, 0x42), 0x9999);
}

TEST(Pit8253Test, BcdZeroMeansTenThousand) {
    Pit8253 pit;
    pit.out(0x43, 0x35);  // channel 0, mode 2, BCD
    pit.out(0x40, 0);
    pit.out(0x40, 0);
    EXPECT_EQ(pit.tick(1 + 9999, kPitHz), 0);
    EXPECT_EQ(pit.tick(1 + 10000, kPitHz), 1);
}

TEST(Pit8253Test, ReadAndWriteFlipFlopsAreSeparate) {
    // Data sheet: read LSB, write LSB, read MSB, write MSB is a legal sequence.
    Pit8253 pit;
    Clock c{pit};
    Program2(pit, 0, 0x1234);
    c.step();
    EXPECT_EQ(pit.in(0x42), 0x34);
    pit.out(0x42, 0x10);
    EXPECT_EQ(pit.in(0x42), 0x12);
    pit.out(0x42, 0x00);
    c.step();
    EXPECT_EQ(ReadCount(pit, 0x42), 0x0010);
}

TEST(Pit8253Test, ControlWordThatRaisesChannel0OutCountsAsAnEdge) {
    Pit8253 pit;
    Clock c{pit};
    pit.out(0x43, 0x30);  // mode 0: OUT low
    pit.out(0x43, 0x34);  // mode 2: OUT high, an edge IRQ0 sees
    EXPECT_EQ(c.step(), 1);
}

TEST(Pit8253Test, Mode2CountZeroReadsBackElapsedClocksFrom65536) {
    // The Zen timer's scheme: mode 2, count 0, latch, negate (Abrash, PZTIMER.ASM).
    Pit8253 pit;
    Clock c{pit};
    pit.out(0x43, 0x34);
    pit.out(0x40, 0x00);
    pit.out(0x40, 0x00);
    c.step(1 + 500);
    pit.out(0x43, 0x00);
    EXPECT_EQ(uint16_t(-ReadCount(pit, 0x40)), 500);
}

TEST(Pit8253Test, Channel1RefreshRisesOncePerCount) {
    // IBM POST: mode 2, count 18 (Technical Reference, System Timers).
    Pit8253 pit;
    pit.out(0x43, 0x54);
    pit.out(0x41, 0x12);
    int rises = 0;
    for (uint64_t now = 1; now <= 1 + 18 * 100; ++now) pit.tick(now, kPitHz, &rises);
    EXPECT_EQ(rises, 100);
}

}  // namespace
