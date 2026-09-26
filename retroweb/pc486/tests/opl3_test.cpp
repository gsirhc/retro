// GoogleTest suite for the Yamaha YMF262 (OPL3) FM synthesizer: the AdLib
// detection sequence every period driver runs before it will touch the FM
// chip at all, timer 1/timer 2 periods and masking, the status byte's exact
// bit layout (an OPL3 tell versus an OPL2), the NEW bit that gates OPL3-only
// registers and bank 1, and audible key-on/key-off behavior including
// stereo panning.
//
// Register/timing details are checked against opl3.h's own contract (which
// cites the YMF262-M datasheet and the Sound Blaster Series Hardware
// Programming Guide Appendix B) rather than against another emulator.

#include <gtest/gtest.h>

#include "opl3.h"

#include <cstdint>
#include <vector>

namespace {

using pc486::Opl3;

class Opl3Test : public ::testing::Test {
protected:
    Opl3 opl;
    uint64_t cycles_ = 0;

    void SetUp() override { opl.reset(); }

    // Every register write goes through the address/data pair for the given
    // bank, exactly as chipset.cpp's port decode would drive it.
    void Reg(int bank, uint8_t index, uint8_t v) {
        opl.write_address(bank, index);
        opl.write_data(bank, v);
    }

    // Advances the chip by a raw CPU-cycle delta, accumulating a running
    // cycle count the way Machine::run_cycles() feeds tick() -- never reset
    // mid-test, since tick() computes its own delta from the previous call.
    void AdvanceCycles(uint64_t delta) {
        cycles_ += delta;
        opl.tick(cycles_);
    }

    // Converts a real elapsed time to CPU cycles at this machine's 66 MHz
    // clock (Opl3::kCpuHz) so tests express intent in microseconds instead
    // of hand-computed cycle counts.
    void AdvanceMicroseconds(double us) {
        AdvanceCycles(uint64_t(us * 1e-6 * Opl3::kCpuHz));
    }

