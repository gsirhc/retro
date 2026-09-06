// GoogleTest suite for the 8253 PIT: control-word channel/access/mode
// decode, reload programming, the counter-latch readback command, the
// channel-2 gate (port 0x61 bit 0), and tick()'s edge-rate output.
//
// Tests that need an exact tick count use cpu_hz == the PIT's own 1.193182
// MHz clock, so tick()'s internal PIT-clocks-per-CPU-cycle ratio is exactly
// 1.0 and elapsed-clock counts are integer-exact -- avoids floating-point
// rounding noise in the assertions.

#include <gtest/gtest.h>

#include "pit8253.h"

namespace {

using ibmpcat::Pit8253;
constexpr double kPitHz = 1193182.0;

TEST(Pit8253Test, ProgramChannel0Mode3ReloadFour) {
    Pit8253 pit;
    pit.out(0x43, 0x36);  // channel 0, access LSB-then-MSB, mode 3
    pit.out(0x40, 4);
    pit.out(0x40, 0);
    // toggle_period = reload/2 = 2, so a rising edge occurs every 4 PIT clocks.
    EXPECT_EQ(pit.tick(2, kPitHz), 0);
    EXPECT_EQ(pit.tick(4, kPitHz), 1);
    EXPECT_EQ(pit.tick(6, kPitHz), 0);
    EXPECT_EQ(pit.tick(8, kPitHz), 1);
}

TEST(Pit8253Test, Channel0DivisorZeroMeansSixtyFiveThousandFiveThirtySix) {
    // The real AT BIOS programs channel 0 with a divisor of 0 (meaning
    // 65536) for the ~18.2 Hz DOS timer tick: 1193182/65536 = 18.2 Hz.
    Pit8253 pit;
    pit.out(0x43, 0x36);
    pit.out(0x40, 0);
    pit.out(0x40, 0);
    // One full period is 65536 PIT clocks -- not yet elapsed at 65535.
    EXPECT_EQ(pit.tick(65535, kPitHz), 0);
    EXPECT_EQ(pit.tick(65536, kPitHz), 1);
}

TEST(Pit8253Test, Gate2FreezesChannelTwoCounter) {
    // tick()'s return value only ever reports channel 0's edges (that's the
    // one wired to PIC IRQ0) -- channel 2's state is read directly via
    // channel2_output(), which is what pcspeaker.h will poll in a later phase.
    Pit8253 pit;
    pit.out(0x43, 0xB6);  // channel 2 (10), access LSB-then-MSB, mode 3
    pit.out(0x42, 4);
    pit.out(0x42, 0);
    ASSERT_TRUE(pit.channel2_output());
    pit.set_gate2(false);
    pit.tick(100, kPitHz);  // channel 2 must not advance while gated low
    EXPECT_TRUE(pit.channel2_output());
    pit.set_gate2(true);
    // toggle_period = 2 -- first toggle (high->low) 2 clocks after the gate reopens.
    pit.tick(102, kPitHz);
    EXPECT_FALSE(pit.channel2_output());
    pit.tick(104, kPitHz);
    EXPECT_TRUE(pit.channel2_output());
}

TEST(Pit8253Test, Gate2LowForcesOutputHighEvenMidCycle) {
    // Real Mode 3 hardware forces the output high the instant GATE drops,
    // regardless of what phase it was in -- not merely whatever it
    // happened to be. This is what lets pcspeaker.h's direct-toggle
    // "digitized" playback technique rely on a clean, predictable high
    // baseline from the PIT side while it drives the speaker itself via
    // the Speaker Data Enable bit.
    Pit8253 pit;
    pit.out(0x43, 0xB6);  // channel 2, access LSB-then-MSB, mode 3
    pit.out(0x42, 4);
    pit.out(0x42, 0);
    pit.tick(2, kPitHz);  // one full toggle_period -- output is now low
    ASSERT_FALSE(pit.channel2_output());
    pit.set_gate2(false);
    EXPECT_TRUE(pit.channel2_output());
}

TEST(Pit8253Test, Gate2RisingEdgeReloadsCounterInsteadOfResumingMidPhase) {
    // Real Mode 3 hardware: GATE's rising edge reloads the counter, so the
    // square wave restarts cleanly from the beginning of its period
    // instead of resuming from wherever it was frozen.
    Pit8253 pit;
    pit.out(0x43, 0xB6);
    pit.out(0x42, 4);
    pit.out(0x42, 0);
    pit.tick(1, kPitHz);  // one clock into the period (counter now 1 of 2)
    pit.set_gate2(false);
    pit.set_gate2(true);  // reloads the counter back to 2
    pit.tick(2, kPitHz);  // one more clock -- would have toggled already
    EXPECT_TRUE(pit.channel2_output());  // ...if it had merely resumed at 1
    pit.tick(3, kPitHz);  // second clock since the reload -- now it toggles
    EXPECT_FALSE(pit.channel2_output());
}

TEST(Pit8253Test, CounterLatchFreezesAConsistentSnapshot) {
    Pit8253 pit;
    pit.out(0x43, 0x76);  // channel 1 (01), access LSB-then-MSB, mode 3
    pit.out(0x41, 100);
    pit.out(0x41, 0);
    // toggle_period = 50 -- latch immediately, before any ticking.
    pit.out(0x43, 0x40);  // latch command, channel 1, rw=00
    EXPECT_EQ(pit.in(0x41), 50);
    EXPECT_EQ(pit.in(0x41), 0);
    // Advance 10 PIT clocks, then latch again -- expect 50-10 = 40.
    pit.tick(10, kPitHz);
    pit.out(0x43, 0x40);
    EXPECT_EQ(pit.in(0x41), 40);
    EXPECT_EQ(pit.in(0x41), 0);
}

TEST(Pit8253Test, LsbOnlyAccessModeProgramsReloadFromOneByte) {
    Pit8253 pit;
    pit.out(0x43, 0x16);  // channel 0, access LSB-only, mode 3
    pit.out(0x40, 8);     // reload = 8 (single-byte access -- MSB implicitly 0)
    // toggle_period = 4 -> first toggle (high->low) at clock 4, no rise yet;
    // rising edge (low->high) at clock 8.
    EXPECT_EQ(pit.tick(4, kPitHz), 0);
    EXPECT_EQ(pit.tick(8, kPitHz), 1);
}

}  // namespace
