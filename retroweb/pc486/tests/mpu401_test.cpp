// GoogleTest suite for the SB16's MPU-401 MIDI interface (UART mode only).

#include <gtest/gtest.h>

#include "mpu401.h"

namespace {

using pc486::Mpu401;

TEST(Mpu401Test, OwnsOnlyItsTwoPortsAtTheDefaultBase) {
    Mpu401 mpu;
    EXPECT_TRUE(mpu.owns(0x330));
    EXPECT_TRUE(mpu.owns(0x331));
    EXPECT_FALSE(mpu.owns(0x332));
    EXPECT_FALSE(mpu.owns(0x32F));
}

TEST(Mpu401Test, OwnsItsTwoPortsAtAnAlternateJumperedBase) {
    Mpu401 mpu(0x300);
    EXPECT_TRUE(mpu.owns(0x300));
    EXPECT_TRUE(mpu.owns(0x301));
    EXPECT_FALSE(mpu.owns(0x330));
}

TEST(Mpu401Test, ResetDetectionHandshakePollsStatusThenReadsAck) {
    // SBPG ch.5 detection: write 0FFh, poll status 40h clear then 80h clear, read 0FEh.
    Mpu401 mpu;
    mpu.reset();
    mpu.out(0x331, 0xFF);
    uint8_t status = mpu.in(0x331);
    EXPECT_EQ(status & 0x40, 0x00);  // ready for output
    EXPECT_EQ(status & 0x80, 0x00);  // ack is available to read
    EXPECT_EQ(mpu.in(0x330), 0xFE);  // Command Acknowledge
}

TEST(Mpu401Test, EnterUartModeSetsFlagAndAcknowledges) {
    Mpu401 mpu;
    mpu.reset();
    EXPECT_FALSE(mpu.uart_mode());
    mpu.out(0x331, 0x3F);
    EXPECT_TRUE(mpu.uart_mode());
    EXPECT_EQ(mpu.in(0x331) & 0x80, 0x00);  // ack ready
    EXPECT_EQ(mpu.in(0x330), 0xFE);
}

TEST(Mpu401Test, UnrecognizedCommandProducesNoAcknowledge) {
    Mpu401 mpu;
    mpu.reset();
    mpu.out(0x331, 0x01);  // not 0FFh or 03Fh -- an Intelligent-mode command this card doesn't support
    EXPECT_EQ(mpu.in(0x331) & 0x80, 0x80);  // still "no input available" -- nothing came of it
}

TEST(Mpu401Test, ResetExitsUartMode) {
    Mpu401 mpu;
    mpu.reset();
    mpu.out(0x331, 0x3F);
    ASSERT_TRUE(mpu.uart_mode());
    mpu.in(0x330);  // drain the Enter-UART ack first
    mpu.out(0x331, 0xFF);
    EXPECT_FALSE(mpu.uart_mode());
    EXPECT_EQ(mpu.in(0x330), 0xFE);  // Reset acknowledges too
}

TEST(Mpu401Test, UartModeDataWritesAreAcceptedAndInputNeverArrives) {
    Mpu401 mpu;
    mpu.reset();
    mpu.out(0x331, 0x3F);  // Enter UART mode
    mpu.in(0x330);         // drain its ack
    mpu.out(0x330, 0x90);  // MIDI Note On, status byte -- goes nowhere, nothing attached
    mpu.out(0x330, 0x40);
    mpu.out(0x330, 0x7F);
    // No MIDI device is attached, so input never becomes available.
    EXPECT_EQ(mpu.in(0x331) & 0x80, 0x80);
}

TEST(Mpu401Test, DataPortReadWithNoAckPendingDoesNotFabricateOne) {
    Mpu401 mpu;
    mpu.reset();
    EXPECT_NE(mpu.in(0x330), 0xFE);
    EXPECT_NE(mpu.in(0x330), 0xFE);  // repeated reads don't invent one either
}

}  // namespace
