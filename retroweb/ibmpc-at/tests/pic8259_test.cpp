#include <gtest/gtest.h>

#include "pic8259.h"

namespace {

using ibmpcat::Pic8259;

// AT BIOS master init: ICW1=0x11, ICW2=0x08 (base), ICW3=0x04 (slave on IR2), ICW4=0x01.
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
    uint8_t v1 = pic.acknowledge();
    EXPECT_EQ(v1, 0x08 + 2);
    // IR2 is in service, IR4 is still pending and reported.
    EXPECT_TRUE(pic.has_interrupt());
    EXPECT_EQ(pic.acknowledge(), 0x08 + 4);
    // Non-specific EOI clears the lowest still-in-service bit (IR2 first).
    pic.out(0x20, 0x20);
    pic.out(0x20, 0x20);
    EXPECT_FALSE(pic.has_interrupt());
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