    // Programs channel 0 as a plain 2-op FM voice: operator slots 0
    // (modulator) and 3 (carrier), a non-zero F-number/BLOCK, full total
    // level, the fastest attack, EGT held sustain at full volume, and the
    // fastest release. None of these values are load-bearing for pitch or
    // timbre -- the point is only that key-on must produce something
    // non-zero and key-off must decay to nothing within a short, bounded
    // time.
    void SetUpAudibleChannel(uint8_t pan = 0x30) {
        Reg(0, 0x20, 0x21);  // modulator: EGT hold, MULT=1
        Reg(0, 0x23, 0x21);  // carrier: EGT hold, MULT=1
        Reg(0, 0x40, 0x00);  // modulator TL: loudest
        Reg(0, 0x43, 0x00);  // carrier TL: loudest
        Reg(0, 0x60, 0xF0);  // modulator AR=15 (fastest attack), DR=0
        Reg(0, 0x63, 0xF0);  // carrier AR=15, DR=0
        Reg(0, 0x80, 0x0F);  // modulator SL=0 (loudest sustain), RR=15 (fastest release)
        Reg(0, 0x83, 0x0F);  // carrier SL=0, RR=15
        Reg(0, 0xC0, pan);
        Reg(0, 0xA0, 0xAE);  // F-number low byte; value itself is arbitrary
    }
    // BLOCK=4, F-number high bits=2, KON set/clear -- channel 0's B0h.
    void KeyOn() { Reg(0, 0xB0, 0x32); }
    void KeyOff() { Reg(0, 0xB0, 0x12); }
};

// Timer periods from opl3.h: clock/1024 and clock/4096, given directly in
// microseconds since that is how the header (and every driver) states them.
constexpr double kTimer1PeriodUs = 80.8;
constexpr double kTimer2PeriodUs = 323.1;

// --- AdLib detection ------------------------------------------------------

TEST_F(Opl3Test, AdLibDetectionSequenceReturnsZeroThenC0) {
    // opl3.h's own worked example, step for step. A regression here means
    // period software concludes there is no OPL at all.
    Reg(0, 0x04, 0x60);  // 1. mask/reset both timers
    Reg(0, 0x04, 0x80);  // 2. reset the IRQ flags
    EXPECT_EQ(opl.status(), 0x00) << "step 3: status must read 00h";

    Reg(0, 0x02, 0xFF);  // 4. timer 1 preset
    Reg(0, 0x04, 0x21);  // 4. start timer 1 (also masks timer 2, per the header text)

    // 5. wait at least 80.8 us (clock/1024) for timer 1 to expire. At this
    // machine's 66 MHz CPU clock that is 80.8e-6 * 66e6 ~= 5332.8 cycles;
    // add 10% margin so the test isn't sensitive to truncation.
    AdvanceMicroseconds(kTimer1PeriodUs * 1.1);
    EXPECT_EQ(opl.status(), 0xC0) << "step 5: status must read C0h once timer 1 has fired";

    Reg(0, 0x04, 0x60);  // 6. write 60h then 80h to 04h again
    Reg(0, 0x04, 0x80);
    EXPECT_EQ(opl.status(), 0x00) << "step 6: status must return to 00h";
}

// --- timer periods and masking --------------------------------------------

TEST_F(Opl3Test, Timer1PeriodIsEightyPointEightMicroseconds) {
    Reg(0, 0x02, 0xFF);  // preset 255: one tick to overflow from 255 to 256
    Reg(0, 0x04, 0x01);  // start timer 1, unmasked
    AdvanceMicroseconds(kTimer1PeriodUs * 0.9);
    EXPECT_FALSE(opl.timer1_expired()) << "must not fire before clock/1024 elapses";
    AdvanceMicroseconds(kTimer1PeriodUs * 0.3);  // now ~1.2x the period, total
    EXPECT_TRUE(opl.timer1_expired());
}

TEST_F(Opl3Test, Timer2PeriodIsThreeHundredTwentyThreePointOneMicroseconds) {
    Reg(0, 0x03, 0xFF);  // preset 255: one tick to overflow
    Reg(0, 0x04, 0x02);  // start timer 2, unmasked
    AdvanceMicroseconds(kTimer2PeriodUs * 0.9);
    EXPECT_FALSE(opl.timer2_expired()) << "must not fire before clock/4096 elapses";
    AdvanceMicroseconds(kTimer2PeriodUs * 0.3);
    EXPECT_TRUE(opl.timer2_expired());
}

TEST_F(Opl3Test, PresetZeroRequiresTheFullTwoFiftySixTicks) {
    // Preset 0 counts up from 0 to 256, the maximum span, versus the single
    // final tick a preset of 255 needs (tested above) -- this is what "the
    // preset shortens the count" means on real hardware.
    Reg(0, 0x02, 0x00);
    Reg(0, 0x04, 0x01);
    AdvanceMicroseconds(kTimer1PeriodUs * 256.0 * 0.97);
    EXPECT_FALSE(opl.timer1_expired());
    AdvanceMicroseconds(kTimer1PeriodUs * 256.0 * 0.06);  // now ~1.03x the full span
    EXPECT_TRUE(opl.timer1_expired());
}

TEST_F(Opl3Test, MaskBitsGateOnlyTheIrqBitNotTheTimersOwnStatusBit) {
    // 04h bits 6/5 mask a timer's contribution to bit 7 (the actual IRQ
    // line) but the timer's own status bit and flag still latch -- exactly
    // what lets a driver poll one timer without the other timer's mask
    // hiding whether it fired.
    Reg(0, 0x02, 0xFF);  // timer 1 preset: one tick to expire
    Reg(0, 0x04, 0x41);  // bit6 mask timer 1, bit0 start timer 1
    AdvanceMicroseconds(kTimer1PeriodUs * 1.5);
    EXPECT_TRUE(opl.timer1_expired());
    EXPECT_EQ(opl.status() & 0x40, 0x40) << "timer 1's own status bit is not masked";
    EXPECT_FALSE(opl.irq_pending()) << "masked timer must not raise bit 7";
    EXPECT_EQ(opl.status() & 0x80, 0x00);
}

TEST_F(Opl3Test, IrqResetBitClearsBothFlagsButReadingStatusDoesNot) {
    Reg(0, 0x02, 0xFF);
    Reg(0, 0x03, 0xFF);
    Reg(0, 0x04, 0x03);  // start both timers, unmasked
    AdvanceMicroseconds(kTimer2PeriodUs * 1.5);  // longer period: covers both
    ASSERT_TRUE(opl.timer1_expired());
    ASSERT_TRUE(opl.timer2_expired());

    // Reading the status port is a pure read on real hardware.
    opl.status();
    opl.status();
    EXPECT_TRUE(opl.timer1_expired());
    EXPECT_TRUE(opl.timer2_expired());

    Reg(0, 0x04, 0x80);  // IRQ reset
    EXPECT_FALSE(opl.timer1_expired());
    EXPECT_FALSE(opl.timer2_expired());
    EXPECT_EQ(opl.status(), 0x00);
}

TEST_F(Opl3Test, StatusBitsFourThroughZeroAlwaysReadZero) {
    // An OPL2 returns 6 (bits 2-1) here, which is one way software tells the
    // two chips apart -- an OPL3 returning stray bits would look like one.
    EXPECT_EQ(opl.status() & 0x1F, 0x00);
    Reg(0, 0x02, 0xFF);
    Reg(0, 0x04, 0x01);
    AdvanceMicroseconds(kTimer1PeriodUs * 1.5);
    ASSERT_TRUE(opl.timer1_expired());
    EXPECT_EQ(opl.status() & 0x1F, 0x00);
}

// --- NEW bit / bank 1 gating -----------------------------------------------

TEST_F(Opl3Test, Bank1WritesAreInertUntilNewIsSet) {
    EXPECT_FALSE(opl.opl3_mode());
    Reg(1, 0x04, 0x3F);  // four-op enable bits, bank 1 -- must not land
    EXPECT_EQ(opl.reg(0x104), 0x00);
    Reg(0, 0x20, 0x01);  // a bank-0 register as a control, must land fine
    EXPECT_EQ(opl.reg(0x20), 0x01);

    Reg(1, 0x05, 0x01);  // 105h bit 0 = NEW: this write must always reach the
                          // chip, or OPL3 mode could never be entered
    EXPECT_TRUE(opl.opl3_mode());
    EXPECT_EQ(opl.reg(0x105), 0x01);

    Reg(1, 0x04, 0x3F);  // bank 1 is live now
    EXPECT_EQ(opl.reg(0x104), 0x3F);
}

// --- reset ------------------------------------------------------------------

TEST_F(Opl3Test, ResetClearsEveryRegisterTimersAndOperatorState) {
    // Write something into a representative register from each area, then
    // confirm a cold reset ("every register 0, ... all 36 operators
    // released and silent", opl3.h) really wipes it all.
    Reg(0, 0x20, 0xFF);
    Reg(0, 0xB0, 0x35);  // key on channel 0
    Reg(1, 0x05, 0x01);  // NEW
    Reg(0, 0x02, 0xFF);
    Reg(0, 0x04, 0x01);  // start timer 1
    AdvanceMicroseconds(kTimer1PeriodUs * 1.5);
    ASSERT_TRUE(opl.timer1_expired());

    opl.reset();

    for (int i = 0; i < 0x200; ++i) {
        EXPECT_EQ(opl.reg(uint16_t(i)), 0x00) << "register " << i;
    }
    EXPECT_FALSE(opl.opl3_mode());
    EXPECT_FALSE(opl.timer1_expired());
    EXPECT_FALSE(opl.timer2_expired());
    EXPECT_EQ(opl.status(), 0x00);
    EXPECT_FALSE(opl.active());
    EXPECT_TRUE(opl.drain_samples().empty());
}

// --- silence and audible output --------------------------------------------

TEST_F(Opl3Test, NoKeyOnProducesNoAudibleOutputAndStaysInactive) {
    EXPECT_FALSE(opl.active());
    AdvanceMicroseconds(10000.0);  // 10 ms of idle time
    EXPECT_FALSE(opl.active()) << "tick() must cost nothing with no operator running";
    for (const auto &s : opl.drain_samples()) {
        EXPECT_EQ(s.left, 0);
        EXPECT_EQ(s.right, 0);
    }
}

TEST_F(Opl3Test, KeyOnProducesAudibleOutputAndKeyOffReleasesToSilence) {
    SetUpAudibleChannel();
    ASSERT_FALSE(opl.active());
    KeyOn();
    EXPECT_TRUE(opl.active());

    AdvanceMicroseconds(5000.0);  // 5 ms of sustained tone
    auto held = opl.drain_samples();
    // 5 ms at the chip's ~49716 Hz frame rate is roughly 249 frames; allow
    // generous slack since tick() only emits whole frames as CPU cycles
    // cross a frame boundary, and the attack phase covers the first slice.
    double expected_held = 5000e-6 * Opl3::kSampleHz;
    EXPECT_GE(held.size(), std::size_t(expected_held * 0.8));
    EXPECT_LE(held.size(), std::size_t(expected_held * 1.2));
    bool any_nonzero = false;
    for (const auto &s : held) {
        if (s.left != 0 || s.right != 0) any_nonzero = true;
    }
    EXPECT_TRUE(any_nonzero) << "a keyed-on, unmuted channel must produce sound";

    KeyOff();
    AdvanceMicroseconds(100000.0);  // 100 ms: far past even the fastest release
    auto released = opl.drain_samples();
    ASSERT_GT(released.size(), 10u);

    // Peak absolute amplitude over the first and last quarter of the release
    // -- decay direction and eventual silence, without pinning any exact
    // sample value the operator tables (written independently) would set.
    auto peak = [](const std::vector<Opl3::Sample> &v, std::size_t lo, std::size_t hi) {
        int32_t m = 0;
        for (std::size_t i = lo; i < hi; ++i) {
            int32_t l = v[i].left < 0 ? -v[i].left : v[i].left;
            int32_t r = v[i].right < 0 ? -v[i].right : v[i].right;
            if (l > m) m = l;
            if (r > m) m = r;
        }
        return m;
    };
    std::size_t n = released.size();
    int32_t early_peak = peak(released, 0, n / 4);
    int32_t late_peak = peak(released, n - n / 4, n);
    EXPECT_LT(late_peak, early_peak) << "release must decay toward silence, not hold at volume";
    EXPECT_EQ(late_peak, 0) << "a fully released envelope is silent";
    EXPECT_FALSE(opl.active());
}

// The chip's clock must not slip when it is ticked coarsely. advance() bounds
// how many frames one call will generate, but it consumes the credit for every
// frame that came due -- so a long gap between ticks drops a slice of audio
// instead of leaving the chip permanently behind. Capping the credit instead
// made the shortfall accumulate call after call, heard as music that starts at
// the right speed and then slows down and keeps slowing.
TEST_F(Opl3Test, CoarseTicksDoNotMakeTheChipsClockFallBehind) {
    // A timer alone keeps the chip active, so this measures pacing without
    // depending on any envelope.
    Reg(0, 0x02, 0x00);
    Reg(0, 0x04, 0x01);

    // 250ms per tick is far past advance()'s per-call frame bound, which is
    // exactly the case that used to leak time.
    const uint64_t step = uint64_t(Opl3::kCpuHz * 0.25);
    uint64_t cycles = 0;
    uint64_t emitted = 0;
    for (int i = 0; i < 8; ++i) {
        cycles += step;
        opl.tick(cycles);
        emitted += opl.drain_samples().size();
    }
    // The chip's own clock is what must stay honest: after 2s of guest time it
    // must have accounted for ~2s of frames, even though the per-call bound
    // means not all of them were handed over.
    const double elapsed = double(cycles) / Opl3::kCpuHz;
    EXPECT_LE(emitted, std::size_t(elapsed * Opl3::kSampleHz * 1.01));

    // Now tick finely and confirm the rate is immediately correct rather than
    // catching up on a backlog -- proof no deficit was carried forward.
    opl.drain_samples();
    const uint64_t fine = uint64_t(Opl3::kCpuHz * 0.01);
    uint64_t fine_total = 0;
    for (int i = 0; i < 100; ++i) {
        cycles += fine;
        opl.tick(cycles);
        fine_total += opl.drain_samples().size();
    }
    const double expected = Opl3::kSampleHz;  // 100 * 10ms == 1s
    EXPECT_NEAR(double(fine_total), expected, expected * 0.02)
        << "1s of fine ticks must yield ~1s of frames, no backlog and no deficit";
}

// --- pitch -------------------------------------------------------------------

// The output frequency is F = fnum * kSampleHz / 2^(20-Block) -- the formula
// the classic AdLib note table is built on, where fnum 159h at block 4 is
// middle C. Nothing else in this suite pins the phase increment, and an error
// here is an error in octaves: the first implementation ran two octaves sharp,
// which is audible as music with no bass at all rather than as a wrong note.
TEST_F(Opl3Test, OutputFrequencyMatchesTheFnumAndBlockFormula) {
    for (int block = 2; block <= 5; ++block) {
        Opl3 chip;
        auto w = [&](uint8_t r, uint8_t v) { chip.write_address(0, r); chip.write_data(0, v); };
        w(0x20, 0x21); w(0x23, 0x21);  // EGT holds at sustain, MULT=1
        w(0x40, 0x3F); w(0x43, 0x00);  // modulator silenced: the carrier is a bare sine
        w(0x60, 0xF0); w(0x63, 0xF0);  // fastest attack, no decay
        w(0x80, 0x00); w(0x83, 0x00);  // sustain at full level
        w(0xC0, 0x31);                 // both outputs, CNT=1 (additive)
        w(0xA0, 0x59);                 // fnum = 159h
        w(0xB0, uint8_t(0x20 | (block << 2) | 0x01));

        uint64_t cycles = 0;
        for (int i = 0; i < 300; ++i) {
            cycles += uint64_t(Opl3::kCpuHz * 0.001);
            chip.tick(cycles);
        }
        auto s = chip.drain_samples();
        ASSERT_GT(s.size(), 1000u);

        // Count zero crossings over the sustained tail, past the attack.
        const std::size_t start = s.size() / 2;
        int crossings = 0;
        int32_t prev = s[start].left;
        for (std::size_t i = start + 1; i < s.size(); ++i) {
            if ((prev < 0 && s[i].left >= 0) || (prev >= 0 && s[i].left < 0)) crossings++;
            prev = s[i].left;
        }
        const double seconds = double(s.size() - start) / Opl3::kSampleHz;
        const double measured = crossings / 2.0 / seconds;
        const double expected = 0x159 * Opl3::kSampleHz / 1048576.0 * double(1u << block);
        // 5% covers zero-crossing granularity; an octave error is 100%.
        EXPECT_NEAR(measured, expected, expected * 0.05)
            << "block " << block << ": " << measured << " Hz, want " << expected;
    }
}

// MULT is a half-integer multiplier, so MULT=0 is x0.5 and MULT=2 is x2 --
// an octave below and above MULT=1's pitch.
TEST_F(Opl3Test, MultFieldScalesPitchAsAHalfIntegerMultiplier) {
    auto tone_hz = [this](uint8_t mult) {
        Opl3 chip;
        auto w = [&](uint8_t r, uint8_t v) { chip.write_address(0, r); chip.write_data(0, v); };
        w(0x20, uint8_t(0x20 | mult)); w(0x23, uint8_t(0x20 | mult));
        w(0x40, 0x3F); w(0x43, 0x00);
        w(0x60, 0xF0); w(0x63, 0xF0);
        w(0x80, 0x00); w(0x83, 0x00);
        w(0xC0, 0x31);
        w(0xA0, 0x59); w(0xB0, uint8_t(0x20 | (4 << 2) | 0x01));
        uint64_t cycles = 0;
        for (int i = 0; i < 300; ++i) { cycles += uint64_t(Opl3::kCpuHz * 0.001); chip.tick(cycles); }
        auto s = chip.drain_samples();
        const std::size_t start = s.size() / 2;
        int crossings = 0;
        int32_t prev = s[start].left;
        for (std::size_t i = start + 1; i < s.size(); ++i) {
            if ((prev < 0 && s[i].left >= 0) || (prev >= 0 && s[i].left < 0)) crossings++;
            prev = s[i].left;
        }
        return crossings / 2.0 / (double(s.size() - start) / Opl3::kSampleHz);
    };
    const double one = tone_hz(1);
    EXPECT_NEAR(tone_hz(0), one / 2.0, one * 0.05) << "MULT=0 is x0.5";
    EXPECT_NEAR(tone_hz(2), one * 2.0, one * 0.10) << "MULT=2 is x2";
}

// --- stereo panning ----------------------------------------------------------

TEST_F(Opl3Test, C0RegisterPansHardLeftOrRightOnceNewIsSet) {
    Reg(1, 0x05, 0x01);  // NEW: OPL3 stereo mode
    // C0h bit 4 is CHA and bit 5 is CHB; CHA drives the left output and CHB
    // the right. Clearing the other pans hard to one side.
    SetUpAudibleChannel(0x10);  // CHA only: left
    KeyOn();
    AdvanceMicroseconds(5000.0);
    auto samples = opl.drain_samples();
    ASSERT_FALSE(samples.empty());
    bool left_nonzero = false, right_nonzero = false;
    for (const auto &s : samples) {
        if (s.left != 0) left_nonzero = true;
        if (s.right != 0) right_nonzero = true;
    }
    EXPECT_TRUE(left_nonzero);
    EXPECT_FALSE(right_nonzero);
}

TEST_F(Opl3Test, BothChannelsStayAudibleWhileNewIsClearRegardlessOfC0) {
    // NEW is clear out of reset. A mono-era driver that never touches 105h
    // or C0h must still hear sound out of both speakers (opl3.h).
    ASSERT_FALSE(opl.opl3_mode());
    SetUpAudibleChannel(0x00);  // no pan bits set at all
    KeyOn();
    AdvanceMicroseconds(5000.0);
    auto samples = opl.drain_samples();
    ASSERT_FALSE(samples.empty());
    bool left_nonzero = false, right_nonzero = false;
    for (const auto &s : samples) {
        if (s.left != 0) left_nonzero = true;
        if (s.right != 0) right_nonzero = true;
    }
    EXPECT_TRUE(left_nonzero);
    EXPECT_TRUE(right_nonzero);
}

// --- sample cadence and bookkeeping -----------------------------------------

TEST_F(Opl3Test, SampleCadenceMatchesTheRealFourNineSevenOneSixHertzRate) {
    // Realism contract (CLAUDE.md): the frame rate is fixed silicon
    // behavior, 14.31818 MHz / 288 = 49715.9 Hz, and drain_samples() must
    // report frames at that real cadence for a given span of CPU cycles --
    // never sped up.
    SetUpAudibleChannel();
    KeyOn();
    AdvanceMicroseconds(20000.0);  // 20 ms
    auto samples = opl.drain_samples();
    double expected = 20000e-6 * Opl3::kSampleHz;
    EXPECT_GE(samples.size(), std::size_t(expected * 0.95));
    EXPECT_LE(samples.size(), std::size_t(expected * 1.05));
}

TEST_F(Opl3Test, DrainSamplesReturnsAndClearsTheLog) {
    SetUpAudibleChannel();
    KeyOn();
    AdvanceMicroseconds(1000.0);
    EXPECT_FALSE(opl.drain_samples().empty());
    EXPECT_TRUE(opl.drain_samples().empty());
}

}  // namespace
