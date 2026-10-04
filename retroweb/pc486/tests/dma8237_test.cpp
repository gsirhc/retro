// GoogleTest suite for the 8237 DMA register file: reset defaults
// (everything masked), the address/count low-then-high byte-pointer
// protocol and its clear command, single/all-channel masking, DMA2's
// doubled port stride, and the status register's per-channel
// terminal-count latch and live/software request bits.

#include <gtest/gtest.h>

#include "dma8237.h"

namespace {

using pc486::Dma8237;

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

TEST(Dma8237Test, AutoinitializeReloadsAddressAndCountAtTerminalCount) {
    Dma8237 dma(0x00, 1);
    dma.reset();
    dma.out(0x04, 0x05); dma.out(0x04, 0x00);  // ch2 address = 5
    dma.out(0x05, 0x00); dma.out(0x05, 0x00);  // ch2 count = 0 (1 byte)
    dma.out(0x0B, 0x12);                       // mode: channel 2, autoinit set
    EXPECT_TRUE(dma.advance(2));                // count was 0 -- TC, reloads from base
    EXPECT_EQ(dma.address(2), 5);
    EXPECT_EQ(dma.count(2), 0);
    EXPECT_TRUE(dma.advance(2));                // reloaded, so this is TC again too
    EXPECT_EQ(dma.address(2), 5);
    EXPECT_EQ(dma.count(2), 0);
}

TEST(Dma8237Test, WithoutAutoinitializeAddressWrapsInsteadOfReloading) {
    Dma8237 dma(0x00, 1);
    dma.reset();
    dma.out(0x04, 0x05); dma.out(0x04, 0x00);  // ch2 address = 5
    dma.out(0x05, 0x00); dma.out(0x05, 0x00);  // ch2 count = 0 (1 byte)
    dma.out(0x0B, 0x02);                       // mode: channel 2, autoinit clear
    EXPECT_TRUE(dma.advance(2));                // count was 0 -- TC
    EXPECT_EQ(dma.address(2), 6);               // kept incrementing, no reload
    EXPECT_EQ(dma.count(2), 0xFFFF);
}

TEST(Dma8237Test, AdvanceDecrementsAddressWhenModeBitFiveIsSet) {
    // Intel 8237A-5 data sheet, "Mode Register": bit 5 selects address
    // decrement instead of increment.
    Dma8237 dma(0x00, 1);
    dma.reset();
    dma.out(0x04, 0x05); dma.out(0x04, 0x00);  // ch2 address = 5
    dma.out(0x05, 0x02); dma.out(0x05, 0x00);  // ch2 count = 2 (3 bytes)
    dma.out(0x0B, 0x22);                       // mode: channel 2, decrement set
    EXPECT_FALSE(dma.advance(2));
    EXPECT_EQ(dma.address(2), 4);
    EXPECT_EQ(dma.count(2), 1);
    EXPECT_FALSE(dma.advance(2));
    EXPECT_EQ(dma.address(2), 3);
}

TEST(Dma8237Test, AddressWrapsWithinThe64KPageInDecrementMode) {
    Dma8237 dma(0x00, 1);
    dma.reset();
    dma.out(0x04, 0x00); dma.out(0x04, 0x00);  // ch2 address = 0
    dma.out(0x05, 0x01); dma.out(0x05, 0x00);  // ch2 count = 1 (2 bytes)
    dma.out(0x0B, 0x22);                       // decrement mode
    EXPECT_FALSE(dma.advance(2));
    EXPECT_EQ(dma.address(2), 0xFFFF);  // wraps within the page; page register untouched
}

TEST(Dma8237Test, AddressWrapsWithinThe64KPageInIncrementMode) {
    // Same quirk the other direction -- 0xFFFF + 1 wraps to 0, not into the
    // next page.
    Dma8237 dma(0x00, 1);
    dma.reset();
    dma.out(0x04, 0xFF); dma.out(0x04, 0xFF);  // ch2 address = 0xFFFF
    dma.out(0x05, 0x01); dma.out(0x05, 0x00);  // ch2 count = 1 (2 bytes)
    dma.out(0x0B, 0x02);                       // mode: channel 2, increment (default)
    EXPECT_FALSE(dma.advance(2));
    EXPECT_EQ(dma.address(2), 0x0000);
}

TEST(Dma8237Test, MasterClearResetsChipViaPort) {
    Dma8237 dma(0x00, 1);
    dma.reset();
    dma.out(0x0A, 0x02);  // unmask channel 2
    ASSERT_FALSE(dma.channel_masked(2));
    dma.out(0x0D, 0);     // master clear
    EXPECT_TRUE(dma.channel_masked(2));
}

// --- status register (in(8)) ----------------------------------------------

TEST(Dma8237Test, StatusRegisterReadsZeroAfterReset) {
    Dma8237 dma(0x00, 1);
    dma.reset();
    EXPECT_EQ(dma.in(0x08), 0x00);
}

TEST(Dma8237Test, StatusRegisterTerminalCountBitSetsOnAdvanceAndClearsOnRead) {
    Dma8237 dma(0x00, 1);
    dma.reset();
    dma.out(0x04, 0x00); dma.out(0x04, 0x00);  // ch2 address = 0
    dma.out(0x05, 0x00); dma.out(0x05, 0x00);  // ch2 count = 0 (1 byte)
    EXPECT_TRUE(dma.advance(2));                // count was 0 -- TC
    EXPECT_EQ(dma.in(0x08) & 0x0F, 0x04)        // bit 2 (channel 2)
        << "TC latch must survive until the status register is read";
    EXPECT_EQ(dma.in(0x08) & 0x0F, 0x00) << "reading the status register clears TC";
}

TEST(Dma8237Test, StatusRegisterTerminalCountIsPerChannel) {
    Dma8237 dma(0x00, 1);
    dma.reset();
    // Channel 0: count = 0, TC on first advance.
    dma.out(0x00, 0x00); dma.out(0x00, 0x00);
    dma.out(0x01, 0x00); dma.out(0x01, 0x00);
    // Channel 3: count = 1, not yet TC on first advance.
    dma.out(0x06, 0x00); dma.out(0x06, 0x00);
    dma.out(0x07, 0x01); dma.out(0x07, 0x00);
    EXPECT_TRUE(dma.advance(0));
    EXPECT_FALSE(dma.advance(3));
    EXPECT_EQ(dma.in(0x08) & 0x0F, 0x01) << "only channel 0's bit is set";
}

TEST(Dma8237Test, StatusRegisterRequestBitFollowsLiveDreqUnlatched) {
    Dma8237 dma(0x00, 1);
    dma.reset();
    dma.set_dreq(2, true);
    EXPECT_EQ(dma.in(0x08) & 0xF0, 0x40);  // bit 6 (channel 2's request bit)
    // Unlike TC, a request bit is not latched by the read -- it tracks the
    // live signal, so it drops the instant the device deasserts DREQ.
    EXPECT_EQ(dma.in(0x08) & 0xF0, 0x40);
    dma.set_dreq(2, false);
    EXPECT_EQ(dma.in(0x08) & 0xF0, 0x00);
}

TEST(Dma8237Test, StatusRegisterRequestBitAlsoFollowsTheSoftwareRequestRegister) {
    Dma8237 dma(0x00, 1);
    dma.reset();
    dma.out(0x09, 0x05);  // Request Register: channel 1 (bits 0-1), set bit (bit 2)
    EXPECT_EQ(dma.in(0x08) & 0xF0, 0x20);  // bit 5 (channel 1)
    dma.out(0x09, 0x01);  // same channel, set bit clear -- resets it
    EXPECT_EQ(dma.in(0x08) & 0xF0, 0x00);
}

TEST(Dma8237Test, StatusRegisterRequestBitStaysSetWhileMaskedSinceItIsNeverServiced) {
    // The one case this is actually observable here: every real transfer in
    // this emulator resolves within the tick that sets DREQ, so a masked
    // channel left wanting service is what makes the bit visible at all.
    Dma8237 dma(0x00, 1);
    dma.reset();
    dma.set_dreq(2, true);
    EXPECT_TRUE(dma.channel_masked(2));  // reset() leaves every channel masked
    EXPECT_EQ(dma.in(0x08) & 0xF0, 0x40);
}

}  // namespace
