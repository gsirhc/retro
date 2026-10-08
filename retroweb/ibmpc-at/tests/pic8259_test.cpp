#include <gtest/gtest.h>

#include "pic8259.h"

namespace {

using ibmpcat::Pic8259;

// AT BIOS master init: ICW1=0x11, ICW2=0x08 (base), ICW3=0x04 (slave on IR2), ICW4=0x01.
void InitMaster(Pic8259& pic, uint8_t icw4 = 0x01) {
    pic.out(0x20, 0x11);
    pic.out(0x21, 0x08);
    pic.out(0x21, 0x04);
    pic.out(0x21, icw4);
    pic.out(0x21, 0x00);
}

void Pulse(Pic8259& pic, int irq) {
    pic.set_line(irq, true);
}

int Ack(Pic8259& pic) {
    int level = pic.inta1();
    return pic.inta2(level);
}

TEST(Pic8259Test, ResetMasksEverything) {
    Pic8259 pic(0x20, true);
    pic.reset();
    EXPECT_EQ(pic.in(0x21), 0xFF);
    EXPECT_FALSE(pic.has_interrupt());
}

TEST(Pic8259Test, InitSequenceSetsVectorBase) {
    Pic8259 pic(0x20, true);
    InitMaster(pic);
    Pulse(pic, 0);
    EXPECT_TRUE(pic.has_interrupt());
    EXPECT_EQ(Ack(pic), 0x08);
}

TEST(Pic8259Test, MaskedLineDoesNotInterrupt) {
    Pic8259 pic(0x20, true);
    InitMaster(pic);
    pic.out(0x21, 0xFF);
    Pulse(pic, 0);
    EXPECT_FALSE(pic.has_interrupt());
}

TEST(Pic8259Test, LowerIrqNumberHasHigherPriority) {
    Pic8259 pic(0x20, true);
    InitMaster(pic);
    Pulse(pic, 3);
    Pulse(pic, 1);
    EXPECT_EQ(Ack(pic), 0x08 + 1);
}

TEST(Pic8259Test, InServiceBlocksEqualAndLowerLevels) {
    Pic8259 pic(0x20, true);
    InitMaster(pic);
    Pulse(pic, 2);
    EXPECT_EQ(Ack(pic), 0x08 + 2);
    Pulse(pic, 4);
    EXPECT_FALSE(pic.has_interrupt());
    Pulse(pic, 1);
    EXPECT_EQ(Ack(pic), 0x08 + 1);
    pic.out(0x20, 0x20);  // non-specific EOI clears IR1, the highest in service
    EXPECT_EQ(pic.isr(), 0x04);
    EXPECT_FALSE(pic.has_interrupt());
    pic.out(0x20, 0x20);
    EXPECT_EQ(Ack(pic), 0x08 + 4);
}

TEST(Pic8259Test, EdgeNeedsTheLineToFallBeforeItCountsAgain) {
    Pic8259 pic(0x20, true);
    InitMaster(pic);
    pic.set_line(5, true);
    Ack(pic);
    pic.out(0x20, 0x20);
    EXPECT_FALSE(pic.has_interrupt());
    pic.set_line(5, false);
    pic.set_line(5, true);
    EXPECT_TRUE(pic.has_interrupt());
}

TEST(Pic8259Test, RequestDroppedBeforeInterruptAcknowledgeIsLost) {
    Pic8259 pic(0x20, true);
    InitMaster(pic);
    pic.set_line(5, true);
    pic.set_line(5, false);
    EXPECT_FALSE(pic.has_interrupt());
    EXPECT_EQ(pic.irr(), 0x00);
}

TEST(Pic8259Test, DefaultIr7WhenTheRequestVanishesBeforeInta) {
    Pic8259 pic(0x20, true);
    InitMaster(pic);
    pic.set_line(3, true);
    ASSERT_TRUE(pic.has_interrupt());
    pic.set_line(3, false);
    int level = pic.inta1();
    EXPECT_EQ(level, 7);
    EXPECT_EQ(pic.inta2(level), 0x0F);
    EXPECT_EQ(pic.isr(), 0x00);
}

TEST(Pic8259Test, LevelTriggeredRequestStaysWhileLineIsHigh) {
    Pic8259 pic(0x20, true);
    pic.out(0x20, 0x19);  // LTIM
    pic.out(0x21, 0x08);
    pic.out(0x21, 0x04);
    pic.out(0x21, 0x01);
    pic.out(0x21, 0x00);
    pic.set_line(3, true);
    EXPECT_EQ(Ack(pic), 0x0B);
    EXPECT_EQ(pic.irr(), 0x08);
    pic.out(0x20, 0x20);
    EXPECT_TRUE(pic.has_interrupt());
}

TEST(Pic8259Test, ReadIrrVsIsrViaOcw3) {
    Pic8259 pic(0x20, true);
    InitMaster(pic);
    Pulse(pic, 0);
    pic.out(0x20, 0x0A);
    EXPECT_EQ(pic.in(0x20), 0x01);
    Ack(pic);
    pic.out(0x20, 0x0B);
    EXPECT_EQ(pic.in(0x20), 0x01);
    EXPECT_EQ(pic.in(0x20), 0x01);
}

TEST(Pic8259Test, Icw1ClearsImrIsrAndReadSelect) {
    Pic8259 pic(0x20, true);
    InitMaster(pic);
    Pulse(pic, 1);
    Ack(pic);
    pic.out(0x21, 0xF0);
    pic.out(0x20, 0x0B);
    pic.out(0x20, 0x11);
    EXPECT_EQ(pic.in(0x21), 0x00);
    EXPECT_EQ(pic.isr(), 0x00);
    pic.out(0x21, 0x08);
    pic.out(0x21, 0x04);
    pic.out(0x21, 0x01);
    EXPECT_EQ(pic.in(0x20), 0x00);  // IRR, not ISR
}

TEST(Pic8259Test, Icw1NeedsAFreshEdgeOnALineAlreadyHigh) {
    Pic8259 pic(0x20, true);
    pic.set_line(4, true);
    InitMaster(pic);
    EXPECT_FALSE(pic.has_interrupt());
    pic.set_line(4, false);
    pic.set_line(4, true);
    EXPECT_TRUE(pic.has_interrupt());
}

TEST(Pic8259Test, Icw1WithoutIc4ZeroesIcw4) {
    Pic8259 pic(0x20, true);
    InitMaster(pic, 0x03);
    pic.out(0x20, 0x12);  // single, no ICW4: MCS-80 mode, no AEOI
    pic.out(0x21, 0x08);
    pic.out(0x21, 0x00);
    Pulse(pic, 1);
    EXPECT_EQ(Ack(pic), 0x08);
    EXPECT_EQ(pic.isr(), 0x02);
}

TEST(Pic8259Test, SingleModeSkipsIcw3) {
    Pic8259 pic(0x20, true);
    pic.out(0x20, 0x13);
    pic.out(0x21, 0x20);
    pic.out(0x21, 0x01);  // ICW4, not ICW3
    pic.out(0x21, 0x00);
    Pulse(pic, 2);
    EXPECT_FALSE(pic.cascades(2));
    EXPECT_EQ(Ack(pic), 0x22);
}

TEST(Pic8259Test, AutoEoiLeavesNothingInService) {
    Pic8259 pic(0x20, true);
    InitMaster(pic, 0x03);
    Pulse(pic, 3);
    EXPECT_EQ(Ack(pic), 0x0B);
    EXPECT_EQ(pic.isr(), 0x00);
}

TEST(Pic8259Test, SpecificEoiClearsTheNamedLevel) {
    Pic8259 pic(0x20, true);
    InitMaster(pic);
    Pulse(pic, 3);
    Ack(pic);
    Pulse(pic, 1);
    Ack(pic);
    pic.out(0x20, 0x63);  // specific EOI, IR3
    EXPECT_EQ(pic.isr(), 0x02);
}

TEST(Pic8259Test, PollReturnsTheLevelAndSetsIsr) {
    Pic8259 pic(0x20, true);
    InitMaster(pic);
    pic.out(0x21, 0xFF);
    pic.out(0x21, 0x00);
    Pulse(pic, 5);
    pic.out(0x20, 0x0C);
    EXPECT_EQ(pic.in(0x20), 0x85);
    EXPECT_EQ(pic.isr(), 0x20);
    EXPECT_FALSE(pic.has_interrupt());
}

TEST(Pic8259Test, PollWorksOnTheDataPortAndIsOneShot) {
    Pic8259 pic(0x20, true);
    InitMaster(pic);
    pic.out(0x21, 0x40);
    pic.out(0x20, 0x0C);
    EXPECT_EQ(pic.in(0x21), 0x00);  // no request: I=0
    EXPECT_EQ(pic.in(0x21), 0x40);  // back to IMR
}

TEST(Pic8259Test, PollOverridesStatusRead) {
    Pic8259 pic(0x20, true);
    InitMaster(pic);
    Pulse(pic, 6);
    pic.out(0x20, 0x0E);  // P=1, RR=1, RIS=0
    EXPECT_EQ(pic.in(0x20), 0x86);
    EXPECT_EQ(pic.in(0x20), 0x00);  // IRR read
}

TEST(Pic8259Test, RotateOnNonSpecificEoiMakesTheLevelLowest) {
    Pic8259 pic(0x20, true);
    InitMaster(pic);
    Pulse(pic, 4);
    EXPECT_EQ(Ack(pic), 0x0C);
    pic.out(0x20, 0xA0);
    Pulse(pic, 3);
    Pulse(pic, 5);
    EXPECT_EQ(Ack(pic), 0x0D);  // IR5 now highest
}

TEST(Pic8259Test, SetPriorityPicksTheBottomLevel) {
    Pic8259 pic(0x20, true);
    InitMaster(pic);
    pic.out(0x20, 0xC5);  // IR5 bottom, IR6 top
    Pulse(pic, 0);
    Pulse(pic, 6);
    EXPECT_EQ(Ack(pic), 0x0E);
}

TEST(Pic8259Test, RotateOnSpecificEoi) {
    Pic8259 pic(0x20, true);
    InitMaster(pic);
    Pulse(pic, 2);
    Ack(pic);
    pic.out(0x20, 0xE2);
    EXPECT_EQ(pic.isr(), 0x00);
    Pulse(pic, 1);
    Pulse(pic, 3);
    EXPECT_EQ(Ack(pic), 0x0B);
}

TEST(Pic8259Test, RotateInAutoEoiMode) {
    Pic8259 pic(0x20, true);
    InitMaster(pic, 0x03);
    pic.out(0x20, 0x80);
    Pulse(pic, 1);
    EXPECT_EQ(Ack(pic), 0x09);
    Pulse(pic, 0);
    Pulse(pic, 2);
    EXPECT_EQ(Ack(pic), 0x0A);  // IR1 bottom, IR2 top
    pic.out(0x20, 0x00);  // clear rotate in AEOI
    Pulse(pic, 3);
    EXPECT_EQ(Ack(pic), 0x0B);  // IR2 still bottom from the last rotation: IR3 top
}

TEST(Pic8259Test, SpecialMaskModeEnablesLowerLevels) {
    Pic8259 pic(0x20, true);
    InitMaster(pic);
    Pulse(pic, 2);
    Ack(pic);
    pic.out(0x20, 0x68);  // set SMM
    pic.out(0x21, 0x04);  // mask IR2, which is in service
    Pulse(pic, 5);
    EXPECT_EQ(Ack(pic), 0x0D);
    pic.out(0x20, 0x20);  // non-specific EOI skips masked IR2
    EXPECT_EQ(pic.isr(), 0x04);
    pic.out(0x20, 0x48);  // reset SMM
    Pulse(pic, 6);
    EXPECT_FALSE(pic.has_interrupt());
}

TEST(Pic8259Test, SpecialFullyNestedLetsTheSameSlaveInputInterruptAgain) {
    Pic8259 pic(0x20, true);
    InitMaster(pic, 0x11);
    pic.set_line(2, true);
    Ack(pic);
    pic.set_line(2, false);
    pic.set_line(2, true);
    EXPECT_TRUE(pic.has_interrupt());
    pic.set_line(5, true);
    pic.set_line(2, false);
    EXPECT_FALSE(pic.has_interrupt());  // IR2 in service still blocks lower IR5
}

TEST(Pic8259Test, NormalNestingLocksOutTheSlaveInputInService) {
    Pic8259 pic(0x20, true);
    InitMaster(pic);
    pic.set_line(2, true);
    Ack(pic);
    pic.set_line(2, false);
    pic.set_line(2, true);
    EXPECT_FALSE(pic.has_interrupt());
}

TEST(Pic8259Test, CascadeFollowsIcw3AndSpPin) {
    Pic8259 master(0x20, true);
    InitMaster(master);
    EXPECT_TRUE(master.cascades(2));
    EXPECT_FALSE(master.cascades(3));
    Pic8259 slave(0xA0, false);
    slave.out(0xA0, 0x11);
    slave.out(0xA1, 0x70);
    slave.out(0xA1, 0x02);
    slave.out(0xA1, 0x01);
    EXPECT_FALSE(slave.cascades(2));
    EXPECT_EQ(slave.slave_id(), 2);
}

TEST(Pic8259Test, BufferedModeTakesMasterSlaveFromIcw4) {
    Pic8259 pic(0xA0, false);
    pic.out(0xA0, 0x11);
    pic.out(0xA1, 0x70);
    pic.out(0xA1, 0x04);
    pic.out(0xA1, 0x0D);  // BUF, M/S=1
    EXPECT_TRUE(pic.cascades(2));
}

TEST(Pic8259Test, Mcs80ModeVectorIsTheCallsLowAddressByte) {
    Pic8259 pic(0x20, true);
    pic.out(0x20, 0xF6);  // A7-A5=111, ADI=1 (interval 4), single, no ICW4
    pic.out(0x21, 0x00);
    pic.out(0x21, 0x00);
    Pulse(pic, 3);
    EXPECT_EQ(Ack(pic), 0xEC);
    pic.out(0x20, 0x20);
    pic.out(0x20, 0xD2);  // A7-A6=11, ADI=0 (interval 8)
    pic.out(0x21, 0x00);
    pic.out(0x21, 0x00);
    pic.set_line(3, false);
    Pulse(pic, 3);
    EXPECT_EQ(Ack(pic), 0xD8);
}

}  // namespace
