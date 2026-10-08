#include <gtest/gtest.h>

#include "i8042.h"

#include <vector>

namespace {

using ibmpcat::I8042;

constexpr double kHz = 8000000.0;

class I8042Test : public ::testing::Test {
protected:
    I8042 kbc;
    uint64_t now = 0;

    void SetUp() override { kbc.reset(); }

    void Run(double seconds) {
        uint64_t end = now + uint64_t(seconds * kHz);
        while (now < end) {
            now += 80;  // 10 us
            kbc.tick(now, kHz);
        }
    }
    // Lets one keyboard frame cross the link.
    void Frame() { Run(I8042::kFrameSeconds + 20e-6); }
    bool Obf() { return (kbc.in(0x64) & 0x01) != 0; }
    std::vector<uint8_t> Drain(double seconds = 0.05) {
        std::vector<uint8_t> out;
        uint64_t end = now + uint64_t(seconds * kHz);
        while (now < end) {
            if (Obf()) out.push_back(kbc.in(0x60));
            now += 80;
            kbc.tick(now, kHz);
        }
        if (Obf()) out.push_back(kbc.in(0x60));
        return out;
    }
};

TEST_F(I8042Test, ResetDeliversUnsolicitedKeyboardBatByte) {
    EXPECT_FALSE(Obf()) << "the BAT byte still has to cross the link";
    Frame();
    EXPECT_TRUE(Obf());
    EXPECT_EQ(kbc.in(0x60), 0xAA);
}

TEST_F(I8042Test, A20OpenAtResetAndAfterSelfTest) {
    EXPECT_TRUE(kbc.a20_enabled()) << "the 286's first fetch at FFFFF0h needs A20 high";
    EXPECT_FALSE(kbc.reset_requested());
    kbc.out(0x64, 0xD1);
    kbc.out(0x60, 0x01);
    ASSERT_FALSE(kbc.a20_enabled());
    kbc.out(0x64, 0xAA);
    EXPECT_TRUE(kbc.a20_enabled());
}

TEST_F(I8042Test, WriteOutputPortEnablesA20) {
    kbc.out(0x64, 0xD1);
    kbc.out(0x60, 0x03);
    EXPECT_TRUE(kbc.a20_enabled());
    EXPECT_FALSE(kbc.reset_requested());
}

TEST_F(I8042Test, OutputPortBitZeroLowTriggersReset) {
    kbc.out(0x64, 0xD1);
    kbc.out(0x60, 0x00);
    EXPECT_TRUE(kbc.reset_requested());
    kbc.clear_reset_request();
    EXPECT_FALSE(kbc.reset_requested());
}

TEST_F(I8042Test, PulseOutputLineZeroCommandAlsoTriggersReset) {
    kbc.out(0x64, 0xFE);
    EXPECT_TRUE(kbc.reset_requested());
}

TEST_F(I8042Test, SelfTestRespondsWithFiftyFiveAtOnce) {
    kbc.out(0x64, 0xAA);
    EXPECT_TRUE(Obf()) << "the controller answers for itself, no link delay";
    EXPECT_EQ(kbc.in(0x60), 0x55);
    EXPECT_FALSE(Obf());
}

TEST_F(I8042Test, CommandByteReadWriteRoundTrip) {
    kbc.out(0x64, 0x60);
    kbc.out(0x60, 0x45);
    kbc.out(0x64, 0x20);
    EXPECT_EQ(kbc.in(0x60), 0x45);
}

TEST_F(I8042Test, StatusBitFourIsTheKeylockNotTheDisableCommand) {
    EXPECT_TRUE(kbc.in(0x64) & 0x10) << "1 = keylock open";
    kbc.out(0x64, 0xAD);
    EXPECT_TRUE(kbc.in(0x64) & 0x10);
}

TEST_F(I8042Test, DisableAndEnableKeyboardDriveCommandByteBitFour) {
    kbc.out(0x64, 0xAD);
    kbc.out(0x64, 0x20);
    EXPECT_EQ(kbc.in(0x60) & 0x10, 0x10);
    kbc.out(0x64, 0xAE);
    kbc.out(0x64, 0x20);
    EXPECT_EQ(kbc.in(0x60) & 0x10, 0x00);
}

TEST_F(I8042Test, ReadInputPortReportsTheFactoryJumpers) {
    kbc.out(0x64, 0xC0);
    uint8_t v = kbc.in(0x60);
    EXPECT_EQ(v & 0x80, 0x80) << "keylock open";
    EXPECT_EQ(v & 0x40, 0x00) << "colour display";
    EXPECT_EQ(v & 0x20, 0x20) << "no manufacturing jumper";
    EXPECT_EQ(v & 0x10, 0x10) << "512KB on the system board";
}

TEST_F(I8042Test, ReadTestInputsSeesTheClockHeldLowWhileInhibited) {
    kbc.out(0x64, 0xE0);
    EXPECT_EQ(kbc.in(0x60), 0x03);
    kbc.out(0x64, 0xAD);
    kbc.out(0x64, 0xE0);
    EXPECT_EQ(kbc.in(0x60), 0x02);
}

TEST_F(I8042Test, ScancodeSetsIrq1OnlyWhenEnabledInCommandByte) {
    Drain();
    kbc.inject_scancode(0x1E);
    Frame();
    EXPECT_FALSE(kbc.irq1_pending());
    EXPECT_EQ(kbc.in(0x60), 0x1E);

    kbc.out(0x64, 0x60);
    kbc.out(0x60, 0x01);
    kbc.inject_scancode(0x1F);
    Frame();
    EXPECT_TRUE(kbc.irq1_pending());
    EXPECT_EQ(kbc.in(0x60), 0x1F);
    EXPECT_FALSE(kbc.irq1_pending());
}

TEST_F(I8042Test, ResetCommandGetsAckThenBatByteOnSeparateReads) {
    // Bochs rombios.c keyboard_init sends 0xFF, expects 0xFA, then 0xAA.
    Drain();
    kbc.out(0x60, 0xFF);
    EXPECT_EQ(Drain(), (std::vector<uint8_t>{0xFA, 0xAA}));
}

TEST_F(I8042Test, BytesWaitInTheKeyboardInsteadOfOverwritingOneAnother) {
    Drain();
    for (uint8_t b : {0xE0, 0x48, 0xE0, 0xC8, 0x1E, 0x9E}) kbc.inject_scancode(b);
    Frame();
    EXPECT_EQ(kbc.in(0x60), 0xE0);
    EXPECT_FALSE(Obf()) << "the next byte takes another frame";
    EXPECT_EQ(Drain(), (std::vector<uint8_t>{0x48, 0xE0, 0xC8, 0x1E, 0x9E}));
}

TEST_F(I8042Test, Irq1DropsBetweenBytesSoEveryByteIsANewEdge) {
    Drain();
    kbc.out(0x64, 0x60);
    kbc.out(0x60, 0x01);
    kbc.inject_scancode(0x1E);
    kbc.inject_scancode(0x9E);
    Frame();
    ASSERT_TRUE(kbc.irq1_pending());
    kbc.in(0x60);
    kbc.tick(now += 80, kHz);
    EXPECT_FALSE(kbc.irq1_pending());
    Frame();
    EXPECT_TRUE(kbc.irq1_pending());
}

TEST_F(I8042Test, InhibitedKeyboardHoldsKeysUntilEnabled) {
    // The Bochs INT 9 handler brackets its read with ADh/AEh.
    Drain();
    kbc.out(0x64, 0xAD);
    kbc.inject_scancode(0x1E);
    Run(0.01);
    EXPECT_FALSE(Obf());
    kbc.out(0x64, 0xAE);
    EXPECT_EQ(Drain(), std::vector<uint8_t>{0x1E});
}

TEST_F(I8042Test, CommandResponsesGoAheadOfBufferedKeys) {
    Drain();
    kbc.out(0x64, 0xAD);
    kbc.inject_scancode(0x1E);
    kbc.out(0x60, 0xEE);
    EXPECT_EQ(Drain(), std::vector<uint8_t>{0xEE}) << "a response isn't held off by the inhibit";
    kbc.out(0x64, 0xAE);
    EXPECT_EQ(Drain(), std::vector<uint8_t>{0x1E});
}

TEST_F(I8042Test, SeventeenthByteBecomesTheOverrunCode) {
    Drain();
    kbc.out(0x64, 0xAD);
    for (int i = 0; i < 20; ++i) kbc.inject_scancode(uint8_t(0x10 + i));
    kbc.out(0x64, 0xAE);
    std::vector<uint8_t> out = Drain(0.1);
    ASSERT_EQ(out.size(), 17u);
    EXPECT_EQ(out[15], 0x1F);
    EXPECT_EQ(out[16], 0xFF);
}

TEST_F(I8042Test, EchoAnswersEchoNotAck) {
    Drain();
    kbc.out(0x60, 0xEE);
    EXPECT_EQ(Drain(), std::vector<uint8_t>{0xEE});
}

TEST_F(I8042Test, SetLedsAcksTheCommandAndItsArgument) {
    Drain();
    kbc.out(0x60, 0xED);
    kbc.out(0x60, 0x05);
    EXPECT_EQ(Drain(), (std::vector<uint8_t>{0xFA, 0xFA}));
    EXPECT_EQ(kbc.leds(), 0x05);
}

TEST_F(I8042Test, ACommandInPlaceOfAnArgumentRunsAsACommand) {
    Drain();
    kbc.out(0x60, 0xED);
    kbc.out(0x60, 0xEE);
    EXPECT_EQ(Drain(), (std::vector<uint8_t>{0xFA, 0xEE}));
    EXPECT_EQ(kbc.leds(), 0x00);
}

TEST_F(I8042Test, ReadIdAnswersTheTranslatedEnhancedKeyboardId) {
    Drain();
    kbc.out(0x60, 0xF2);
    EXPECT_EQ(Drain(), (std::vector<uint8_t>{0xFA, 0xAB, 0x41}));
}

TEST_F(I8042Test, DefaultDisableStopsScanningAndEnableRestartsIt) {
    Drain();
    kbc.inject_scancode(0x1E);
    kbc.out(0x60, 0xF5);
    EXPECT_EQ(Drain(), std::vector<uint8_t>{0xFA}) << "F5 clears the buffer";
    EXPECT_FALSE(kbc.scanning());
    kbc.inject_scancode(0x1F);
    EXPECT_TRUE(Drain().empty());
    kbc.out(0x60, 0xF4);
    EXPECT_EQ(Drain(), std::vector<uint8_t>{0xFA});
    kbc.inject_scancode(0x20);
    EXPECT_EQ(Drain(), std::vector<uint8_t>{0x20});
}

TEST_F(I8042Test, ResendRepeatsTheLastByteSent) {
    Drain();
    kbc.inject_scancode(0x1E);
    Drain();
    kbc.out(0x60, 0xFE);
    EXPECT_EQ(Drain(), std::vector<uint8_t>{0x1E});
}

TEST_F(I8042Test, AnUnknownCommandAsksForAResend) {
    Drain();
    kbc.out(0x60, 0x12);
    EXPECT_EQ(Drain(), std::vector<uint8_t>{0xFE});
}

// --- typematic repeat ---

class TypematicTest : public I8042Test {
protected:
    void SetUp() override { I8042Test::SetUp(); Drain(); }
    std::vector<uint8_t> RunTo(double seconds) { return Drain(seconds - double(now) / kHz); }
};

TEST_F(TypematicTest, AHeldKeyRepeatsAfterHalfASecondAtTenPointNineCps) {
    kbc.inject_scancode(0x1E);
    EXPECT_EQ(Drain(0.01), std::vector<uint8_t>{0x1E});
    EXPECT_TRUE(RunTo(0.54).empty()) << "nothing before the 500 ms delay";
    EXPECT_EQ(RunTo(0.57), std::vector<uint8_t>{0x1E});
    auto second = RunTo(1.57);
    EXPECT_GE(second.size(), 10u);
    EXPECT_LE(second.size(), 11u) << "one every 91.7 ms";
    kbc.inject_scancode(0x9E);
    EXPECT_EQ(Drain(0.01), std::vector<uint8_t>{0x9E});
    EXPECT_TRUE(RunTo(3.0).empty()) << "the break ends the repeat";
}

TEST_F(TypematicTest, OnlyTheMostRecentlyPressedKeyRepeats) {
    kbc.inject_scancode(0x1E);
    kbc.inject_scancode(0x1F);
    Drain(0.01);
    EXPECT_EQ(RunTo(0.6), std::vector<uint8_t>{0x1F});
    kbc.inject_scancode(0x9F);
    Drain(0.01);
    EXPECT_TRUE(RunTo(2.0).empty()) << "releasing it does not hand the repeat back to A";
}

TEST_F(TypematicTest, AGreyKeyRepeatsWithItsE0Prefix) {
    kbc.inject_scancode(0xE0);
    kbc.inject_scancode(0x48);
    Drain(0.01);
    EXPECT_EQ(RunTo(0.6), (std::vector<uint8_t>{0xE0, 0x48}));
    kbc.inject_scancode(0xE0);
    kbc.inject_scancode(0xC8);
    Drain(0.01);
    EXPECT_TRUE(RunTo(2.0).empty());
}

TEST_F(TypematicTest, SetTypematicRateChangesTheDelayAndTheRate) {
    kbc.out(0x60, 0xF3);
    kbc.out(0x60, 0x00);  // 250 ms, 30 cps
    EXPECT_EQ(Drain(0.01), (std::vector<uint8_t>{0xFA, 0xFA}));
    EXPECT_EQ(kbc.typematic_byte(), 0x00);
    double t0 = double(now) / kHz;
    kbc.inject_scancode(0x1E);
    Drain(0.01);
    EXPECT_EQ(RunTo(t0 + 0.26).size(), 1u);
    auto out = RunTo(t0 + 1.26);
    EXPECT_GE(out.size(), 29u);
    EXPECT_LE(out.size(), 31u);

    kbc.out(0x60, 0xFF);
    Drain(0.01);
    EXPECT_EQ(kbc.typematic_byte(), 0x2B);
}

TEST_F(TypematicTest, PauseNeverRepeats) {
    for (uint8_t b : {0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5}) kbc.inject_scancode(b);
    Drain(0.02);
    EXPECT_TRUE(RunTo(2.0).empty());
}

TEST_F(TypematicTest, AHeldKeyStoresOnlyItsFirstMakeWhileInhibited) {
    kbc.out(0x64, 0xAD);
    kbc.inject_scancode(0x1E);
    Run(1.5);
    kbc.inject_scancode(0x9E);
    kbc.out(0x64, 0xAE);
    EXPECT_EQ(Drain(0.01), (std::vector<uint8_t>{0x1E, 0x9E}));
}

TEST_F(TypematicTest, AReleaseWhileInhibitedStillEndsTheRepeat) {
    kbc.inject_scancode(0x1E);
    Drain(0.01);
    kbc.out(0x64, 0xAD);
    kbc.inject_scancode(0x9E);
    kbc.out(0x64, 0xAE);
    EXPECT_EQ(RunTo(2.0), std::vector<uint8_t>{0x9E});
}

TEST_F(TypematicTest, InterleavedGreyKeyBreaksStillEndTheRepeat) {
    for (uint8_t b : {0xE0, 0x48, 0xE0, 0x4D}) kbc.inject_scancode(b);
    Drain(0.01);
    for (uint8_t b : {0xE0, 0xE0, 0xC8, 0xCD}) kbc.inject_scancode(b);
    Drain(0.01);
    EXPECT_TRUE(RunTo(2.0).empty());
}

}  // namespace
