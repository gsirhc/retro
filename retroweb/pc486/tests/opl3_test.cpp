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

#include <algorithm>
#include <cstdint>
#include <cstdlib>
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
    // Operator slot offsets for channels 0-8 within one bank: the OPL's
    // operator numbering is not contiguous across channels.
    static uint8_t OpOffset(int ch, bool carrier) {
        static const uint8_t kBase[9] = {0x00, 0x01, 0x02, 0x08, 0x09, 0x0A, 0x10, 0x11, 0x12};
        return uint8_t(kBase[ch] + (carrier ? 3 : 0));
    }

    // Programs one 2-op FM voice on an arbitrary bank/channel at the given
    // total levels and keys it on, so a test can build real polyphony instead
    // of the single channel 0 SetUpAudibleChannel() covers.
    void SetUpVoiceAndKeyOn(int bank, int ch, uint8_t mod_tl, uint8_t car_tl) {
        uint8_t m = OpOffset(ch, false), c = OpOffset(ch, true);
        Reg(bank, uint8_t(0x20 + m), 0x21);
        Reg(bank, uint8_t(0x20 + c), 0x21);
        Reg(bank, uint8_t(0x40 + m), mod_tl);
        Reg(bank, uint8_t(0x40 + c), car_tl);
        Reg(bank, uint8_t(0x60 + m), 0xF0);
        Reg(bank, uint8_t(0x60 + c), 0xF0);
        Reg(bank, uint8_t(0x80 + m), 0x0F);
        Reg(bank, uint8_t(0x80 + c), 0x0F);
        Reg(bank, uint8_t(0xC0 + ch), 0x30);
        Reg(bank, uint8_t(0xA0 + ch), 0xAE);
        Reg(bank, uint8_t(0xB0 + ch), 0x32);
    }

    // Fraction of drained samples sitting against either clamp bound.
    static double ClippedFraction(const std::vector<Opl3::Sample> &samples) {
        std::size_t clipped = 0;
        for (const auto &s : samples) {
            if (s.left >= 32767 || s.left <= -32768 || s.right >= 32767 || s.right <= -32768) ++clipped;
        }
        return samples.empty() ? 0.0 : double(clipped) / double(samples.size());
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

// --- output headroom ------------------------------------------------------

// The output stage's master gain has no hardware citation (see opl3.cpp), so
// what pins it is the clamp: real FM music runs many voices at once, and a
// gain that clips them is audible as distortion. Nothing used to sum more
// than one channel, which is exactly how a gain that clipped at three voices
// shipped. 18 moderately-attenuated voices is full OPL3 polyphony voiced the
// way period music actually is.
TEST_F(Opl3Test, FullPolyphonyAtModerateLevelsDoesNotClip) {
    Reg(1, 0x05, 0x01);  // NEW: OPL3 mode, so bank 1's nine channels sound
    for (int ch = 0; ch < 9; ++ch) {
        SetUpVoiceAndKeyOn(0, ch, 10, 18);
        SetUpVoiceAndKeyOn(1, ch, 10, 18);
    }
    AdvanceMicroseconds(10000.0);  // past the attack
    opl.drain_samples();
    AdvanceMicroseconds(60000.0);  // steady state
    auto samples = opl.drain_samples();
    ASSERT_FALSE(samples.empty());
    EXPECT_DOUBLE_EQ(0.0, ClippedFraction(samples));
}

// A single unattenuated voice must leave room for the rest of them: if one
// channel alone eats a large share of full scale, any real arrangement
// clips. The bound is what opl3.cpp's gain comment claims (~18%).
TEST_F(Opl3Test, OneFullVolumeVoiceLeavesHeadroomForEighteen) {
    SetUpAudibleChannel();
    KeyOn();
    AdvanceMicroseconds(10000.0);
    opl.drain_samples();
    AdvanceMicroseconds(20000.0);
    auto samples = opl.drain_samples();
    ASSERT_FALSE(samples.empty());
    int peak = 0;
    for (const auto &s : samples) {
        peak = std::max(peak, std::abs(int(s.left)));
        peak = std::max(peak, std::abs(int(s.right)));
    }
    EXPECT_GT(peak, 3000);   // still audibly present, not scaled into nothing
    EXPECT_LT(peak, 8200);   // under 1/4 of full scale, so polyphony fits
}

// --- modulation index --------------------------------------------------------

// kModulationIndexRadians (opl3.cpp) was recalibrated to 8*pi, exactly double
// kFeedbackRadians[7] (4*pi) -- both pinned by the same decap fact: a full-
// scale operator output is 4084 units against a phase adder scaled at 1024
// units/cycle, and cross-operator modulation injects that output unshifted
// while self-feedback shifts it by (9-FB). The raw phase-offset arithmetic
// isn't reachable from a test, so this checks the audible consequence against
// a specific numeric prediction (not just "more modulation than before"),
// which is what actually catches a regression back to the old, wrong value:
// attenuate the modulator by a known amount (TL steps are 0.75dB each) so the
// realized index is a known fraction of kModulationIndexRadians, then verify
// the carrier's instantaneous frequency shift near key-on (where the
// modulator's own phase derivative, and so the carrier's frequency deviation,
// is at its peak -- classic FM: x(t)=sin(wc t + beta*sin(wm t)) has
// instantaneous frequency wc + beta*wm*cos(wm t), maximal at t=0) matches
// beta*fm for beta computed from the CURRENT kModulationIndexRadians. If the
// constant regressed to pi, the predicted shift here would be 8x too large
// and this test would fail.
TEST_F(Opl3Test, FullScaleModulationMatchesAnEightPiModulationIndex) {
    // Interpolated zero-crossing frequency over [lo,hi): using only the
    // first and last crossing's position averages out individual-sample
    // jitter, needed since the measurement window is short (a handful of
    // carrier cycles, to stay close to the keyon-instant peak deviation).
    auto measured_hz = [](const std::vector<int32_t> &v, std::size_t lo, std::size_t hi) -> double {
        std::vector<double> crossings;
        for (std::size_t i = lo + 1; i < hi; ++i) {
            if ((v[i - 1] < 0) != (v[i] < 0)) {
                double frac = double(-v[i - 1]) / double(v[i] - v[i - 1]);
                crossings.push_back(double(i - 1) + frac);
            }
        }
        if (crossings.size() < 3) return 0.0;
        double periods = double(crossings.size() - 1) / 2.0;
        double span_frames = crossings.back() - crossings.front();
        return periods / (span_frames / Opl3::kSampleHz);
    };

    Opl3 chip;
    auto w = [&](uint8_t r, uint8_t v) { chip.write_address(0, r); chip.write_data(0, v); };
    w(0x20, uint8_t(0x20 | 1));   // modulator: EGT hold, MULT=1 (x1)
    w(0x23, uint8_t(0x20 | 15));  // carrier: EGT hold, MULT=15 (x15) -- a 15:1 carrier:modulator ratio
    const uint8_t kModulatorTl = 24;  // 24 * 0.75dB = 18dB atten, amplitude ~10^(-18/20)
    w(0x40, kModulatorTl);
    w(0x43, 0x00);                 // carrier TL: loudest
    w(0x60, 0xF0); w(0x63, 0xF0);  // both: fastest attack, DR=0
    w(0x80, 0x00); w(0x83, 0x00);  // both: SL=0 (loudest sustain), fastest release
    w(0xC0, 0x30);                 // CNT=0 (FM), FB=0 (no self-feedback to confound the measurement)
    w(0xA0, 0x59);                 // fnum = 0x159
    w(0xB0, uint8_t(0x20 | (2 << 2) | 0x01));  // KON, block=2, fnum hi=1

    std::vector<int32_t> left;
    uint64_t cycles = 0;
    const uint64_t step = uint64_t(Opl3::kCpuHz * 0.0002);  // 0.2 ms per step
    for (int i = 0; i < 15; ++i) {
        cycles += step;
        chip.tick(cycles);
        for (const auto &s : chip.drain_samples()) left.push_back(s.left);
    }
    ASSERT_GT(left.size(), 30u);

    const double kPi2 = 3.14159265358979323846;
    const double modulator_hz = 0x159 * Opl3::kSampleHz / 1048576.0 * double(1u << 2) * 1.0;
    const double carrier_hz = 0x159 * Opl3::kSampleHz / 1048576.0 * double(1u << 2) * 15.0;
    const double modulator_amplitude = std::pow(10.0, -(double(kModulatorTl) * 0.75) / 20.0);
    const double predicted_index = 8.0 * kPi2 * modulator_amplitude;
    // The window isn't infinitesimal, so the measured (zero-crossing-derived,
    // effectively averaged) deviation is attenuated from the keyon-instant
    // peak by the classic sinc-shaped FM average: sin(wm*T)/(wm*T) over a
    // window of length T starting at keyon (t=0), wm = 2*pi*modulator_hz.
    const double window_seconds = double(left.size()) / Opl3::kSampleHz;
    const double wm_t = 2.0 * kPi2 * modulator_hz * window_seconds;
    const double correction = wm_t > 1e-9 ? std::sin(wm_t) / wm_t : 1.0;
    const double predicted_deviation = predicted_index * modulator_hz * correction;

    const double measured = measured_hz(left, 3, left.size() - 3);
    const double deviation = measured - carrier_hz;
    EXPECT_NEAR(deviation, predicted_deviation, predicted_deviation * 0.5)
        << "measured " << measured << " Hz (carrier " << carrier_hz << " Hz, modulator " << modulator_hz
        << " Hz), deviation " << deviation << " Hz, predicted " << predicted_deviation << " Hz (index "
        << predicted_index << ")";
}

// --- four-operator mode ------------------------------------------------------

// While a channel pair is in 4-op mode, the secondary channel's A0h/B0h are
// latched but not live: the primary's fnum/block drive all four operators
// (opl3.cpp's generate_frame comment cites Nuked-OPL3's OPL3_ChannelSync4Op as
// the decap cross-check). Isolate operators 3/4's own contribution by diffing
// against an identical plain 2-op channel that never sees them: before the
// fix, ops 3/4 ran from channel 3's untouched (zero) fnum/block, so in this
// connection pattern they sat at a frozen phase=0 receiving no modulation --
// an exact, provable zero contribution, not merely a quiet one.
TEST_F(Opl3Test, FourOpSecondaryOperatorsRunFromThePrimarysFnumAndBlock) {
    Opl3 four_op;
    auto w4 = [&](int bank, uint8_t r, uint8_t v) { four_op.write_address(bank, r); four_op.write_data(bank, v); };
    w4(1, 0x05, 0x01);  // NEW
    w4(1, 0x04, 0x01);  // pair channels 0+3 into one four-operator voice
    for (uint8_t base : {uint8_t(0x00), uint8_t(0x03), uint8_t(0x08), uint8_t(0x0B)}) {
        w4(0, uint8_t(0x20 + base), 0x21);  // EGT hold, MULT=1
        w4(0, uint8_t(0x40 + base), 0x00);  // TL: loudest
        w4(0, uint8_t(0x60 + base), 0xF0);  // fastest attack, DR=0
        w4(0, uint8_t(0x80 + base), 0x0F);  // SL=0, fastest release
    }
    w4(0, 0xC0, 0x30);  // channel 0: CNT=0 (mod0 -> car0)
    w4(0, 0xC3, 0x01);  // channel 3: CNT=1 (op3 unmodulated, op3 -> op4)
    // Channel 0 (the primary) is the only channel given a pitch; channel 3's
    // A0h/B0h are deliberately left at their post-reset zero.
    w4(0, 0xA0, 0x59);
    w4(0, 0xB0, uint8_t(0x20 | (4 << 2) | 0x01));  // KON, block=4, fnum hi=1 (0x159)

    Opl3 two_op;
    auto w2 = [&](uint8_t r, uint8_t v) { two_op.write_address(0, r); two_op.write_data(0, v); };
    w2(0x20, 0x21); w2(0x23, 0x21);
    w2(0x40, 0x00); w2(0x43, 0x00);
    w2(0x60, 0xF0); w2(0x63, 0xF0);
    w2(0x80, 0x0F); w2(0x83, 0x0F);
    w2(0xC0, 0x30);
    w2(0xA0, 0x59);
    w2(0xB0, uint8_t(0x20 | (4 << 2) | 0x01));

    uint64_t cycles = 0;
    std::vector<int32_t> diff;
    for (int i = 0; i < 300; ++i) {
        cycles += uint64_t(Opl3::kCpuHz * 0.001);
        four_op.tick(cycles);
        two_op.tick(cycles);
        auto a = four_op.drain_samples();
        auto b = two_op.drain_samples();
        ASSERT_EQ(a.size(), b.size());
        for (std::size_t j = 0; j < a.size(); ++j) diff.push_back(int32_t(a[j].left) - int32_t(b[j].left));
    }
    ASSERT_GT(diff.size(), 1000u);

    // op3/op4's own output is heavily phase-modulated (kModulationIndexRadians
    // is large), so its zero-crossing rate doesn't simply track the primary's
    // frequency -- but it must still be EXACTLY periodic at the primary's
    // cycle length (op3's phase increments at that same rate, and op4 is a
    // deterministic function of op3), so check period-to-period repetition
    // directly instead of counting crossings.
    const std::size_t period_frames = std::size_t(std::lround(1048576.0 / double(0x159 << 4)));
    const std::size_t start = diff.size() / 2;
    double sum_abs = 0.0, sum_abs_diff = 0.0;
    std::size_t count = 0;
    for (std::size_t i = start; i + period_frames < diff.size(); ++i) {
        sum_abs += std::abs(diff[i]);
        sum_abs_diff += std::abs(diff[i] - diff[i + period_frames]);
        ++count;
    }
    ASSERT_GT(count, 100u);
    const double avg_abs = sum_abs / double(count);
    const double avg_abs_diff = sum_abs_diff / double(count);
    EXPECT_GT(avg_abs, 50.0)
        << "operators 3/4 must produce their own sound once the pair is 4-op, "
           "even though channel 3 was never programmed with a pitch";
    EXPECT_LT(avg_abs_diff, avg_abs * 0.3)
        << "operators 3/4's output must repeat at the primary channel's period ("
        << period_frames << " frames): avg |diff|=" << avg_abs << ", avg period-to-period change=" << avg_abs_diff;
}

// --- note-select / key-scale number -------------------------------------------

// NTS=0 selects F-number bit 9 for key scaling, NTS=1 selects bit 8 (Yamaha
// YMF715x Register Description Document). Observed through KSR: with KSR=1
// the key-scale number feeds the rate formula directly, so flipping which
// fnum bit NTS reads measurably changes a fixed decay rate's speed.
TEST_F(Opl3Test, NoteSelectPicksFnumBitNineWhenClearAndBitEightWhenSet) {
    auto decayed_peak = [](bool nts) -> int32_t {
        Opl3 chip;
        auto w = [&](uint8_t r, uint8_t v) { chip.write_address(0, r); chip.write_data(0, v); };
        w(0x08, nts ? 0x40 : 0x00);
        w(0x20, 0x01);                  // modulator: MULT=1 (silenced below)
        w(0x23, 0x31);                  // carrier: EGT hold, KSR=1, MULT=1
        w(0x40, 0x3F); w(0x43, 0x00);   // modulator TL: silent, carrier TL: loudest
        w(0x60, 0xF0); w(0x63, 0xF4);   // modulator fastest attack; carrier AR=15, DR=4
        w(0x80, 0x00); w(0x83, 0xF0);   // carrier SL=15 (decays toward full attenuation, held by EGT), RR=0
        // fnum = 0x200: bit 9 set, bit 8 clear -- NTS picks a different bit
        // of this same value depending on its setting.
        w(0xA0, 0x00);
        w(0xB0, 0x22);  // KON, block=0, fnum hi bits = 10b
        uint64_t cycles = 0;
        const uint64_t step = uint64_t(Opl3::kCpuHz * 0.001);  // 1 ms
        for (int ms = 0; ms < 2000; ++ms) { cycles += step; chip.tick(cycles); chip.drain_samples(); }
        int32_t peak = 0;
        for (int ms = 0; ms < 5; ++ms) {
            cycles += step;
            chip.tick(cycles);
            for (const auto &s : chip.drain_samples()) peak = std::max(peak, std::abs(int(s.left)));
        }
        return peak;
    };
    const int32_t peak_nts0 = decayed_peak(false);  // selects fnum bit 9 (=1) -> ksn=1, faster decay
    const int32_t peak_nts1 = decayed_peak(true);   // selects fnum bit 8 (=0) -> ksn=0, slower decay
    EXPECT_LT(peak_nts0, peak_nts1 * 0.9)
        << "NTS must pick different fnum bits, giving a different key-scaled decay rate: "
        << "peak after 2s, NTS=0 " << peak_nts0 << ", NTS=1 " << peak_nts1;
}

// --- rhythm output doubling ---------------------------------------------------

// Each rhythm voice is wired into two of the channel's four output buses on
// real silicon, so it sums at twice a melodic channel's amplitude -- decap-
// corroborated (Nuked-OPL3 wires out[0]/out[1] and out[2]/out[3] to the same
// slot in rhythm mode), not stated in a Yamaha document. The bass drum is the
// one rhythm voice that is an ordinary 2-op FM channel, which makes it the
// clean comparison point against the same patch played as a melodic voice.
TEST_F(Opl3Test, RhythmVoiceSumsAtTwiceAMelodicChannelsAmplitude) {
    auto peak_of = [](bool rhythm) -> int32_t {
        Opl3 chip;
        auto w = [&](uint8_t r, uint8_t v) { chip.write_address(0, r); chip.write_data(0, v); };
        uint8_t m = OpOffset(6, false), c = OpOffset(6, true);
        w(uint8_t(0x20 + m), 0x21); w(uint8_t(0x20 + c), 0x21);  // EGT hold, MULT=1
        w(uint8_t(0x40 + m), 0x00); w(uint8_t(0x40 + c), 0x00);  // TL: loudest
        w(uint8_t(0x60 + m), 0xF0); w(uint8_t(0x60 + c), 0xF0);  // fastest attack, DR=0
        w(uint8_t(0x80 + m), 0x0F); w(uint8_t(0x80 + c), 0x0F);  // SL=0, fastest release
        w(0xC6, 0x30);  // CNT=0 (FM), both pan bits on
        w(0xA6, 0xAE);
        w(0xB6, 0x12);  // block=4, fnum hi=2 -- no KON here; see below
        if (rhythm) w(0xBD, 0x30);  // rhythm enable + bass-drum key (bypasses channel 6's own KON)
        else w(0xB6, 0x32);         // channel 6's own KON, same block/fnum
        uint64_t cycles = 0;
        for (int i = 0; i < 20; ++i) { cycles += uint64_t(Opl3::kCpuHz * 0.001); chip.tick(cycles); }
        chip.drain_samples();
        for (int i = 0; i < 20; ++i) { cycles += uint64_t(Opl3::kCpuHz * 0.001); chip.tick(cycles); }
        int32_t peak = 0;
        for (const auto &s : chip.drain_samples()) peak = std::max(peak, std::abs(int(s.left)));
        return peak;
    };
    const int32_t melodic_peak = peak_of(false);
    const int32_t rhythm_peak = peak_of(true);
    ASSERT_GT(melodic_peak, 0);
    EXPECT_NEAR(double(rhythm_peak), double(melodic_peak) * 2.0, double(melodic_peak) * 0.1)
        << "a rhythm voice must sum at 2x a melodic channel's amplitude: melodic=" << melodic_peak
        << " rhythm=" << rhythm_peak;
}

// --- waveform 7 (exponential-decay sawtooth) ----------------------------------

// The negative half's phase is mirrored and its ramp covers the full ~96 dB
// range (<<3, not <<2): the positive half starts near full scale and decays
// toward silence, and the negative half must start near silence (a
// continuous, mirrored continuation of the positive half, not a jump to full
// amplitude) and rise to full amplitude only at the very end of the cycle.
TEST_F(Opl3Test, Waveform7MirrorsItsNegativeHalfAndRampsAcrossTheFullRange) {
    Opl3 chip;
    auto w = [&](uint8_t r, uint8_t v) { chip.write_address(0, r); chip.write_data(0, v); };
    chip.write_address(1, 0x05); chip.write_data(1, 0x01);  // NEW: waveforms 4-7 need OPL3 mode
    w(0x20, 0x01); w(0x40, 0x3F);  // modulator: MULT=1, silenced (AR=0 too: never attacks)
    w(0x23, 0x21);                 // carrier: EGT hold, MULT=1
    w(0x43, 0x00);                 // carrier TL: loudest
    w(0x63, 0xF0);                 // carrier AR=15 (fastest attack), DR=0
    w(0x83, 0x00);                 // carrier SL=0 (loudest sustain), RR=0
    w(0xE3, 0x07);                 // carrier waveform 7
    w(0xA0, 0x40);                 // fnum = 64
    w(0xB0, 0x20);                 // KON, block=0, fnum hi=0

    // Phase accumulates by exactly fnum=64 units/frame (block=0, MULT=1), and
    // the first generated frame already reflects one increment (key-on's
    // phase reset and that frame's own advance both happen before it is
    // sampled), so sample k uses phase=(k+1)*64. A full 2^20-unit cycle is
    // exactly 16384 frames: frame 100 sits early in the positive half (near
    // peak), frame 8192 is the very first frame of the negative half, and
    // frame 16382 is the last frame before the cycle wraps (frame 16383 has
    // already wrapped back to phase 0).
    std::vector<Opl3::Sample> all;
    uint64_t cycles = 0;
    const double cycles_per_frame = Opl3::kCpuHz / Opl3::kSampleHz;
    for (int i = 0; i < 6; ++i) {
        cycles += uint64_t(std::llround(4000.0 * cycles_per_frame));
        chip.tick(cycles);
        auto batch = chip.drain_samples();
        all.insert(all.end(), batch.begin(), batch.end());
    }
    ASSERT_GT(all.size(), 16400u);

    int32_t early = all[100].left;
    int32_t half_start = all[8192].left;
    int32_t cycle_end = all[16382].left;
    EXPECT_GT(early, 3000) << "positive half starts near full scale";
    EXPECT_LT(std::abs(int(half_start)), 600)
        << "the negative half must start near silence (a mirrored, continuous ramp), not a jump to full amplitude: "
        << half_start;
    EXPECT_LT(cycle_end, -3000)
        << "the negative half must rise to full amplitude by the end of the cycle, not decay away from it: "
        << cycle_end;
}

// --- vibrato -------------------------------------------------------------------

// Real hardware steps a vibrato position 0-7 once every 1024 output frames,
// applying an F-number delta (not a continuous cents ratio) -- see opl3.cpp's
// advance_env_phase. vib_pos_/vib_frame_ start at 0 at construction/reset, so
// a fresh chip's step-2 window (frames 2048-3071 after key-on at frame 0) is
// reachable deterministically. Step 2 (range=base, before the DVB halving) is
// the pattern's largest deviation for a given fnum.
TEST_F(Opl3Test, VibratoAppliesTheQuantisedEightStepFnumDeltaAndDvbDoublesIt) {
    // Interpolated zero-crossing frequency over [lo,hi): using only the
    // first and last crossing's position averages out individual-sample
    // jitter, which matters here since the expected shift (under 1% of the
    // carrier's frequency) is far smaller than one sample period.
    auto measured_hz = [](const std::vector<int32_t> &v, std::size_t lo, std::size_t hi) -> double {
        std::vector<double> crossings;
        for (std::size_t i = lo + 1; i < hi; ++i) {
            if ((v[i - 1] < 0) != (v[i] < 0)) {
                double frac = double(-v[i - 1]) / double(v[i] - v[i - 1]);
                crossings.push_back(double(i - 1) + frac);
            }
        }
        if (crossings.size() < 3) return 0.0;
        double periods = double(crossings.size() - 1) / 2.0;
        double span_frames = crossings.back() - crossings.front();
        return periods / (span_frames / Opl3::kSampleHz);
    };
    auto measure = [&](bool vib, bool dvb) -> double {
        Opl3 chip;
        auto w = [&](uint8_t r, uint8_t v) { chip.write_address(0, r); chip.write_data(0, v); };
        w(0xBD, dvb ? 0x40 : 0x00);
        w(0x20, 0x01);  // modulator: MULT=1 (AR=0 default: never attacks, stays silent)
        w(0x23, uint8_t(0x20 | (vib ? 0x40 : 0x00) | 0x01));  // carrier: EGT hold, VIB, MULT=1
        w(0x43, 0x00);                                        // carrier TL: loudest
        w(0x63, 0xF0); w(0x83, 0x00);                          // carrier: fastest attack, SL=0 hold
        w(0xA0, 0x00);                                         // fnum = 0x200 (512): (fnum>>7)&7 == 4
        w(0xB0, uint8_t(0x20 | (7 << 2) | 0x02));              // KON, block=7, fnum hi bits = 10b

        std::vector<int32_t> left;
        uint64_t cycles = 0;
        const double cycles_per_frame = Opl3::kCpuHz / Opl3::kSampleHz;
        for (int i = 0; i < 4; ++i) {  // 4000 frames > the 3072 needed to clear step 2
            cycles += uint64_t(std::llround(1000.0 * cycles_per_frame));
            chip.tick(cycles);
            for (const auto &s : chip.drain_samples()) left.push_back(s.left);
        }
        // Step 2 spans frames 2048-3071 since key-on (frame 0); stay clear of its edges.
        return measured_hz(left, 2200, 3000);
    };

    const double base_hz = 512.0 * Opl3::kSampleHz / 1048576.0 * double(1u << 7);
    const double vib_off_hz = measure(/*vib=*/false, /*dvb=*/false);
    const double vib_on_hz = measure(/*vib=*/true, /*dvb=*/false);
    const double vib_on_dvb_hz = measure(/*vib=*/true, /*dvb=*/true);

    EXPECT_NEAR(vib_off_hz, base_hz, base_hz * 0.01) << "VIB clear must leave pitch unchanged";
    EXPECT_GT(vib_on_hz, base_hz * 1.002)
        << "step 2 without DVB (range=base>>1=+2) must raise pitch measurably: " << vib_on_hz << " vs base " << base_hz;
    const double shallow_delta = vib_on_hz - base_hz;
    const double deep_delta = vib_on_dvb_hz - base_hz;
    EXPECT_NEAR(deep_delta, shallow_delta * 2.0, shallow_delta * 0.3)
        << "DVB must double the vibrato depth: shallow delta " << shallow_delta << ", deep delta " << deep_delta;
}

// --- register write trace (opt-in diagnostic) ------------------------------

TEST_F(Opl3Test, TraceStaysEmptyWhenNotArmed) {
    Reg(0, 0x20, 0x21);
    Reg(1, 0x05, 0x01);
    EXPECT_TRUE(opl.drain_trace().empty());
}

TEST_F(Opl3Test, ArmedTraceCapturesCycleRegisterAndValue) {
    opl.start_trace(10);
    AdvanceCycles(1234);
    Reg(0, 0x20, 0x21);   // bank 0 -> register index 0x020
    Reg(1, 0x05, 0x01);   // bank 1 -> register index 0x105
    auto events = opl.drain_trace();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].cycle, cycles_);
    EXPECT_EQ(events[0].reg, 0x020);
    EXPECT_EQ(events[0].value, 0x21);
    EXPECT_EQ(events[1].cycle, cycles_);
    EXPECT_EQ(events[1].reg, 0x105);
    EXPECT_EQ(events[1].value, 0x01);
}

TEST_F(Opl3Test, TraceStopsGrowingAtTheCap) {
    opl.start_trace(3);
    for (int i = 0; i < 10; ++i) Reg(0, 0x40, uint8_t(i));
    EXPECT_EQ(opl.drain_trace().size(), 3u);
}

TEST_F(Opl3Test, DrainTraceClearsTheBuffer) {
    opl.start_trace(10);
    Reg(0, 0x40, 0x00);
    EXPECT_FALSE(opl.drain_trace().empty());
    EXPECT_TRUE(opl.drain_trace().empty());
}

}  // namespace
