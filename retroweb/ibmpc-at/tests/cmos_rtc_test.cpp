// GoogleTest suite for the MC146818 CMOS/RTC: address/data port protocol,
// the NMI-mask bit, the always-clear UIP bit (so BIOS's update-in-progress
// wait loop can't hang), register C's read-clears-flags behavior, and the
// poke()/peek() host-side preload path.

#include <gtest/gtest.h>

#include "cmos_rtc.h"

namespace {

using ibmpcat::CmosRtc;

TEST(CmosRtcTest, ResetLeavesBatteryValidFlagSet) {
    CmosRtc cmos;
    cmos.reset();
    cmos.out(0x70, 0x0D);
    EXPECT_EQ(cmos.in(0x71) & 0x80, 0x80);
}

TEST(CmosRtcTest, AddressDataRoundTrip) {
    CmosRtc cmos;
    cmos.reset();
    cmos.out(0x70, 0x15);  // an arbitrary general-purpose config byte
    cmos.out(0x71, 0x42);
    cmos.out(0x70, 0x15);  // re-select before reading (real protocol)
    EXPECT_EQ(cmos.in(0x71), 0x42);
}

TEST(CmosRtcTest, HighBitOfAddressPortSetsNmiMask) {
    CmosRtc cmos;
    cmos.reset();
    EXPECT_FALSE(cmos.nmi_masked());
    cmos.out(0x70, 0x80 | 0x0D);
    EXPECT_TRUE(cmos.nmi_masked());
    // the low 7 bits are still the register address, unaffected by the mask bit
    EXPECT_EQ(cmos.in(0x71) & 0x80, 0x80);
}

TEST(CmosRtcTest, RegisterAUpdateInProgressNeverAsserts) {
    CmosRtc cmos;
    cmos.reset();
    cmos.out(0x70, 0x0A);
    cmos.out(0x71, 0xFF);  // even if something wrote all-1s, including bit7...
    cmos.out(0x70, 0x0A);
    EXPECT_EQ(cmos.in(0x71) & 0x80, 0x00);  // ...UIP must still read 0
}

TEST(CmosRtcTest, RegisterCReadClearsIt) {
    CmosRtc cmos;
    cmos.reset();
    cmos.out(0x70, 0x0C);
    cmos.out(0x71, 0x40);  // pretend an interrupt flag got set
    cmos.out(0x70, 0x0C);
    EXPECT_EQ(cmos.in(0x71), 0x40);
    cmos.out(0x70, 0x0C);
    EXPECT_EQ(cmos.in(0x71), 0x00);  // cleared by the read above
}

TEST(CmosRtcTest, PokeAndPeekBypassThePortProtocol) {
    CmosRtc cmos;
    cmos.reset();
    cmos.poke(0x12, 0x28);  // e.g. fixed-disk type nibble byte
    EXPECT_EQ(cmos.peek(0x12), 0x28);
    cmos.out(0x70, 0x12);
    EXPECT_EQ(cmos.in(0x71), 0x28);  // visible through the normal port path too
}

}  // namespace
