// Board smoke: the generated self-test ROM boots, passes the shifter and ROM 5-6 checks, paints the help screen.

#include <gtest/gtest.h>

#include "hwtest_roms.h"
#include "hwtest_set.h"
#include "machine.h"

#include <array>

TEST(Smoke, HwtestBootsSignatureAndPaints) {
    lunarrescue::Machine m;
    m.load_roms(lunarrescue::hwtest_set());
    m.reset();
    m.run_cycles(lunarrescue::kCpuHz / 10);
    EXPECT_EQ(m.ram[0], 'T');
    EXPECT_EQ(m.ram[1], 'S');
    EXPECT_EQ(m.ram[2], 'T');
    EXPECT_EQ(m.ram[3], '1');
    EXPECT_GT(m.frames, 0);

    std::array<uint32_t, lunarrescue::kUprightW * lunarrescue::kUprightH> rgb{};
    m.render(rgb.data());
    bool any = false;
    for (uint32_t p : rgb) {
        if (p) { any = true; break; }
    }
    EXPECT_TRUE(any);
}

TEST(Smoke, HwtestHoldsHelpScreenWithSelfTestOk) {
    lunarrescue::Machine m;
    m.load_roms(lunarrescue::hwtest_set());
    m.reset();
    m.run_cycles(lunarrescue::kCpuHz * 5);
    EXPECT_EQ(m.ram[5], lunarrescue::hwtest::shift_expect);
    EXPECT_EQ(m.ram[4], 1);
    EXPECT_EQ(m.ram[0x0A], 1) << "ROM 5-6 bank check";
    const auto& cells = lunarrescue::hwtest::status_ok;
    for (size_t i = 0; i < cells.size(); i++) {
        const int col = lunarrescue::hwtest::status_col + int(i / 8);
        const int addr = 0x2400 + col * 256 + int(i % 8) * 32 + (31 - lunarrescue::hwtest::status_row);
        EXPECT_EQ(m.mem_read(uint16_t(addr)), cells[i]) << "SELF TEST OK cell byte " << i;
    }
}
