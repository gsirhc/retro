// GoogleTest suite for the 8237 DMA register file: reset defaults
// (everything masked), the address/count low-then-high byte-pointer
// protocol and its clear command, single/all-channel masking, and DMA2's
// doubled port stride.

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

TEST(Dma8237Test, AdvanceIncrementsAddressAndDetectsTerminalCount) {
    Dma8237 dma(0x00, 1);
    dma.reset();
    dma.out(0x04, 0x00); dma.out(0x04, 0x00);  // ch2 address = 0
    dma.out(0x05, 0x01); dma.out(0x05, 0x00);  // ch2 count = 1 (2 bytes: N-1 convention)
    EXPECT_FALSE(dma.advance(2));  // count was 1, not 0 -- not yet TC
    EXPECT_EQ(dma.address(2), 1);
    EXPECT_EQ(dma.count(2), 0);
    EXPECT_TRUE(dma.advance(2));   // count was 0 -- this one is TC
    EXPECT_EQ(dma.address(2), 2);
    EXPECT_EQ(dma.count(2), 0xFFFF);
}

TEST(Dma8237Test, MasterClearResetsChipViaPort) {
    Dma8237 dma(0x00, 1);
    dma.reset();
    dma.out(0x0A, 0x02);  // unmask channel 2
    ASSERT_FALSE(dma.channel_masked(2));
    dma.out(0x0D, 0);     // master clear
    EXPECT_TRUE(dma.channel_masked(2));
}

}  // namespace
