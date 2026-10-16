// Board smoke: the generated self-test ROM boots, writes the RAM signature, kicks the watchdog, paints.

#include <gtest/gtest.h>

#include "hwtest_roms.h"
#include "hwtest_set.h"
#include "machine.h"

#include <algorithm>
#include <array>

namespace {


}  // namespace

TEST(Smoke, HwtestBootsSignatureWatchdogAndPaints) {
    frogger::Machine m;
    m.load_roms(frogger::hwtest_set());
    m.reset();
    m.run_cycles(frogger::kCpuHz / 10);
    EXPECT_EQ(m.ram[0], 'T');
    EXPECT_EQ(m.ram[1], 'S');
    EXPECT_EQ(m.ram[2], 'T');
    EXPECT_EQ(m.ram[3], '1');
    EXPECT_FALSE(m.watchdog_reset);
    EXPECT_GT(m.frames, 0);

    std::array<uint32_t, frogger::kUprightW * frogger::kUprightH> rgb{};
    m.render(rgb.data());
    bool any = false;
    for (uint32_t p : rgb) {
        if (p) { any = true; break; }
    }
    EXPECT_TRUE(any);
}
