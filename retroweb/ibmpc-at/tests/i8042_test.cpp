#include <gtest/gtest.h>

#include "i8042.h"

namespace {

using ibmpcat::I8042;

TEST(I8042Test, ResetDeliversUnsolicitedKeyboardBatByte) {
    // Unsolicited 0xAA after the keyboard's power-on self-test; BIOS POST waits for it.
    I8042 kbc;
    kbc.reset();
    EXPECT_TRUE(kbc.in(0x64) & 0x01);
    EXPECT_EQ(kbc.in(0x60), 0xAA);
}

TEST(I8042Test, A20OpenAtResetAndAfterSelfTest) {
    I8042 kbc;
    kbc.reset();
    EXPECT_TRUE(kbc.a20_enabled()) << "the 286's first fetch at FFFFF0h needs A20 high";
    EXPECT_FALSE(kbc.reset_requested());
    kbc.out(0x64, 0xD1);
    kbc.out(0x60, 0x01);  // close A20, keep RESET released
    ASSERT_FALSE(kbc.a20_enabled());
    kbc.out(0x64, 0xAA);
    EXPECT_TRUE(kbc.a20_enabled());
}

TEST(I8042Test, WriteOutputPortEnablesA20) {
    I8042 kbc;
    kbc.reset();
    kbc.out(0x64, 0xD1);  // write output port
    kbc.out(0x60, 0x03);  // bit1 set -> A20 enabled; bit0 set -> reset line held high (inactive)
    EXPECT_TRUE(kbc.a20_enabled());
    EXPECT_FALSE(kbc.reset_requested());
}

TEST(I8042Test, OutputPortBitZeroLowTriggersReset) {
    I8042 kbc;
    kbc.reset();
    kbc.out(0x64, 0xD1);
    kbc.out(0x60, 0x00);  // bit0 clear -> reset line pulsed low
    EXPECT_TRUE(kbc.reset_requested());
    kbc.clear_reset_request();
    EXPECT_FALSE(kbc.reset_requested());
}

TEST(I8042Test, PulseOutputLineZeroCommandAlsoTriggersReset) {
    I8042 kbc;
    kbc.reset();
    kbc.out(0x64, 0xFE);
    EXPECT_TRUE(kbc.reset_requested());
}

TEST(I8042Test, SelfTestRespondsWithFiftyFive) {
    I8042 kbc;
    kbc.reset();
    kbc.out(0x64, 0xAA);
    EXPECT_TRUE(kbc.in(0x64) & 0x01);  // output buffer full
    EXPECT_EQ(kbc.in(0x60), 0x55);
    EXPECT_FALSE(kbc.in(0x64) & 0x01);  // reading 0x60 drains the buffer
}

TEST(I8042Test, CommandByteReadWriteRoundTrip) {
    I8042 kbc;
    kbc.reset();
    kbc.out(0x64, 0x60);  // write command byte
    kbc.out(0x60, 0x45);  // IRQ1 enabled, translation on (bit examples)
    kbc.out(0x64, 0x20);  // read command byte back
    EXPECT_EQ(kbc.in(0x60), 0x45);
}

TEST(I8042Test, ScancodeSetsIrq1OnlyWhenEnabledInCommandByte) {
    I8042 kbc;
    kbc.reset();
    kbc.inject_scancode(0x1E);  // command byte's IRQ1-enable bit not yet set
    EXPECT_FALSE(kbc.irq1_pending());
    EXPECT_EQ(kbc.in(0x60), 0x1E);  // byte still delivered to the data port

    kbc.out(0x64, 0x60);
    kbc.out(0x60, 0x01);  // bit0 = enable IRQ1
    kbc.inject_scancode(0x1F);
    EXPECT_TRUE(kbc.irq1_pending());
    EXPECT_EQ(kbc.in(0x60), 0x1F);
    kbc.clear_irq1();
    EXPECT_FALSE(kbc.irq1_pending());
}

TEST(I8042Test, ResetCommandGetsAckThenBatByteOnSeparateReads) {
    // BIOS keyboard POST (Bochs rombios.c) sends 0xFF, expects 0xFA, then 0xAA as a separate byte.
    I8042 kbc;
    kbc.reset();
    kbc.in(0x60);  // drain the power-on BAT byte first
    kbc.out(0x60, 0xFF);  // RESET command
    EXPECT_EQ(kbc.in(0x60), 0xFA);
    EXPECT_TRUE(kbc.in(0x64) & 0x01);  // a second byte is already waiting
    EXPECT_EQ(kbc.in(0x60), 0xAA);
    EXPECT_FALSE(kbc.in(0x64) & 0x01);
}

TEST(I8042Test, DisabledKeyboardDropsScancodes) {
    I8042 kbc;
    kbc.reset();
    kbc.in(0x60);  // drain the keyboard's own unsolicited post-reset BAT byte first
    kbc.out(0x64, 0xAD);  // disable keyboard
    kbc.inject_scancode(0x1E);
    EXPECT_FALSE(kbc.in(0x64) & 0x01);  // nothing delivered
}

}  // namespace
