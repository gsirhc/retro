#include <gtest/gtest.h>

#include "dma8237.h"

namespace {

using ibmpcat::Dma8237;

TEST(Dma8237Test, ResetMasksEveryChannel) {
    Dma8237 dma(0x00, 1);
    dma.reset();
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(dma.channel_masked(i));
}

TEST(Dma8237Test, AddressRegisterLowThenHighByteProtocol) {
    Dma8237 dma(0x00, 1);
    dma.reset();
    dma.out(0x04, 0x34);  // channel 2 address, low byte (reg index 4 = ch2 addr)
    dma.out(0x04, 0x12);  // high byte
    dma.out(0x0C, 0);     // clear byte pointer flip-flop before reading back
    EXPECT_EQ(dma.in(0x04), 0x34);
    EXPECT_EQ(dma.in(0x04), 0x12);
}

TEST(Dma8237Test, CountRegisterIsTheOddOffsetOfEachChannelPair) {
    Dma8237 dma(0x00, 1);
    dma.reset();
    dma.out(0x05, 0xFF);  // channel 2 count, low byte (reg index 5 = ch2 count)
    dma.out(0x05, 0x01);  // high byte -> count = 0x01FF
    dma.out(0x0C, 0);
    EXPECT_EQ(dma.in(0x05), 0xFF);
    EXPECT_EQ(dma.in(0x05), 0x01);
}

TEST(Dma8237Test, SingleMaskRegisterUnmasksOneChannel) {
    Dma8237 dma(0x00, 1);
    dma.reset();
    dma.out(0x0A, 0x02);  // channel 2, clear-mask bit not set (bit2=0) -> unmask
    EXPECT_FALSE(dma.channel_masked(2));
    EXPECT_TRUE(dma.channel_masked(0));
    EXPECT_TRUE(dma.channel_masked(1));
    EXPECT_TRUE(dma.channel_masked(3));
}

TEST(Dma8237Test, ClearMaskRegisterUnmasksAll) {
    Dma8237 dma(0x00, 1);
    dma.reset();
    dma.out(0x0E, 0);  // any value
    for (int i = 0; i < 4; ++i) EXPECT_FALSE(dma.channel_masked(i));
}

TEST(Dma8237Test, PageRegistersAreIndependentOfPortDecode) {
    Dma8237 dma(0x00, 1);
    dma.reset();
    dma.set_page(2, 0x00);  // floppy DMA (channel 2) page register, e.g. from port 0x81
    EXPECT_EQ(dma.page(2), 0x00);
    dma.set_page(2, 0x0A);
    EXPECT_EQ(dma.page(2), 0x0A);
}

TEST(Dma8237Test, Dma2UsesDoubledPortStride) {
    Dma8237 dma2(0xC0, 2);
    dma2.reset();
    dma2.out(0xC4, 0x78);  // channel 2 address low byte, at base+4*stride
    dma2.out(0xC4, 0x56);
    dma2.out(0xD8, 0);     // clear byte pointer -- reg 12 at base+12*stride = 0xC0+24=0xD8
    EXPECT_EQ(dma2.in(0xC4), 0x78);
    EXPECT_EQ(dma2.in(0xC4), 0x56);
}

TEST(Dma8237Test, TransferIncrementsAddressAndDetectsTerminalCount) {
    Dma8237 dma(0x00, 1);
    dma.reset();
    dma.out(0x04, 0x00); dma.out(0x04, 0x00);  // ch2 address = 0
    dma.out(0x05, 0x01); dma.out(0x05, 0x00);  // ch2 count = 1 (2 bytes: N-1 convention)
    dma.out(0x0A, 0x02);
    bool tc = true;
    EXPECT_EQ(dma.transfer(2, &tc), 0);
    EXPECT_FALSE(tc);
    EXPECT_EQ(dma.transfer(2, &tc), 1);
    EXPECT_TRUE(tc);
    EXPECT_EQ(dma.address(2), 2);
    EXPECT_EQ(dma.count(2), 0xFFFF);
    EXPECT_TRUE(dma.channel_masked(2));
}

TEST(Dma8237Test, AddressWrapsWithinTheSixteenBitCounter) {
    Dma8237 dma(0x00, 1);
    dma.reset();
    dma.out(0x04, 0xFF); dma.out(0x04, 0xFF);
    dma.out(0x05, 0x01); dma.out(0x05, 0x00);
    bool tc;
    EXPECT_EQ(dma.transfer(2, &tc), 0xFFFF);
    EXPECT_EQ(dma.transfer(2, &tc), 0x0000);
}

TEST(Dma8237Test, DecrementModeCountsAddressDown) {
    Dma8237 dma(0x00, 1);
    dma.reset();
    dma.out(0x0B, 0x66);  // ch2, write, decrement
    dma.out(0x04, 0x01); dma.out(0x04, 0x00);
    dma.out(0x05, 0x05); dma.out(0x05, 0x00);
    bool tc;
    EXPECT_EQ(dma.transfer(2, &tc), 0x0001);
    EXPECT_EQ(dma.transfer(2, &tc), 0x0000);
    EXPECT_EQ(dma.transfer(2, &tc), 0xFFFF);
}

TEST(Dma8237Test, AutoinitReloadsAndLeavesTheMaskAlone) {
    Dma8237 dma(0x00, 1);
    dma.reset();
    dma.out(0x0B, 0x56);  // ch2, write, autoinit
    dma.out(0x04, 0x00); dma.out(0x04, 0x30);
    dma.out(0x05, 0x01); dma.out(0x05, 0x00);
    dma.out(0x0A, 0x02);
    bool tc;
    dma.transfer(2, &tc);
    dma.transfer(2, &tc);
    EXPECT_TRUE(tc);
    EXPECT_EQ(dma.address(2), 0x3000);
    EXPECT_EQ(dma.count(2), 0x0001);
    EXPECT_FALSE(dma.channel_masked(2));
}

TEST(Dma8237Test, StatusReportsTerminalCountOnceAndLiveRequests) {
    Dma8237 dma(0x00, 1);
    dma.reset();
    dma.out(0x05, 0x00); dma.out(0x05, 0x00);
    bool tc;
    dma.transfer(2, &tc);
    dma.set_dreq(1, true);
    dma.out(0x09, 0x07);  // software request, ch3
    EXPECT_EQ(dma.in(0x08), 0xA4);
    EXPECT_EQ(dma.in(0x08), 0xA0);
    dma.set_dreq(1, false);
    dma.out(0x09, 0x03);
    EXPECT_EQ(dma.in(0x08), 0x00);
}

TEST(Dma8237Test, TerminalCountClearsTheSoftwareRequest) {
    Dma8237 dma(0x00, 1);
    dma.reset();
    dma.out(0x09, 0x06);
    dma.out(0x05, 0x00); dma.out(0x05, 0x00);
    bool tc;
    dma.transfer(2, &tc);
    EXPECT_EQ(dma.in(0x08), 0x04);
}

TEST(Dma8237Test, ModeSelectsTransferType) {
    Dma8237 dma(0x00, 1);
    dma.reset();
    dma.out(0x0B, 0x42);
    EXPECT_EQ(dma.type(2), Dma8237::kVerify);
    dma.out(0x0B, 0x46);
    EXPECT_EQ(dma.type(2), Dma8237::kWrite);
    dma.out(0x0B, 0x4A);
    EXPECT_EQ(dma.type(2), Dma8237::kRead);
}

TEST(Dma8237Test, CommandRegisterCanDisableTheController) {
    Dma8237 dma(0x00, 1);
    dma.reset();
    dma.out(0x0A, 0x02);
    EXPECT_TRUE(dma.can_service(2));
    dma.out(0x08, 0x04);
    EXPECT_FALSE(dma.can_service(2));
}

TEST(Dma8237Test, MaskRegisterIsWriteOnly) {
    Dma8237 dma(0x00, 1);
    dma.reset();
    dma.out(0x0F, 0x05);
    EXPECT_TRUE(dma.channel_masked(0));
    EXPECT_FALSE(dma.channel_masked(1));
    EXPECT_EQ(dma.in(0x0F), 0xFF);
}

TEST(Dma8237Test, MasterClearResetsChipViaPort) {
    Dma8237 dma(0x00, 1);
    dma.reset();
    dma.out(0x0A, 0x02);  // unmask channel 2
    dma.out(0x04, 0x34); dma.out(0x04, 0x12);
    dma.out(0x0B, 0x46);
    dma.set_page(2, 0x07);
    ASSERT_FALSE(dma.channel_masked(2));
    dma.out(0x0D, 0);     // master clear
    EXPECT_TRUE(dma.channel_masked(2));
    EXPECT_EQ(dma.address(2), 0x1234);
    EXPECT_EQ(dma.mode(2), 0x46);
    EXPECT_EQ(dma.page(2), 0x07);
    EXPECT_EQ(dma.in(0x04), 0x34);  // flip-flop cleared
}

TEST(Dma8237Test, Dma2OddPortsAliasTheEvenOnes) {
    Dma8237 dma2(0xC0, 2);
    dma2.reset();
    dma2.out(0xC5, 0x11);
    dma2.out(0xC5, 0x22);
    dma2.out(0xD8, 0);
    EXPECT_EQ(dma2.in(0xC4), 0x11);
}

}  // namespace
