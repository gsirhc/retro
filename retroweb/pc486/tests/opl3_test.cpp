// GoogleTest suite for the Yamaha YMF262 (OPL3). Checked against opl3.h's contract
// (YMF262-M datasheet, SB Hardware Programming Guide Appendix B).

#include <gtest/gtest.h>

#include "opl3.h"

#include <algorithm>
#include <cmath>
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

    // Register write through the bank's address/data pair, as chipset.cpp's port decode does.
    void Reg(int bank, uint8_t index, uint8_t v) {
        opl.write_address(bank, index);
        opl.write_data(bank, v);
    }

    // Advance the chip by a CPU-cycle delta; the running count is never reset since tick() takes deltas.
    void AdvanceCycles(uint64_t delta) {
        cycles_ += delta;
        opl.tick(cycles_);
    }

    // Microseconds to CPU cycles at 66 MHz (Opl3::kCpuHz).
    void AdvanceMicroseconds(double us) {
        AdvanceCycles(uint64_t(us * 1e-6 * Opl3::kCpuHz));
    }

    // Channel 0 as a plain 2-op FM voice: slots 0 (modulator) and 3 (carrier), fastest attack and
    // release. Values only need key-on to sound and key-off to decay to nothing quickly.
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
    // Operator slot offsets for channels 0-8 in one bank (not contiguous).
    static uint8_t OpOffset(int ch, bool carrier) {
        static const uint8_t kBase[9] = {0x00, 0x01, 0x02, 0x08, 0x09, 0x0A, 0x10, 0x11, 0x12};
        return uint8_t(kBase[ch] + (carrier ? 3 : 0));
    }

    // One 2-op voice on any bank/channel at the given total levels, keyed on, for polyphony.
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

    // Fraction of drained samples sitting at either clamp bound.
    static double ClippedFraction(const std::vector<Opl3::Sample> &samples) {
        std::size_t clipped = 0;
        for (const auto &s : samples) {
            if (s.left >= 32767 || s.left <= -32768 || s.right >= 32767 || s.right <= -32768) ++clipped;
        }
        return samples.empty() ? 0.0 : double(clipped) / double(samples.size());
    }

    // BLOCK=4, F-number high bits=2, KON set/clear: channel 0's B0h.
    void KeyOn() { Reg(0, 0xB0, 0x32); }
    void KeyOff() { Reg(0, 0xB0, 0x12); }

    // Pair channels 0+3 into a 4-op voice with the given connection bits, key on with operator
    // `loud_op` (1-4) at full volume and the other three silenced (AR=0). Returns the peak |left|
    // after a few ms, isolating which operators a (cp,cs) algorithm sums into the output.
    static int32_t FourOpIsolatedPeak(uint8_t cp_cnt, uint8_t cs_cnt, int loud_op) {
        Opl3 chip;
        chip.reset();
        auto w = [&](int bank, uint8_t r, uint8_t v) { chip.write_address(bank, r); chip.write_data(bank, v); };
        w(1, 0x05, 0x01);  // NEW
        w(1, 0x04, 0x01);  // pair channels 0+3
        static const uint8_t kBases[4] = {0x00, 0x03, 0x08, 0x0B};  // op1,op2,op3,op4 register bases
        for (int i = 0; i < 4; ++i) {
            uint8_t base = kBases[i];
            bool loud = (i + 1) == loud_op;
            w(0, uint8_t(0x20 + base), loud ? 0x21 : 0x01);   // loud: EGT hold, MULT=1; silent: MULT=1 only
            w(0, uint8_t(0x40 + base), 0x00);                  // TL: loudest
            w(0, uint8_t(0x60 + base), loud ? 0xF0 : 0x00);    // loud: AR=15, DR=0; silent: AR=0 (never attacks)
            w(0, uint8_t(0x80 + base), 0x0F);                  // SL=0, RR=15
        }
        // C0h bits 5-4 are pan bits, live once NEW is set; 0x30 sends the primary to both sides.
        w(0, 0xC0, uint8_t(0x30 | cp_cnt));  // channel 0 (primary) connection
        w(0, 0xC3, cs_cnt);                   // channel 3 (secondary) connection
        w(0, 0xA0, 0x59);
        w(0, 0xB0, uint8_t(0x20 | (4 << 2) | 0x01));  // KON, block=4, fnum hi=1
        uint64_t cycles = 0;
        for (int i = 0; i < 10; ++i) { cycles += uint64_t(Opl3::kCpuHz * 0.001); chip.tick(cycles); }
        int32_t peak = 0;
        for (const auto &s : chip.drain_samples()) peak = std::max(peak, std::abs(int(s.left)));
        return peak;
    }

    // Carrier-only voice (modulator silenced via AR=0) at waveform `wf`, fnum=64/block=0/MULT=1:
    // 64 phase units per frame, a 2^20 cycle is 16384 frames. Returns one cycle of left samples plus
    // margin. Frame k's phase is (k+1)*64.
    static std::vector<int32_t> SampleOneWaveformCycle(uint8_t wf) {
        Opl3 chip;
        chip.reset();
        chip.write_address(1, 0x05); chip.write_data(1, 0x01);  // NEW: waveforms 4-7 need OPL3 mode
        auto w = [&](uint8_t r, uint8_t v) { chip.write_address(0, r); chip.write_data(0, v); };
        w(0x20, 0x01); w(0x40, 0x3F);  // modulator: MULT=1, silenced (AR=0 too: never attacks)
        w(0x23, 0x21);                 // carrier: EGT hold, MULT=1
        w(0x43, 0x00);                 // carrier TL: loudest
        w(0x63, 0xF0);                 // carrier AR=15 (fastest attack), DR=0
        w(0x83, 0x00);                 // carrier SL=0 (loudest sustain), RR=0
        w(0xC0, 0x30);                  // pan both channels on
        w(0xE3, wf);                     // carrier waveform
        w(0xA0, 0x40);                   // fnum = 64
        w(0xB0, 0x20);                   // KON, block=0, fnum hi=0

        std::vector<int32_t> out;
        uint64_t cycles = 0;
        const double cycles_per_frame = Opl3::kCpuHz / Opl3::kSampleHz;
        for (int i = 0; i < 6; ++i) {
            cycles += uint64_t(std::llround(4000.0 * cycles_per_frame));
            chip.tick(cycles);
            for (const auto &s : chip.drain_samples()) out.push_back(s.left);
        }
        return out;
    }
};

