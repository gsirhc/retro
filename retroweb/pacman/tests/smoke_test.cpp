// The shared Z80 boots this machine's self-test ROM, writes the RAM signature,
// kicks the watchdog, and paints.

#include <gtest/gtest.h>

#include "hwtest_roms.h"
#include "hwtest_set.h"
#include "machine.h"

#include <algorithm>
#include <array>

namespace {


}  // namespace

TEST(Smoke, HwtestBootsSignatureWatchdogAndPaints) {
    pacman::Machine m;
    m.load_roms(pacman::hwtest_set());
    m.reset();
    m.run_cycles(pacman::kCpuHz / 10);
    EXPECT_EQ(m.ram[0x4C00 - 0x4C00], 'T');
    EXPECT_EQ(m.ram[0x4C01 - 0x4C00], 'S');
    EXPECT_EQ(m.ram[0x4C02 - 0x4C00], 'T');
    EXPECT_EQ(m.ram[0x4C03 - 0x4C00], '1');
    EXPECT_FALSE(m.watchdog_reset);
    EXPECT_GT(m.frames, 0);

    std::array<uint32_t, pacman::kUprightW * pacman::kUprightH> fb{};
    m.render(fb.data());
    int lit = 0;
    for (uint32_t p : fb)
        if (p != 0) lit++;
    EXPECT_GT(lit, 1000);
}
