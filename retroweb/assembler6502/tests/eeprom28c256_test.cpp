// GoogleTest suite for eeprom28c256::Eeprom -- page-write timing and the
// bus-write-is-a-no-op contract (see eeprom28c256.h).

#include <gtest/gtest.h>

#include "../eeprom28c256.h"

using namespace eeprom28c256;

namespace {

TEST(Eeprom28C256, ReadReturnsLoadedImage) {
    Eeprom e;
    uint8_t img[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    e.load_image(img, 8);
    EXPECT_EQ(e.read(0), 1);
    EXPECT_EQ(e.read(7), 8);
}

TEST(Eeprom28C256, ProgramIsNotVisibleUntilThePageWriteCycleCompletes) {
    Eeprom e;
    e.erase_all();
    uint8_t data[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    e.begin_program(0x0100, data, 4);
    EXPECT_TRUE(e.busy());
    EXPECT_EQ(e.read(0x0100), 0xFF);   // still the erased state mid-write
    e.advance(kPageWriteCycleUs - 1);
    EXPECT_TRUE(e.busy());
    e.advance(2);
    EXPECT_FALSE(e.busy());
    EXPECT_EQ(e.read(0x0100), 0xAA);
    EXPECT_EQ(e.read(0x0103), 0xDD);
}

TEST(Eeprom28C256, WriteSpanningTwoPagesTakesTwoPageCycles) {
    Eeprom e;
    e.erase_all();
    uint8_t data[10];
    for (int i = 0; i < 10; i++) data[i] = uint8_t(0x10 + i);
    // Starts 2 bytes before the page boundary, runs 8 bytes into the next
    // page -- touches exactly pages 0 and 1.
    e.begin_program(uint16_t(kPageSize - 2), data, 10);
    e.advance(kPageWriteCycleUs);        // first page committed
    EXPECT_TRUE(e.busy());
    EXPECT_EQ(e.read(kPageSize - 2), data[0]);   // first page's data landed
    EXPECT_EQ(e.read(kPageSize), 0xFF);           // second page not yet
    e.advance(kPageWriteCycleUs);
    EXPECT_FALSE(e.busy());
    EXPECT_EQ(e.read(kPageSize), data[2]);
    EXPECT_EQ(e.read(kPageSize + 7), data[9]);
}

TEST(Eeprom28C256, ProgressReports0To1AcrossTheBurn) {
    Eeprom e;
    uint8_t data[1] = {0x00};
    e.begin_program(0, data, 1);
    EXPECT_DOUBLE_EQ(e.progress(), 0.0);
    e.advance(kPageWriteCycleUs / 2);
    EXPECT_NEAR(e.progress(), 0.5, 0.01);
    e.advance(kPageWriteCycleUs);
    EXPECT_DOUBLE_EQ(e.progress(), 1.0);
}

} // namespace
