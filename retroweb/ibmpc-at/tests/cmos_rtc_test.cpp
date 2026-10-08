#include <gtest/gtest.h>

#include "cmos_rtc.h"

namespace {

using ibmpcat::CmosRtc;

constexpr double kHz = 8000000.0;
constexpr uint64_t kSecond = 8000000;

uint8_t Read(CmosRtc &c, uint8_t reg) { c.out(0x70, reg); return c.in(0x71); }
void Write(CmosRtc &c, uint8_t reg, uint8_t v) { c.out(0x70, reg); c.out(0x71, v); }

TEST(CmosRtcTest, ResetLeavesBatteryValidFlagSet) {
    CmosRtc cmos;
    EXPECT_EQ(Read(cmos, 0x0D) & 0x80, 0x80);
}

TEST(CmosRtcTest, AddressDataRoundTrip) {
    CmosRtc cmos;
    Write(cmos, 0x15, 0x42);
    EXPECT_EQ(Read(cmos, 0x15), 0x42);
}

TEST(CmosRtcTest, HighBitOfAddressPortSetsNmiMask) {
    CmosRtc cmos;
    EXPECT_FALSE(cmos.nmi_masked());
    cmos.out(0x70, 0x80 | 0x0D);
    EXPECT_TRUE(cmos.nmi_masked());
    EXPECT_EQ(cmos.in(0x71) & 0x80, 0x80) << "the low 7 bits still select the register";
}

TEST(CmosRtcTest, SixtyFourBytesAliasAbove3Fh) {
    CmosRtc cmos;
    Write(cmos, 0x1B, 0xF2);
    EXPECT_EQ(Read(cmos, 0x5B), 0xF2) << "the 5170's MC146818A decodes six address bits";
}

TEST(CmosRtcTest, PowerOnRunsInBcdTwentyFourHourMode) {
    CmosRtc cmos;
    EXPECT_EQ(Read(cmos, 0x0A), 0x26);
    EXPECT_EQ(Read(cmos, 0x0B), 0x02);
    EXPECT_EQ(Read(cmos, 0x09), 0x86);
    EXPECT_EQ(Read(cmos, 0x32), 0x19);
}

TEST(CmosRtcTest, PokeAndPeekBypassThePortProtocol) {
    CmosRtc cmos;
    cmos.poke(0x12, 0x28);
    EXPECT_EQ(cmos.peek(0x12), 0x28);
    EXPECT_EQ(Read(cmos, 0x12), 0x28);
}

TEST(CmosRtcTest, SetTimeLoadsBcdCalendarAndCentury) {
    CmosRtc cmos;
    cmos.set_time(2026, 10, 4, 15, 42, 7, 1);
    EXPECT_EQ(Read(cmos, 0x00), 0x07);
    EXPECT_EQ(Read(cmos, 0x02), 0x42);
    EXPECT_EQ(Read(cmos, 0x04), 0x15);
    EXPECT_EQ(Read(cmos, 0x06), 0x01);
    EXPECT_EQ(Read(cmos, 0x07), 0x04);
    EXPECT_EQ(Read(cmos, 0x08), 0x10);
    EXPECT_EQ(Read(cmos, 0x09), 0x26);
    EXPECT_EQ(Read(cmos, 0x32), 0x20);
}

TEST(CmosRtcTest, SetTimeFollowsBinaryAndTwelveHourModes) {
    CmosRtc cmos;
    Write(cmos, 0x0B, 0x04);  // binary, 12-hour
    cmos.set_time(1994, 1, 1, 15, 30, 0, 7);
    EXPECT_EQ(Read(cmos, 0x04), 0x83) << "3 PM: hour 3 with the PM bit";
    EXPECT_EQ(Read(cmos, 0x02), 30);
}

TEST(CmosRtcTest, TheClockAdvancesOneSecondPerSecondOfGuestTime) {
    CmosRtc cmos;
    cmos.set_time(1994, 1, 1, 0, 0, 0, 7);
    cmos.tick(kSecond / 2, kHz);
    EXPECT_EQ(Read(cmos, 0x00), 0x00);
    cmos.tick(kSecond + 1000, kHz);
    EXPECT_EQ(Read(cmos, 0x00), 0x01);
    cmos.tick(10 * kSecond + 1000, kHz);
    EXPECT_EQ(Read(cmos, 0x00), 0x10);
}

TEST(CmosRtcTest, TheCrystalNotTheCpuClockSetsTheRate) {
    CmosRtc cmos;
    cmos.set_time(1994, 1, 1, 0, 0, 0, 7);
    cmos.tick(33000000 + 1000, 33000000.0);  // a 33 MHz CPU: half the cycles per second
    EXPECT_EQ(Read(cmos, 0x00), 0x01);
}

TEST(CmosRtcTest, UpdateInProgressRisesJustBeforeTheUpdate) {
    CmosRtc cmos;
    cmos.tick(kSecond - kSecond / 1000, kHz);  // 1 ms before the update
    EXPECT_NE(Read(cmos, 0x0A) & 0x80, 0) << "UIP covers the 244 us lead plus the 1984 us update";
    cmos.tick(kSecond + 1000, kHz);
    EXPECT_EQ(Read(cmos, 0x0A) & 0x80, 0);
    cmos.tick(kSecond + kSecond / 2, kHz);
    EXPECT_EQ(Read(cmos, 0x0A) & 0x80, 0) << "clear for most of the second";
}

TEST(CmosRtcTest, MidnightOnNewYearsEveRollsEveryField) {
    CmosRtc cmos;
    cmos.set_time(1999, 12, 31, 23, 59, 59, 7);
    cmos.tick(kSecond + 1000, kHz);
    EXPECT_EQ(Read(cmos, 0x00), 0x00);
    EXPECT_EQ(Read(cmos, 0x02), 0x00);
    EXPECT_EQ(Read(cmos, 0x04), 0x00);
    EXPECT_EQ(Read(cmos, 0x06), 0x01) << "Saturday wraps to Sunday";
    EXPECT_EQ(Read(cmos, 0x07), 0x01);
    EXPECT_EQ(Read(cmos, 0x08), 0x01);
    EXPECT_EQ(Read(cmos, 0x09), 0x00);
    EXPECT_EQ(Read(cmos, 0x32), 0x19) << "the century byte is plain RAM the chip never touches";
}

TEST(CmosRtcTest, FebruaryHasTwentyNineDaysEveryFourthYear) {
    CmosRtc cmos;
    cmos.set_time(1996, 2, 28, 23, 59, 59, 4);
    cmos.tick(kSecond + 1000, kHz);
    EXPECT_EQ(Read(cmos, 0x07), 0x29);
    cmos.set_time(1995, 2, 28, 23, 59, 59, 3);
    cmos.tick(2 * kSecond + 1000, kHz);
    EXPECT_EQ(Read(cmos, 0x07), 0x01);
    EXPECT_EQ(Read(cmos, 0x08), 0x03);
}

TEST(CmosRtcTest, TwelveHourModeRollsElevenPmToTwelveAm) {
    CmosRtc cmos;
    Write(cmos, 0x0B, 0x00);  // BCD, 12-hour
    cmos.set_time(1994, 1, 1, 23, 59, 59, 7);
    EXPECT_EQ(Read(cmos, 0x04), 0x91);
    cmos.tick(kSecond + 1000, kHz);
    EXPECT_EQ(Read(cmos, 0x04), 0x12);
}

TEST(CmosRtcTest, PeriodicFlagRaisesIrq8OnlyWhenEnabled) {
    CmosRtc cmos;
    cmos.tick(kSecond / 1000, kHz);  // past one 1024 Hz period
    EXPECT_FALSE(cmos.irq_pending()) << "PF sets, but PIE is off";
    EXPECT_EQ(Read(cmos, 0x0C), 0x40);
    Write(cmos, 0x0B, 0x02 | 0x40);  // PIE
    cmos.tick(2 * kSecond / 1000, kHz);
    EXPECT_TRUE(cmos.irq_pending());
    EXPECT_EQ(Read(cmos, 0x0C), 0xC0);
    EXPECT_FALSE(cmos.irq_pending()) << "reading C drops IRQ8";
}

TEST(CmosRtcTest, UpdateEndedAndAlarmInterrupts) {
    CmosRtc cmos;
    cmos.set_time(1994, 1, 1, 6, 30, 0, 7);
    Write(cmos, 0x01, 0x02);  // alarm at second 2
    Write(cmos, 0x03, 0xC0);  // any minute
    Write(cmos, 0x05, 0xFF);  // any hour
    Write(cmos, 0x0B, 0x02 | 0x20 | 0x10);  // AIE, UIE
    cmos.tick(kSecond + 1000, kHz);
    EXPECT_EQ(Read(cmos, 0x0C) & 0xB0, 0x90) << "update ended, no alarm yet";
    cmos.tick(2 * kSecond + 1000, kHz);
    EXPECT_EQ(Read(cmos, 0x0C) & 0xB0, 0xB0) << "update ended and alarm matched";
}

TEST(CmosRtcTest, SetBitFreezesTheClockAndClearsUie) {
    CmosRtc cmos;
    cmos.set_time(1994, 1, 1, 0, 0, 0, 7);
    Write(cmos, 0x0B, 0x80 | 0x10 | 0x02);
    EXPECT_EQ(Read(cmos, 0x0B) & 0x10, 0);
    cmos.tick(3 * kSecond, kHz);
    EXPECT_EQ(Read(cmos, 0x00), 0x00);
    Write(cmos, 0x00, 0x30);  // software sets the time while frozen
    Write(cmos, 0x0B, 0x02);
    cmos.tick(3 * kSecond + kSecond / 2 + 1000, kHz);
    EXPECT_EQ(Read(cmos, 0x00), 0x31) << "the first update lands half a second after SET clears";
}

TEST(CmosRtcTest, RegistersCAndDAreReadOnly) {
    CmosRtc cmos;
    Write(cmos, 0x0C, 0xF0);
    EXPECT_EQ(Read(cmos, 0x0C), 0x00);
    Write(cmos, 0x0D, 0x00);
    EXPECT_EQ(Read(cmos, 0x0D), 0x80);
}

TEST(CmosRtcTest, StoppingTheDividerHoldsTheTime) {
    CmosRtc cmos;
    cmos.set_time(1994, 1, 1, 0, 0, 0, 7);
    Write(cmos, 0x0A, 0x76);  // divider held in reset
    cmos.tick(5 * kSecond, kHz);
    EXPECT_EQ(Read(cmos, 0x00), 0x00);
    EXPECT_EQ(Read(cmos, 0x0C) & 0x40, 0) << "no periodic flag without the time base";
}

}  // namespace