// Timer periods from opl3.h: clock/1024 and clock/4096, in microseconds.
constexpr double kTimer1PeriodUs = 80.8;
constexpr double kTimer2PeriodUs = 323.1;

// --- AdLib detection ------------------------------------------------------

TEST_F(Opl3Test, AdLibDetectionSequenceReturnsZeroThenC0) {
    // opl3.h's worked example, step for step.
    Reg(0, 0x04, 0x60);  // 1. mask/reset both timers
    Reg(0, 0x04, 0x80);  // 2. reset the IRQ flags
    EXPECT_EQ(opl.status(), 0x00) << "step 3: status must read 00h";

    Reg(0, 0x02, 0xFF);  // 4. timer 1 preset
    Reg(0, 0x04, 0x21);  // 4. start timer 1 (also masks timer 2, per the header text)

    // 5. wait at least 80.8 us (clock/1024) for timer 1: ~5332.8 cycles at 66 MHz, plus 10% margin.
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
    // Preset 0 counts 0 to 256, the maximum span, versus one final tick for 255.
    Reg(0, 0x02, 0x00);
    Reg(0, 0x04, 0x01);
    AdvanceMicroseconds(kTimer1PeriodUs * 256.0 * 0.97);
    EXPECT_FALSE(opl.timer1_expired());
    AdvanceMicroseconds(kTimer1PeriodUs * 256.0 * 0.06);  // now ~1.03x the full span
    EXPECT_TRUE(opl.timer1_expired());
}

