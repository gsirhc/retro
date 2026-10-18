// Board smoke: the generated self-test ROM boots, passes the shifter check, kicks the watchdog, paints the help screen.

#include <gtest/gtest.h>

#include "hwtest_roms.h"
#include "hwtest_set.h"
#include "machine.h"

#include <array>

TEST(Smoke, HwtestBootsSignatureWatchdogAndPaints) {
    invaders::Machine m;
    m.load_roms(invaders::hwtest_set());
    m.reset();
    m.run_cycles(invaders::kCpuHz / 10);
    EXPECT_EQ(m.ram[0], 'T');
    EXPECT_EQ(m.ram[1], 'S');
    EXPECT_EQ(m.ram[2], 'T');
    EXPECT_EQ(m.ram[3], '1');
    EXPECT_FALSE(m.watchdog_reset);
    EXPECT_GT(m.frames, 0);

    std::array<uint32_t, invaders::kUprightW * invaders::kUprightH> rgb{};
    m.render(rgb.data());
    bool any = false;
    for (uint32_t p : rgb) {
        if (p) { any = true; break; }
    }
    EXPECT_TRUE(any);
}

TEST(Smoke, HwtestHoldsHelpScreenWithSelfTestOk) {
    invaders::Machine m;
    m.load_roms(invaders::hwtest_set());
    m.reset();
    m.run_cycles(invaders::kCpuHz * 4);
    ASSERT_FALSE(m.watchdog_reset);
    EXPECT_EQ(m.ram[5], invaders::hwtest::shift_expect);
    EXPECT_EQ(m.ram[4], 1);
    const auto& cells = invaders::hwtest::status_ok;
    for (size_t i = 0; i < cells.size(); i++) {
        const int col = invaders::hwtest::status_col + int(i / 8);
        const int addr = 0x2400 + col * 256 + int(i % 8) * 32 + (31 - invaders::hwtest::status_row);
        EXPECT_EQ(m.mem_read(uint16_t(addr)), cells[i]) << "SELF TEST OK cell byte " << i;
    }
}
