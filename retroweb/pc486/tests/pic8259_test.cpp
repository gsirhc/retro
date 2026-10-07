// GoogleTest suite for the 8259A PIC (Intel 8259A data sheet, Programming).

#include <gtest/gtest.h>

#include "pic8259.h"

namespace {

using pc486::Pic8259;

// AT BIOS master init: ICW1=0x11, ICW2=0x08, ICW3=0x04 (slave on IR2), ICW4=0x01.
void InitMaster(Pic8259 &pic) {
    pic.out(0x20, 0x11);
    pic.out(0x21, 0x08);
    pic.out(0x21, 0x04);
    pic.out(0x21, 0x01);
}

TEST(Pic8259Test, ResetMasksEverything) {
    Pic8259 pic(0x20);
    pic.reset();
    EXPECT_EQ(pic.in(0x21), 0xFF);
    EXPECT_FALSE(pic.has_interrupt());
}

TEST(Pic8259Test, InitSequenceSetsVectorBaseAndUnmasksViaOcw1) {
    Pic8259 pic(0x20);
    InitMaster(pic);
    pic.out(0x21, 0x00);  // OCW1: unmask everything
    pic.raise(0);
    EXPECT_TRUE(pic.has_interrupt());
    EXPECT_EQ(pic.acknowledge(), 0x08);  // vector_base(8) + IR0
}

TEST(Pic8259Test, MaskedLineDoesNotInterrupt) {
    Pic8259 pic(0x20);
    InitMaster(pic);
    pic.out(0x21, 0xFF);  // OCW1: mask everything
    pic.raise(0);
    EXPECT_FALSE(pic.has_interrupt());
}

TEST(Pic8259Test, LowerIrqNumberHasHigherPriority) {
    Pic8259 pic(0x20);
    InitMaster(pic);
    pic.out(0x21, 0x00);
    pic.raise(3);
    pic.raise(1);
    EXPECT_EQ(pic.acknowledge(), 0x08 + 1);  // IR1 beats IR3
}

TEST(Pic8259Test, AcknowledgeClearsIrrAndSetsIsrUntilEoi) {
    Pic8259 pic(0x20);
    InitMaster(pic);
    pic.out(0x21, 0x00);
    pic.raise(2);
    pic.raise(4);
    EXPECT_EQ(pic.acknowledge(), 0x08 + 2);
    // Fully nested mode (8259A data sheet): IR4 waits behind IR2 in service.
    EXPECT_FALSE(pic.has_interrupt());
    // Non-specific EOI clears IR2, then IR4 is granted.
    pic.out(0x20, 0x20);
    EXPECT_TRUE(pic.has_interrupt());
    EXPECT_EQ(pic.acknowledge(), 0x08 + 4);
    pic.out(0x20, 0x20);
    EXPECT_FALSE(pic.has_interrupt());
}

TEST(Pic8259Test, ALineInServiceCannotInterruptItselfUntilEoi) {
    // A device re-asserting its line inside its handler (AUX IRQ12 mid-packet)
    // must not re-enter it while in service (PC486_REVIEW.md §13).
    Pic8259 pic(0x20);
    InitMaster(pic);
    pic.out(0x21, 0x00);
    pic.raise(4);
    EXPECT_EQ(pic.acknowledge(), 0x08 + 4);
    pic.raise(4);                       // the device has another byte ready
    EXPECT_FALSE(pic.has_interrupt());  // ...and it waits for EOI
    pic.out(0x20, 0x20);
    EXPECT_TRUE(pic.has_interrupt());
    EXPECT_EQ(pic.acknowledge(), 0x08 + 4);
}

TEST(Pic8259Test, HigherPriorityLinePreemptsOneInService) {
    // Fully nested: a higher-priority request preempts a lower one in service.
    Pic8259 pic(0x20);
    InitMaster(pic);
    pic.out(0x21, 0x00);
    pic.raise(6);
    EXPECT_EQ(pic.acknowledge(), 0x08 + 6);
    pic.raise(0);
    EXPECT_TRUE(pic.has_interrupt());
    EXPECT_EQ(pic.acknowledge(), 0x08 + 0);
    pic.out(0x20, 0x20);  // EOI clears IR0, the highest in service
    EXPECT_FALSE(pic.has_interrupt());
    pic.raise(7);
    EXPECT_FALSE(pic.has_interrupt());  // IR6 is still in service and outranks IR7
    pic.out(0x20, 0x20);
    EXPECT_TRUE(pic.has_interrupt());
}

TEST(Pic8259Test, AutoEoiSetsNoInServiceBitSoNothingIsBlocked) {
    Pic8259 pic(0x20);
    pic.out(0x20, 0x11);
    pic.out(0x21, 0x08);
    pic.out(0x21, 0x04);
    pic.out(0x21, 0x03);  // ICW4: 8086 mode + auto-EOI
    pic.out(0x21, 0x00);
    pic.raise(4);
    EXPECT_EQ(pic.acknowledge(), 0x08 + 4);
    pic.raise(4);
    EXPECT_TRUE(pic.has_interrupt());  // no ISR bit was ever set
}

TEST(Pic8259Test, SpecificEoiUnblocksOnlyTheNamedLine) {
    Pic8259 pic(0x20);
    InitMaster(pic);
    pic.out(0x21, 0x00);
    pic.raise(1);
    EXPECT_EQ(pic.acknowledge(), 0x08 + 1);
    pic.raise(0);
    EXPECT_EQ(pic.acknowledge(), 0x08 + 0);
    pic.raise(5);
    EXPECT_FALSE(pic.has_interrupt());
    pic.out(0x20, 0x60);  // specific EOI for IR0
    EXPECT_FALSE(pic.has_interrupt());  // IR1 is still in service
    pic.out(0x20, 0x61);  // specific EOI for IR1
    EXPECT_TRUE(pic.has_interrupt());
}

TEST(Pic8259Test, EdgeTriggeredRaiseThenLowerClearsPendingRequest) {
    Pic8259 pic(0x20);
    InitMaster(pic);
    pic.out(0x21, 0x00);
    pic.raise(5);
    pic.lower(5);
    EXPECT_FALSE(pic.has_interrupt());
}

TEST(Pic8259Test, ReadIrrVsIsrViaOcw3) {
    Pic8259 pic(0x20);
    InitMaster(pic);
    pic.out(0x21, 0x00);
    pic.raise(0);
    pic.out(0x20, 0x0A);  // OCW3: read IRR next
    EXPECT_EQ(pic.in(0x20), 0x01);
    pic.acknowledge();
    pic.out(0x20, 0x0B);  // OCW3: read ISR next
    EXPECT_EQ(pic.in(0x20), 0x01);
}

TEST(Pic8259Test, PollReturnsTheHighestRequestAndAcknowledgesIt) {
    // 8259A data sheet, The Poll Command: OCW3 P=1 makes the next read return I (bit 7) and the level.
    Pic8259 pic(0x20);
    InitMaster(pic);
    pic.out(0x21, 0x00);
    pic.raise(6);
    pic.raise(3);
    pic.out(0x20, 0x0C);  // OCW3: poll
    EXPECT_EQ(pic.in(0x20), 0x83);
    pic.out(0x20, 0x0B);  // read ISR
    EXPECT_EQ(pic.in(0x20), 0x08);
    pic.out(0x20, 0x0A);  // read IRR
    EXPECT_EQ(pic.in(0x20), 0x40);
    EXPECT_FALSE(pic.has_interrupt());  // IR6 waits behind IR3 in service
}

TEST(Pic8259Test, PollWithNothingPendingReturnsZero) {
    Pic8259 pic(0x20);
    InitMaster(pic);
    pic.out(0x21, 0x00);
    pic.out(0x20, 0x0C);
    EXPECT_EQ(pic.in(0x20), 0x00);
}

TEST(Pic8259Test, PollIsOneShotAndHonoursTheMask) {
    Pic8259 pic(0x20);
    InitMaster(pic);
    pic.out(0x21, 0x01);  // IR0 masked
    pic.raise(0);
    pic.raise(4);
    pic.out(0x20, 0x0C);
    EXPECT_EQ(pic.in(0x21), 0x84);  // the next read on either port is the poll
    EXPECT_EQ(pic.in(0x21), 0x01);  // then the IMR again
}

}  // namespace