TEST_F(Opl3Test, MaskBitsGateOnlyTheIrqBitNotTheTimersOwnStatusBit) {
    // 04h bits 6/5 mask a timer's contribution to bit 7 but its own status bit still latches.
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

    // Reading status is a pure read.
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
    // An OPL2 returns 6 (bits 2-1) here; that is how software tells the chips apart.
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
    Reg(1, 0x04, 0x3F);  // four-op enable bits, bank 1, must not land
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
    // Cold reset wipes a register from each area and releases all 36 operators (opl3.h).
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
    // 5 ms is ~249 frames at ~49716 Hz; allow slack since tick() emits whole frames.
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

    // Peak over the first and last quarter of release: direction and eventual silence, no exact values.
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

// --- envelope generator: instant attack, pinned idle, retrigger, rate scaling ---

// AR=15 is RATE 60 or more, which Table 3-6 (YM3812 application manual) lists as a 0.00 ms attack.
TEST_F(Opl3Test, InstantAttackReachesFullVolumeOnTheVeryFirstFewFramesWhenEffectiveRateHiIsFifteen) {
    auto peak_over_ms = [](Opl3 &chip, uint64_t &cycles, double ms) {
        const double cycles_per_frame = Opl3::kCpuHz / Opl3::kSampleHz;
        int32_t peak = 0;
        uint64_t frames = uint64_t(ms * 1e-3 * Opl3::kSampleHz);
        for (uint64_t i = 0; i < frames; ++i) {
            cycles += uint64_t(std::llround(cycles_per_frame));
            chip.tick(cycles);
            for (const auto &s : chip.drain_samples()) peak = std::max(peak, std::abs(int(s.left)));
        }
        return peak;
    };
    int32_t fast_early = 0, fast_settled = 0, slow_early = 0, slow_settled = 0;
    auto early_vs_settled = [&](uint8_t ar, int32_t &early, int32_t &settled) {
        Opl3 chip;
        chip.reset();
        auto w = [&](uint8_t r, uint8_t v) { chip.write_address(0, r); chip.write_data(0, v); };
        w(0x20, 0x01);                             // modulator: MULT=1, AR=0, stays silent
        w(0x23, 0x21);                              // carrier: EGT hold, MULT=1
        w(0x43, 0x00);                               // carrier TL=0 (loudest), KSL=0
        w(0x63, uint8_t((ar << 4) | 0x00));          // carrier AR=ar, DR=0
        w(0x83, 0x00);                                // carrier SL=0 (hold at full), RR=0
        w(0xC0, 0x30);
        w(0xA0, 0xAE);
        w(0xB0, uint8_t(0x20 | (4 << 2) | 0x02));     // KON, block=4, fnum hi bits=10b
        // fnum 0x2AE/block 4 gives a ~1.9 ms cycle at MULT=1; the window must span a full cycle to avoid a zero-crossing.
        uint64_t cycles = 0;
        early = peak_over_ms(chip, cycles, 2.5);
        settled = peak_over_ms(chip, cycles, 50.0);    // long past even a slow attack
    };
    early_vs_settled(15, fast_early, fast_settled);
    early_vs_settled(6, slow_early, slow_settled);  // rate_hi=6: slow, but not so slow it
                                                     // needs an impractically long window to settle
    ASSERT_GT(fast_settled, 0);
    ASSERT_GT(slow_settled, 0);
    EXPECT_GE(fast_early, fast_settled * 9 / 10)
        << "AR=15 (rate_hi=15) must already be at full volume within the first fraction of a "
           "millisecond, not ramping up -- fast_early=" << fast_early << " fast_settled=" << fast_settled;
    EXPECT_LT(slow_early, slow_settled / 2)
        << "AR=6 (rate_hi well under 15) must still be well below its settled volume after the "
           "same short window, proving the instant case is specific to rate_hi=15 -- slow_early="
        << slow_early << " slow_settled=" << slow_settled;
}

// Idle is derived from release with attenuation pinned at maximum (no stored off state, opl3.h Env);
// a later key-on must still attack normally.
TEST_F(Opl3Test, FullyIdleOperatorAttacksNormallyAgainOnASubsequentKeyOn) {
    SetUpAudibleChannel();
    auto peak = [&] {
        int32_t p = 0;
        for (const auto &s : opl.drain_samples()) p = std::max(p, std::abs(int(s.left)));
        return p;
    };
    KeyOn();
    AdvanceMicroseconds(2000.0);
    int32_t peak1 = peak();
    ASSERT_GT(peak1, 0);

    KeyOff();
    AdvanceMicroseconds(100000.0);  // far past release, fully idle
    opl.drain_samples();
    ASSERT_FALSE(opl.active());

    KeyOn();  // retrigger from true silence, not mid-release
    EXPECT_TRUE(opl.active());
    AdvanceMicroseconds(2000.0);
    int32_t peak2 = peak();
    EXPECT_GT(peak2, peak1 / 2)
        << "a fresh key-on from full idle must attack normally again, not stay silent because "
           "\"idle\" used to be a separate stored enum value -- peak1=" << peak1 << " peak2=" << peak2;
}

// Key-on while still in release re-triggers attack from the current attenuation, with no dip
// toward silence.
TEST_F(Opl3Test, RetriggeringMidReleaseResumesFromThereInsteadOfDippingToSilenceFirst) {
    Opl3 chip;
    chip.reset();
    auto w = [&](uint8_t r, uint8_t v) { chip.write_address(0, r); chip.write_data(0, v); };
    w(0x20, 0x01);   // modulator: MULT=1, AR=0, stays silent
    w(0x23, 0x21);   // carrier: EGT hold, MULT=1
    w(0x43, 0x00);   // carrier TL=0, KSL=0
    w(0x63, 0xF0);   // carrier AR=15 (instant, for a reliable fast start), DR=0
    w(0x83, 0x08);   // carrier SL=0, RR=8 (moderate release)
    w(0xC0, 0x30);
    w(0xA0, 0xAE);

    uint64_t cycles = 0;
    const double cycles_per_frame = Opl3::kCpuHz / Opl3::kSampleHz;
    auto advance_ms = [&](double ms) {
        uint64_t frames = uint64_t(ms * 1e-3 * Opl3::kSampleHz);
        for (uint64_t i = 0; i < frames; ++i) {
            cycles += uint64_t(std::llround(cycles_per_frame));
            chip.tick(cycles);
        }
    };
    auto peak_since_drain = [&] {
        int32_t p = 0;
        for (const auto &s : chip.drain_samples()) p = std::max(p, std::abs(int(s.left)));
        return p;
    };

    chip.write_address(0, 0xB0); chip.write_data(0, uint8_t(0x20 | (4 << 2) | 0x02));  // KON
    advance_ms(2.0);  // instant attack, then settle
    chip.drain_samples();

    // Fnum 0x2AE/block 4 gives a ~1.9 ms cycle (MULT=1) and key-on zeroes phase, so windows span a
    // full cycle. Release runs 30 ms (about 9% of RR=8's ~330 ms) so a reset-to-silence bug can't hide.
    chip.write_address(0, 0xB0); chip.write_data(0, uint8_t(0x00 | (4 << 2) | 0x02));  // KOFF
    advance_ms(27.5);
    chip.drain_samples();
    advance_ms(2.5);  // >1 cycle, ending right at the retrigger point
    int32_t mid_release = peak_since_drain();
    ASSERT_GT(mid_release, 0) << "release must still be audible, not already silent, at the retrigger point";

    // Moderate attack rate before retriggering so the attack is observable.
    chip.write_address(0, 0x63); chip.write_data(0, 0x80);  // AR=8, DR=0

    chip.write_address(0, 0xB0); chip.write_data(0, uint8_t(0x20 | (4 << 2) | 0x02));  // retrigger
    advance_ms(2.5);  // the first >1-cycle window after retriggering
    int32_t just_after = peak_since_drain();
    EXPECT_GE(just_after, mid_release * 7 / 10)
        << "a key-on while still releasing must resume attack from the current attenuation, not "
           "reset to silence first -- mid_release=" << mid_release << " just_after=" << just_after;

    advance_ms(30.0);
    int32_t full_again = peak_since_drain();
    EXPECT_GT(full_again, mid_release)
        << "the retriggered attack must still climb back toward full volume -- full_again="
        << full_again << " mid_release=" << mid_release;
}

// Each +1 step of a rate register (+4 effective RATE) halves the full-range traverse time
// (YM3812 application manual Table 3-6), so N steps is 2^N.
TEST_F(Opl3Test, DecayAttenuationAtAFixedElapsedTimeRoughlyDoublesForOneStepOfTheRateRegister) {
    auto make_chip = [](Opl3 &chip, uint8_t dr, uint8_t sl) {
        chip.reset();
        auto w = [&](uint8_t r, uint8_t v) { chip.write_address(0, r); chip.write_data(0, v); };
        w(0x20, 0x01);                             // modulator: MULT=1, AR=0, stays silent
        w(0x23, 0x21);                              // carrier: EGT hold, MULT=1
        w(0x43, 0x00);                               // carrier TL=0, KSL=0
        w(0x63, uint8_t(0xF0 | (dr & 0x0F)));        // carrier AR=15 (instant), DR=dr
        w(0x83, uint8_t((sl & 0x0F) << 4));          // SL=sl, RR=0
        w(0xC0, 0x30);
        w(0xA0, 0x59);
        w(0xB0, 0x31);  // KON, block=4, fnum hi=1 -> fnum=0x159 (a ~1.9 ms waveform cycle)
    };
    auto peak_over_ms = [](Opl3 &chip, uint64_t &cycles, double ms) {
        const double cycles_per_frame = Opl3::kCpuHz / Opl3::kSampleHz;
        int32_t peak = 0;
        uint64_t frames = uint64_t(ms * 1e-3 * Opl3::kSampleHz);
        for (uint64_t i = 0; i < frames; ++i) {
            cycles += uint64_t(std::llround(cycles_per_frame));
            chip.tick(cycles);
            for (const auto &s : chip.drain_samples()) peak = std::max(peak, std::abs(int(s.left)));
        }
        return peak;
    };
    // SL=0 reference: nothing decays, so full scale is isolated from the DR under test.
    Opl3 ref;
    uint64_t ref_cycles = 0;
    make_chip(ref, 15, 0);
    peak_over_ms(ref, ref_cycles, 1.0);  // past the instant attack
    int32_t full = peak_over_ms(ref, ref_cycles, 4.0);

    auto db_down_at = [&](uint8_t dr, double ms) -> double {
        Opl3 chip;
        make_chip(chip, dr, 15);  // SL=15, decay runs the complete range
        uint64_t cycles = 0;
        peak_over_ms(chip, cycles, std::max(0.0, ms - 4.0));  // skip ahead, discarding
        int32_t at_t = peak_over_ms(chip, cycles, 4.0);       // >1 cycle, ending at t=ms
        return 20.0 * std::log10(double(full) / double(std::max(at_t, 1)));
    };
    double db7 = db_down_at(7, 50.0);
    double db8 = db_down_at(8, 50.0);
    ASSERT_GT(db7, 1.0) << "must still be a measurable, non-trivial attenuation at this point";
    EXPECT_NEAR(db8 / db7, 2.0, 0.5)
        << "DR=8 is one register step (= 4 RATE units) above DR=7 and must show roughly double "
           "the attenuation at the same elapsed time -- db7=" << db7 << " db8=" << db8;
}

// --- envelope times against the YM3812 application manual, Table 3-6 (p.16) ---
// The table is at fM = 3.6 MHz, one sample per 72 clocks: 50 frames per ms. The YMF262 runs the
// same 72-clock frame from 14.31818 MHz / 4, so times compare in frames.

class Opl3EnvelopeTimeTest : public ::testing::Test {
protected:
    static constexpr int kCarrier = 3;  // op_[3]: channel 0's carrier
    static constexpr double kFramesPerMs = 50.0;
    Opl3 chip;
    uint64_t cycles_ = 0;

    void SetUp() override {
        chip.reset();
        cycles_ = 0;
        chip.set_cpu_hz(Opl3::kSampleHz * 64.0);  // 64 cycles per frame, exactly
    }
    void W(uint8_t r, uint8_t v) { chip.write_address(0, r); chip.write_data(0, v); }

    // Carrier with KSR=1 so RATE = R*4 + ksn; ksn = BLOCK*2 + F-number bit 8 (NTS=0)
    void Voice(uint8_t ar, uint8_t dr, uint8_t sl, uint8_t rr, int ksn, bool egt = true) {
        W(0x23, uint8_t((egt ? 0x20 : 0x00) | 0x10 | 0x01));
        W(0x43, 0x00);
        W(0x63, uint8_t((ar << 4) | dr));
        W(0x83, uint8_t((sl << 4) | rr));
        W(0xA0, 0x00);
        block_fnum_ = uint8_t(((ksn >> 1) << 2) | (ksn & 1));
    }
    void KeyOn() { W(0xB0, uint8_t(0x20 | block_fnum_)); }
    void KeyOff() { W(0xB0, block_fnum_); }

    // Frames until pred(level) holds, checked every `chunk` frames; -1 past `limit`
    template <typename Pred>
    long FramesUntil(Pred pred, long limit, long chunk = 1) {
        for (long f = 0; f <= limit; f += chunk) {
            if (pred(chip.envelope_level(kCarrier))) return f;
            cycles_ += uint64_t(64 * chunk);
            chip.tick(cycles_);
        }
        return -1;
    }
    static double Tolerance(double frames, double pct) { return std::max(frames * pct, 2.0); }

private:
    uint8_t block_fnum_ = 0;
};

// Attack, 0-100% (silence to 0 dB) and 10-90% (20 dB to 0.9 dB), at RATE = RM*4 + RL
TEST_F(Opl3EnvelopeTimeTest, AttackTimesMatchTable36AcrossRates) {
    struct Row { uint8_t r; int ksn; double ms_0_100, ms_10_90; };
    const Row rows[] = {
        {1, 0, 2826.24, 1482.75},  // RATE 4
        {4, 3, 199.68, 108.54},    // RATE 19
        {9, 2, 7.36, 3.87},        // RATE 38
        {12, 1, 1.12, 0.61},       // RATE 49
        {13, 0, 0.70, 0.37},       // RATE 52
    };
    for (const Row &row : rows) {
        SetUp();
        Voice(row.r, 0, 0, 0, row.ksn);
        ASSERT_EQ(chip.envelope_level(kCarrier), 511);
        KeyOn();
        long chunk = row.ms_0_100 > 100.0 ? 8 : 1;
        long limit = long(row.ms_0_100 * kFramesPerMs * 2.0);
        long t10 = FramesUntil([](int32_t l) { return l <= 106; }, limit, chunk);
        long t90 = FramesUntil([](int32_t l) { return l <= 4; }, limit, chunk);
        long t100 = FramesUntil([](int32_t l) { return l == 0; }, limit, chunk);
        ASSERT_GE(t10, 0);
        ASSERT_GE(t90, 0);
        ASSERT_GE(t100, 0);
        long done = t10 + t90 + t100;
        double want_100 = row.ms_0_100 * kFramesPerMs;
        double want_1090 = row.ms_10_90 * kFramesPerMs;
        EXPECT_NEAR(double(done), want_100, Tolerance(want_100, 0.02)) << "RATE " << row.r * 4 + row.ksn;
        EXPECT_NEAR(double(t90), want_1090, Tolerance(want_1090, 0.05)) << "RATE " << row.r * 4 + row.ksn;
    }
}

// Release covers the decay table's 0-96 dB in the same time (manual 3-1-7)
TEST_F(Opl3EnvelopeTimeTest, ReleaseTimesMatchTable36DecayColumnAcrossRates) {
    struct Row { uint8_t r; int ksn; double ms; };
    const Row rows[] = {
        {1, 0, 39280.64},  // RATE 4
        {5, 2, 1635.84},   // RATE 22
        {10, 1, 61.36},    // RATE 41
        {14, 0, 4.80},     // RATE 56
        {15, 2, 2.40},     // RATE 62: 60-63 share one time
    };
    for (const Row &row : rows) {
        SetUp();
        Voice(15, 0, 0, row.r, row.ksn);
        KeyOn();
        ASSERT_EQ(FramesUntil([](int32_t l) { return l == 0; }, 4), 1);
        KeyOff();
        double want = row.ms * kFramesPerMs;
        long chunk = row.ms > 1000.0 ? 64 : 1;
        long t = FramesUntil([](int32_t l) { return l >= 511; }, long(want * 2.0), chunk);
        ASSERT_GE(t, 0) << "RATE " << row.r * 4 + row.ksn;
        EXPECT_NEAR(double(t), want, Tolerance(want, 0.02)) << "RATE " << row.r * 4 + row.ksn;
    }
}

// Decay runs at the same speed as release and stops at SL (3 dB steps) while KON holds with EGT=1
TEST_F(Opl3EnvelopeTimeTest, DecayStopsAtTheSustainLevelAndHoldsWithEgtSet) {
    Voice(15, 10, 14, 0, 1);  // DR RATE 41, SL=14: 42 dB
    KeyOn();
    ASSERT_EQ(FramesUntil([](int32_t l) { return l == 0; }, 4), 1);
    long t = FramesUntil([](int32_t l) { return l >= 224; }, 10000);
    double want = 61.36 * kFramesPerMs * 224.0 / 512.0;
    EXPECT_NEAR(double(t), want, Tolerance(want, 0.02));
    FramesUntil([](int32_t) { return false; }, 5000);
    EXPECT_EQ(chip.envelope_level(kCarrier), 224) << "42 dB = 224 units, held";
}

// SL=15 is 93 dB, not 45 (YMF262 datasheet SL)
TEST_F(Opl3EnvelopeTimeTest, SustainLevelFifteenIsNinetyThreeDecibels) {
    Voice(15, 14, 15, 0, 0);
    KeyOn();
    FramesUntil([](int32_t) { return false; }, 2000);
    EXPECT_EQ(chip.envelope_level(kCarrier), 496);
}

// EGT=0: the release rate takes over at SL even with KON held (YMF262 datasheet EGT)
TEST_F(Opl3EnvelopeTimeTest, EgtClearReleasesFromTheSustainLevelWhileKeyedOn) {
    Voice(15, 14, 4, 14, 0, /*egt=*/false);
    KeyOn();
    ASSERT_EQ(FramesUntil([](int32_t l) { return l == 0; }, 4), 1);
    long t = FramesUntil([](int32_t l) { return l >= 511; }, 5000);
    ASSERT_GE(t, 0) << "a percussive envelope must fall to silence with KON still set";
    // 64 units at the DR rate, then 447 at the RR rate, both RATE 56
    double want = 4.80 * kFramesPerMs;
    EXPECT_NEAR(double(t), want, Tolerance(want, 0.03));
}

// Rate 0 never moves the envelope (Table 3-6 note)
TEST_F(Opl3EnvelopeTimeTest, RateZeroLeavesTheEnvelopeWhereItIs) {
    Voice(0, 0, 0, 0, 15);
    KeyOn();
    FramesUntil([](int32_t) { return false; }, 5000);
    EXPECT_EQ(chip.envelope_level(kCarrier), 511) << "AR=0 must not attack, even with KSR";
}

// A coarse tick must not slip the clock. advance() bounds frames per call but consumes credit for
// every frame due, so a long gap drops audio instead of leaving the chip behind.
TEST_F(Opl3Test, CoarseTicksDoNotMakeTheChipsClockFallBehind) {
    // A timer alone keeps the chip active.
    Reg(0, 0x02, 0x00);
    Reg(0, 0x04, 0x01);

    // 250 ms per tick is far past advance()'s per-call frame bound.
    const uint64_t step = uint64_t(Opl3::kCpuHz * 0.25);
    uint64_t cycles = 0;
    uint64_t emitted = 0;
    for (int i = 0; i < 8; ++i) {
        cycles += step;
        opl.tick(cycles);
        emitted += opl.drain_samples().size();
    }
    // After 2s of guest time the chip must have accounted for ~2s of frames.
    const double elapsed = double(cycles) / Opl3::kCpuHz;
    EXPECT_LE(emitted, std::size_t(elapsed * Opl3::kSampleHz * 1.01));

    // Fine ticks must be immediately correct, with no backlog carried forward.
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

// F = fnum * kSampleHz / 2^(20-Block); fnum 159h at block 4 is middle C. An error here is in octaves.
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

        // Zero crossings over the sustained tail, past the attack.
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

// MULT is a half-integer multiplier: MULT=0 is x0.5, MULT=2 is x2.
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
    // C0h bit 4 is CHA (left), bit 5 is CHB (right).
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
    // NEW is clear out of reset; a mono-era driver must still hear both speakers (opl3.h).
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
    // Frame rate is fixed silicon: 14.31818 MHz / 288 = 49715.9 Hz.
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

// The master gain has no hardware citation (opl3.cpp), so the clamp pins it: 18 moderately
// attenuated voices is full OPL3 polyphony as period music voices it.
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

// One unattenuated voice must leave headroom for the rest (~18%, opl3.cpp gain comment).
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

// kModulationIndexRadians (opl3.cpp) is 8*pi, double kFeedbackRadians[7] (4*pi): a full-scale
// operator output is 4084 units against a 1024 units/cycle phase adder, and cross-operator
// modulation injects it unshifted while feedback shifts by (9-FB). This checks the audible
// consequence: attenuate the modulator by a known TL, then compare the carrier's frequency
// shift near key-on (peak beta*wm*cos(wm t) at t=0) with beta*fm.
TEST_F(Opl3Test, FullScaleModulationMatchesAnEightPiModulationIndex) {
    // Interpolated zero-crossing frequency over [lo,hi) from the first and last crossing.
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
    w(0x23, uint8_t(0x20 | 15));  // carrier: EGT hold, MULT=15 (x15), a 15:1 carrier:modulator ratio
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
    // The short window attenuates the peak deviation by sin(wm*T)/(wm*T), wm = 2*pi*modulator_hz.
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

// In 4-op mode the secondary channel's A0h/B0h are latched but not live; the primary's fnum/block
// drive all four operators (opl3.cpp generate_frame). Isolate ops 3/4
// by diffing against an identical 2-op channel.
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
    // Only channel 0 gets a pitch; channel 3's A0h/B0h stay at zero.
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

    // op3/op4 are heavily phase-modulated, but must repeat exactly at the primary's cycle length,
    // so check periodicity instead of counting crossings.
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

// The (cp,cs) combinations select the four OPL3 4-op algorithms (moddingwiki OPL chip reference):
// FM-FM (0,0) outputs Op4; AM-FM (1,0) Op1+Op4; FM-AM (0,1) Op2+Op4; AM-AM (1,1) Op1+Op3+Op4.
// (0,1) is covered by the waveform test above; these isolate which operators reach the output.
TEST_F(Opl3Test, FourOpFmFmAlgorithmOutputsOnlyTheFinalOperator) {
    // cp=0, cs=0: only Op4 sums into the output.
    EXPECT_EQ(FourOpIsolatedPeak(0x00, 0x00, 1), 0) << "Op1 alone must not reach the output";
    EXPECT_EQ(FourOpIsolatedPeak(0x00, 0x00, 2), 0) << "Op2 alone must not reach the output";
    EXPECT_EQ(FourOpIsolatedPeak(0x00, 0x00, 3), 0) << "Op3 alone must not reach the output";
    EXPECT_GT(FourOpIsolatedPeak(0x00, 0x00, 4), 1000) << "Op4 alone must reach the output";
}

TEST_F(Opl3Test, FourOpAmFmAlgorithmSumsTheFirstAndLastOperators) {
    // cp=1, cs=0: Op1 sums directly; Op2 and Op3 only modulate Op4.
    EXPECT_GT(FourOpIsolatedPeak(0x01, 0x00, 1), 1000) << "Op1 alone must reach the output";
    EXPECT_EQ(FourOpIsolatedPeak(0x01, 0x00, 2), 0) << "Op2 alone must not reach the output";
    EXPECT_EQ(FourOpIsolatedPeak(0x01, 0x00, 3), 0) << "Op3 alone must not reach the output";
    EXPECT_GT(FourOpIsolatedPeak(0x01, 0x00, 4), 1000) << "Op4 alone must reach the output";
}

TEST_F(Opl3Test, FourOpAmAmAlgorithmSumsThreeOfTheFourOperators) {
    // cp=1, cs=1: Op1, Op3 and Op4 sum; Op2 is a pure modulator.
    EXPECT_GT(FourOpIsolatedPeak(0x01, 0x01, 1), 1000) << "Op1 alone must reach the output";
    EXPECT_EQ(FourOpIsolatedPeak(0x01, 0x01, 2), 0) << "Op2 alone must not reach the output";
    EXPECT_GT(FourOpIsolatedPeak(0x01, 0x01, 3), 1000) << "Op3 alone must reach the output";
    EXPECT_GT(FourOpIsolatedPeak(0x01, 0x01, 4), 1000) << "Op4 alone must reach the output";
}

// --- key-scale level (KSL) -------------------------------------------------

// KSL attenuates by octave: at BLOCK=7 and F-number top bits 15 Table 3-5 (YM3812 application
// manual) gives 21 dB. KSL=1 is that figure, 2 half, 3 double (Table 3-4). The modulator is silenced (AR=0) so the carrier is unmodulated.
TEST_F(Opl3Test, KeyScaleLevelMatchesDocumentedDbPerOctaveSteps) {
    auto peak_for = [](uint8_t ksl) -> int32_t {
        Opl3 chip;
        chip.reset();
        auto w = [&](uint8_t r, uint8_t v) { chip.write_address(0, r); chip.write_data(0, v); };
        w(0x20, 0x01);                          // modulator: MULT=1, AR=0, never attacks, stays silent
        w(0x23, 0x21);                          // carrier: EGT hold, MULT=1
        w(0x43, uint8_t((ksl << 6) | 0x00));    // carrier KSL + TL=0 (loudest)
        w(0x63, 0xF0);                           // carrier AR=15 (fastest attack), DR=0
        w(0x83, 0x00);                           // carrier SL=0, RR=0
        w(0xC0, 0x30);                            // pan both channels on, CNT=0 (irrelevant: modulator is silent)
        w(0xA0, 0xFF);                            // fnum low byte all-1s
        w(0xB0, uint8_t(0x20 | (7 << 2) | 0x03)); // KON, BLOCK=7, fnum hi bits=11b -> fnum=0x3FF (top4=15)
        uint64_t cycles = 0;
        for (int i = 0; i < 5; ++i) { cycles += uint64_t(Opl3::kCpuHz * 0.001); chip.tick(cycles); }
        int32_t peak = 0;
        for (const auto &s : chip.drain_samples()) peak = std::max(peak, std::abs(int(s.left)));
        return peak;
    };
    const int32_t p0 = peak_for(0);
    const int32_t p1 = peak_for(1);
    const int32_t p2 = peak_for(2);
    const int32_t p3 = peak_for(3);
    ASSERT_GT(p0, 0);
    auto db_down = [](int32_t ref, int32_t p) { return 20.0 * std::log10(double(ref) / double(std::max(p, 1))); };
    EXPECT_NEAR(db_down(p0, p1), 21.0, 1.5)
        << "KSL=1 at BLOCK=7, fnum top4=15 must attenuate ~21 dB (the 3 dB/octave figure): p0=" << p0 << " p1=" << p1;
    EXPECT_NEAR(db_down(p0, p2), 10.5, 1.5)
        << "KSL=2 must attenuate half of KSL=1 (1.5 dB/octave): p0=" << p0 << " p2=" << p2;
    EXPECT_NEAR(db_down(p0, p3), 42.0, 2.5)
        << "KSL=3 must attenuate double KSL=1 (6 dB/octave): p0=" << p0 << " p3=" << p3;
}

// Lower octaves follow Table 3-5's own cells: 3 dB less per octave, floored at 0
TEST_F(Opl3Test, KeyScaleLevelMatchesTable35BelowTheTopOctave) {
    auto peak_for = [](uint8_t ksl, int block, uint16_t fnum) -> int32_t {
        Opl3 chip;
        chip.reset();
        auto w = [&](uint8_t r, uint8_t v) { chip.write_address(0, r); chip.write_data(0, v); };
        w(0x20, 0x01);
        w(0x23, 0x21);
        w(0x43, uint8_t(ksl << 6));
        w(0x63, 0xF0);
        w(0x83, 0x00);
        w(0xC0, 0x30);
        w(0xA0, uint8_t(fnum & 0xFF));
        w(0xB0, uint8_t(0x20 | (block << 2) | (fnum >> 8)));
        uint64_t cycles = 0;
        for (int i = 0; i < 40; ++i) { cycles += uint64_t(Opl3::kCpuHz * 0.001); chip.tick(cycles); }
        int32_t peak = 0;
        for (const auto &s : chip.drain_samples()) peak = std::max(peak, std::abs(int(s.left)));
        return peak;
    };
    struct Cell { int block; uint16_t fnum; double db; };
    const Cell cells[] = {
        {3, 0x150, 4.125},   // OCT 3, F-number 5
        {5, 0x310, 13.875},  // OCT 5, F-number 12
        {6, 0x090, 9.000},   // OCT 6, F-number 2
        {1, 0x0D0, 0.000},   // OCT 1, F-number 3
    };
    for (const Cell &c : cells) {
        int32_t p0 = peak_for(0, c.block, c.fnum);
        int32_t p1 = peak_for(1, c.block, c.fnum);
        ASSERT_GT(p0, 0);
        double db = 20.0 * std::log10(double(p0) / double(std::max(p1, 1)));
        EXPECT_NEAR(db, c.db, 0.3) << "OCT " << c.block << " fnum 0x" << std::hex << c.fnum;
    }
}

// --- key-scale rate (KSR) ----------------------------------------------------

// RATE = register*4 + Rof; Rof is the key-scale number when KSR=1, or ksn>>2 when KSR=0
// (YMF262 datasheet KSR). At BLOCK=7, NTS=1, ksn=15: KSR=1 adds 15, KSR=0 adds 3, 12 steps
// apart, an 8x difference in decay speed.
TEST_F(Opl3Test, KeyScaleRateOnVersusOffChangesTheDecayRateAtAFixedNote) {
    auto peak_after_ms = [](bool ksr, int ms) -> int32_t {
        Opl3 chip;
        chip.reset();
        auto w = [&](uint8_t r, uint8_t v) { chip.write_address(0, r); chip.write_data(0, v); };
        w(0x20, 0x01);                                 // modulator: MULT=1, AR=0, never attacks, stays silent
        w(0x23, uint8_t(0x21 | (ksr ? 0x10 : 0x00)));  // carrier: EGT hold, MULT=1, KSR
        w(0x43, 0x00);                                   // carrier TL: loudest, KSL=0
        w(0x63, 0xF4);                                   // carrier AR=15 (fastest attack), DR=4
        w(0x83, 0xF0);                                   // carrier SL=15 (decay runs to full silence), RR=0
        w(0xC0, 0x30);                                    // pan both channels on
        w(0x08, 0x40);                                    // NTS=1, selects fnum bit 9 for key scaling
        w(0xA0, 0xFF);
        w(0xB0, uint8_t(0x20 | (7 << 2) | 0x02));  // KON, BLOCK=7, fnum hi bits: bit9=1,bit8=0 -> ksn=15
        uint64_t cycles = 0;
        const uint64_t step = uint64_t(Opl3::kCpuHz * 0.001);
        for (int i = 0; i < ms; ++i) { cycles += step; chip.tick(cycles); chip.drain_samples(); }
        int32_t peak = 0;
        for (int i = 0; i < 5; ++i) {
            cycles += step;
            chip.tick(cycles);
            for (const auto &s : chip.drain_samples()) peak = std::max(peak, std::abs(int(s.left)));
        }
        return peak;
    };
    const int32_t with_ksr = peak_after_ms(true, 700);
    const int32_t without_ksr = peak_after_ms(false, 700);
    EXPECT_LT(with_ksr, without_ksr / 4)
        << "KSR=1 must decay substantially faster than KSR=0 at the same high key-scale number: "
        << "KSR=1 peak=" << with_ksr << " KSR=0 peak=" << without_ksr;
}

// --- note-select / key-scale number -------------------------------------------

// NTS=0 splits on F-number bit 8, NTS=1 on bit 9 (YM3812 application manual 3-1-3, YMF262
// datasheet NTS). Observed through KSR=1, where ksn feeds the rate directly.
TEST_F(Opl3Test, NoteSelectPicksFnumBitEightWhenClearAndBitNineWhenSet) {
    auto decayed_peak = [](bool nts) -> int32_t {
        Opl3 chip;
        auto w = [&](uint8_t r, uint8_t v) { chip.write_address(0, r); chip.write_data(0, v); };
        w(0x08, nts ? 0x40 : 0x00);
        w(0x20, 0x01);                  // modulator: MULT=1 (silenced below)
        w(0x23, 0x31);                  // carrier: EGT hold, KSR=1, MULT=1
        w(0x40, 0x3F); w(0x43, 0x00);   // modulator TL: silent, carrier TL: loudest
        w(0x60, 0xF0); w(0x63, 0xF4);   // modulator fastest attack; carrier AR=15, DR=4
        w(0x80, 0x00); w(0x83, 0xF0);   // carrier SL=15 (decays toward full attenuation, held by EGT), RR=0
        // fnum = 0x200: bit 9 set, bit 8 clear.
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
    const int32_t peak_nts0 = decayed_peak(false);  // selects fnum bit 8 (=0) -> ksn=0, slower decay
    const int32_t peak_nts1 = decayed_peak(true);   // selects fnum bit 9 (=1) -> ksn=1, faster decay
    EXPECT_LT(peak_nts1, peak_nts0 * 0.9)
        << "NTS must pick different fnum bits, giving a different key-scaled decay rate: "
        << "peak after 2s, NTS=0 " << peak_nts0 << ", NTS=1 " << peak_nts1;
}

// --- rhythm output doubling ---------------------------------------------------

// Rhythm voices sum at twice a melodic channel (unsourced, PC486_REVIEW.md §35).
// The bass drum is an ordinary 2-op channel, so it compares cleanly against a melodic voice.
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
        w(0xB6, 0x12);  // block=4, fnum hi=2, no KON here; see below
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

// --- waveforms 1-6 -----------------------------------------------------------

// SampleOneWaveformCycle's fnum=64/block=0 gives four 4096-frame quadrants. Frames 2047 and 10239
// hit the same logsin entry (qidx=128) a half-cycle apart, so waveform sign/repeat is a direct comparison.

TEST_F(Opl3Test, Waveform1IsAHalfSineWithTheNegativeHalfClampedToZero) {
    auto samples = SampleOneWaveformCycle(1);
    ASSERT_GT(samples.size(), 16400u);
    EXPECT_GT(samples[2047], 1000) << "the positive half (quadrants 0-1) must sound like an ordinary sine";
    EXPECT_EQ(samples[8292], 0) << "the negative half must be clamped to exactly zero, not mirrored";
    EXPECT_EQ(samples[14000], 0) << "stays clamped to zero across the whole negative half";
}

TEST_F(Opl3Test, Waveform2IsFullWaveRectifiedAlwaysPositive) {
    auto samples = SampleOneWaveformCycle(2);
    ASSERT_GT(samples.size(), 16400u);
    const int32_t pos_half = samples[2047];
    const int32_t mirrored = samples[10239];  // same qidx, one half-cycle later
    EXPECT_GT(pos_half, 1000) << "quadrant 0 must sound like an ordinary sine";
    EXPECT_GT(mirrored, 1000) << "quadrant 2 must mirror the same magnitude but stay positive, not flip negative";
    EXPECT_NEAR(pos_half, mirrored, pos_half / 10 + 20)
        << "full-wave rectification: magnitude at the mirrored position must match: pos=" << pos_half
        << " mirrored=" << mirrored;
}

TEST_F(Opl3Test, Waveform3RepeatsItsRisingQuarterTwicePerCycleWithSilenceBetween) {
    auto samples = SampleOneWaveformCycle(3);
    ASSERT_GT(samples.size(), 16400u);
    EXPECT_GT(samples[2047], 1000) << "quadrant 0 plays the rising quarter";
    EXPECT_EQ(samples[6000], 0) << "quadrant 1 is silent";
    EXPECT_EQ(samples[14000], 0) << "quadrant 3 is silent";
    const int32_t q0 = samples[2047];
    const int32_t q2 = samples[10239];
    EXPECT_NEAR(q0, q2, std::abs(q0) / 20 + 20)
        << "quadrant 2 must repeat quadrant 0's exact rising-quarter shape: q0=" << q0 << " q2=" << q2;
}

TEST_F(Opl3Test, Waveform4IsASilentSecondHalfWithDoubleFrequencySineInTheFirst) {
    auto samples = SampleOneWaveformCycle(4);
    ASSERT_GT(samples.size(), 16400u);
    EXPECT_EQ(samples[9599], 0) << "the second half of the base cycle must be silent";
    const int32_t a = samples[1023];  // idx1024=64 -> doubled phase qidx=128, sign=+1
    const int32_t b = samples[5119];  // idx1024=320 (still in the active first half) -> same qidx=128, sign=-1
    EXPECT_GT(std::abs(a), 500) << "must be clearly audible, not silent, at this point";
    EXPECT_NEAR(a, -b, std::abs(a) / 10 + 50)
        << "doubling the phase must repeat the same shape with the sign flipped halfway through the first half: a="
        << a << " b=" << b;
}

TEST_F(Opl3Test, Waveform5IsASilentSecondHalfWithRectifiedDoubleFrequencySineInTheFirst) {
    auto samples = SampleOneWaveformCycle(5);
    ASSERT_GT(samples.size(), 16400u);
    EXPECT_EQ(samples[9599], 0) << "the second half of the base cycle must be silent";
    const int32_t a = samples[1023];
    const int32_t b = samples[5119];
    EXPECT_GT(a, 500) << "must be positive and clearly audible at this point";
    EXPECT_NEAR(a, b, a / 10 + 50)
        << "rectified: both halves of the doubled phase must agree in sign (always positive) and magnitude: a="
        << a << " b=" << b;
}

TEST_F(Opl3Test, Waveform6IsAFullAmplitudeSquareWaveWithNoWaveformShaping) {
    auto samples = SampleOneWaveformCycle(6);
    ASSERT_GT(samples.size(), 16400u);
    const int32_t early_pos = samples[100];   // well within quadrant 0 (positive half)
    const int32_t early_neg = samples[8292];  // the same early offset into quadrant 2 (negative half)
    EXPECT_GT(early_pos, 3000) << "a square wave must be near full scale immediately, not ramping up like a sine";
    EXPECT_LT(early_neg, -3000) << "the negative half must likewise be near full scale immediately";
    EXPECT_NEAR(early_pos, -early_neg, 200)
        << "no waveform shaping: magnitude must be identical in both halves, only the sign differs: pos="
        << early_pos << " neg=" << early_neg;
}

// --- waveform 7 (exponential-decay sawtooth) ----------------------------------

// The negative half mirrors the positive one (YMF262 datasheet WS=7 figure): it starts near
// silence and rises to full amplitude only at the end of the cycle.
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

    // Sample k uses phase=(k+1)*64 (key-on reset and that frame's advance precede sampling). Frame 100
    // is near peak, 8192 the first negative frame, 16382 the last before the wrap.
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

// VIB is a 6.1 Hz vibrato, 7 cents deep or 14 with DVB (YMF262 datasheet VIB, DVB).
TEST_F(Opl3Test, VibratoSwingsSevenCentsAtSixPointOneHertzAndDvbMakesItFourteen) {
    // Interpolated zero-crossing frequency over [lo,hi)
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
    auto record = [](bool vib, bool dvb) {
        Opl3 chip;
        auto w = [&](uint8_t r, uint8_t v) { chip.write_address(0, r); chip.write_data(0, v); };
        w(0xBD, dvb ? 0x40 : 0x00);
        w(0x20, 0x01);  // modulator: MULT=1 (AR=0 default: never attacks, stays silent)
        w(0x23, uint8_t(0x20 | (vib ? 0x40 : 0x00) | 0x01));  // carrier: EGT hold, VIB, MULT=1
        w(0x43, 0x00);
        w(0x63, 0xF0); w(0x83, 0x00);
        w(0xA0, 0x00);
        w(0xB0, uint8_t(0x20 | (7 << 2) | 0x02));  // KON, block=7, fnum=0x200: ~3.1 kHz
        std::vector<int32_t> left;
        uint64_t cycles = 0;
        const double cycles_per_frame = Opl3::kCpuHz / Opl3::kSampleHz;
        for (int i = 0; i < 18; ++i) {
            cycles += uint64_t(std::llround(1000.0 * cycles_per_frame));
            chip.tick(cycles);
            for (const auto &s : chip.drain_samples()) left.push_back(s.left);
        }
        return left;
    };
    struct Swing { double cents_up, cents_down; long peak_frame, trough_frame; };
    auto swing = [&](const std::vector<int32_t> &v, double ref_hz) {
        constexpr std::size_t kWindow = 300;
        Swing s{-1e9, 1e9, 0, 0};
        for (std::size_t lo = 0; lo + kWindow <= 16000 && lo + kWindow <= v.size(); lo += 50) {
            double cents = 1200.0 * std::log2(measured_hz(v, lo, lo + kWindow) / ref_hz);
            if (cents > s.cents_up) { s.cents_up = cents; s.peak_frame = long(lo); }
            if (cents < s.cents_down) { s.cents_down = cents; s.trough_frame = long(lo); }
        }
        return s;
    };
    const double ref_hz = measured_hz(record(false, false), 100, 16000);
    ASSERT_GT(ref_hz, 3000.0);

    Swing off = swing(record(false, false), ref_hz);
    EXPECT_LT(std::max(off.cents_up, -off.cents_down), 0.5) << "VIB clear must leave pitch steady";

    Swing shallow = swing(record(true, false), ref_hz);
    EXPECT_NEAR(shallow.cents_up, 7.0, 1.0);
    EXPECT_NEAR(shallow.cents_down, -7.0, 1.0);

    Swing deep = swing(record(true, true), ref_hz);
    EXPECT_NEAR(deep.cents_up, 14.0, 1.5);
    EXPECT_NEAR(deep.cents_down, -14.0, 1.5);

    // Peak to trough is half an LFO period
    const double half_period = Opl3::kSampleHz / 6.1 / 2.0;
    EXPECT_NEAR(double(std::labs(shallow.trough_frame - shallow.peak_frame)), half_period, half_period * 0.05);
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
