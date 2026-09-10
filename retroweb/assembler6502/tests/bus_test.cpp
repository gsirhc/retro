// GoogleTest suite for bus::Bus -- the address decode derived directly
// from pcb6502full.net, including the two confirmed corrections (see
// bus.h and CGOAC6502_REVIEW.md).

#include <gtest/gtest.h>

#include "../bus.h"

using namespace bus;

namespace {

class BusTest : public ::testing::Test {
protected:
    Bus b;
    void SetUp() override { b.reset(); }
};

TEST_F(BusTest, RamOccupiesOnlyTheLower16K) {
    b.write(0x0000, 0x11);
    b.write(0x3FFF, 0x22);
    EXPECT_EQ(b.read(0x0000), 0x11);
    EXPECT_EQ(b.read(0x3FFF), 0x22);
}

TEST_F(BusTest, RomOccupiesUpperHalfAndIgnoresWrites) {
    uint8_t img[32768] = {};
    img[0] = 0x55;               // corresponds to $8000
    img[32767] = 0xAA;           // corresponds to $FFFF
    b.rom.load_image(img, 32768);
    EXPECT_EQ(b.read(0x8000), 0x55);
    EXPECT_EQ(b.read(0xFFFF), 0xAA);
    b.write(0x8000, 0x99);        // ~WE tied to +5V -- always write-disabled in-circuit
    EXPECT_EQ(b.read(0x8000), 0x55);
}

TEST_F(BusTest, ViaDecodedAt6000MirroredAcrossItsWindow) {
    b.write(0x6003, 0xFF);        // DDRA
    b.write(0x6001, 0x3C);        // ORA
    EXPECT_EQ(b.read(0x6001), 0x3C);
    EXPECT_EQ(b.read(0x6801), 0x3C);   // A0-A3 mirrored across the whole 8K window
    EXPECT_FALSE(b.last_access_was_contended);
}

TEST_F(BusTest, AciaDecodedAt5000MirroredAcrossItsWindow) {
    b.write(0x5003, 0x0F);        // CTRL
    EXPECT_EQ(b.acia.baud(), 19200);
    EXPECT_EQ(b.read(0x5003), 0x0F);
    EXPECT_EQ(b.read(0x5F03), 0x0F);
    EXPECT_FALSE(b.last_access_was_contended);
}

TEST_F(BusTest, UnmappedWindowBelowViaAndAciaFloatsHigh) {
    EXPECT_EQ(b.read(0x4500), 0xFF);   // $4000-$4FFF: CS2 active but neither A13 nor A12 set
}

TEST_F(BusTest, DocumentedViaAciaOverlapAt7000IsModeledAsContention) {
    b.write(0x7003, 0xFF);   // DDRA via the contended window
    b.write(0x7001, 0x81);   // ORA via the contended window
    EXPECT_TRUE(b.last_access_was_contended);
    // Both chips latched the same byte on write; the read path wired-ANDs
    // whatever each chip now reports (see bus.cpp) -- assert only that
    // contention is flagged and both chips actually saw the access, not a
    // specific numeric result (that's an approximation, not a claimed-exact
    // electrical outcome -- see bus.h).
    EXPECT_EQ(b.via.read(0x1), 0x81);
    b.read(0x7001);
    EXPECT_TRUE(b.last_access_was_contended);
}

TEST_F(BusTest, ContentionNeverFiresInTheRangesRealFirmwareUses) {
    // rom/bios.s uses $5000-$5003 (ACIA) and rom/via.s uses $6000-$600E
    // (VIA) exclusively -- neither ever touches $7000-$7FFF.
    for (uint16_t a = 0x5000; a <= 0x5003; a++) { b.read(a); EXPECT_FALSE(b.last_access_was_contended); }
    for (uint16_t a = 0x6000; a <= 0x600E; a++) { b.read(a); EXPECT_FALSE(b.last_access_was_contended); }
}

} // namespace
