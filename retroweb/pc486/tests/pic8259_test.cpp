// GoogleTest suite for the 8259A PIC: ICW1-4 initialization, OCW1 masking,
// OCW2 EOI, and the acknowledge()/raise()/priority behavior the chipset
// relies on. Reference: Intel 8259A data sheet, "Programming" section.

#include <gtest/gtest.h>

#include "pic8259.h"

namespace {

using pc486::Pic8259;

// A genuine AT BIOS's real init sequence for the master (vector base 0x08,
// cascaded, edge-triggered, 8086 mode): ICW1=0x11 (edge, cascade, ICW4
// needed), ICW2=0x08 (vector base), ICW3=0x04 (slave on IR2), ICW4=0x01
// (8086 mode, normal EOI).
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
    // Fully nested mode: IR2 is in service, so the lower-priority IR4 waits
    // -- the chip does not even assert INT for it (Intel 8259A data sheet,
    // "Fully Nested Mode").
    EXPECT_FALSE(pic.has_interrupt());
    // Non-specific EOI clears the highest-priority in-service bit (IR2), and
    // IR4 is granted immediately after.
    pic.out(0x20, 0x20);
    EXPECT_TRUE(pic.has_interrupt());
    EXPECT_EQ(pic.acknowledge(), 0x08 + 4);
    pic.out(0x20, 0x20);
    EXPECT_FALSE(pic.has_interrupt());
}

TEST(Pic8259Test, ALineInServiceCannotInterruptItselfUntilEoi) {
    // The case a device that re-asserts its own line inside its own handler
    // depends on: the AUX port raises IRQ12 again for byte 2 of a mouse
    // packet while the firmware's INT 74h handler -- which runs with
    // interrupts enabled -- is still assembling byte 1. Without the
    // in-service block the handler re-enters itself and the packet comes out
    // scrambled (PC486_REVIEW.md §13).
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
    // The other half of fully nested mode: a *higher* priority request is
    // granted while a lower one is still in service, which is what makes the
    // timer tick through a slow device handler.
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

}  // namespace
