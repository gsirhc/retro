#include <gtest/gtest.h>

#include "hwtest_roms.h"
#include "hwtest_set.h"
#include "machine.h"

#include <algorithm>
#include <array>

namespace {


}  // namespace

TEST(Smoke, HwtestBootsSignatureWatchdogAndPaints) {
    galaga::Machine m;
    m.load_roms(galaga::hwtest_set());
    m.reset();
    m.run_cycles(galaga::kCpuHz / 10);
    EXPECT_EQ(m.ram1[0], 'T');
    EXPECT_EQ(m.ram1[1], 'S');
    EXPECT_EQ(m.ram1[2], 'T');
    EXPECT_EQ(m.ram1[3], '1');
    EXPECT_EQ(m.ram1[8], 'S');
    EXPECT_EQ(m.ram1[9], 'N');
    EXPECT_GT(m.ram1[4], 0);
    EXPECT_FALSE(m.watchdog_reset);
    EXPECT_GT(m.frames, 0);
    EXPECT_EQ(m.ram1[0x0A], 0xFF);

    std::array<uint32_t, galaga::kUprightW * galaga::kUprightH> rgb{};
    m.render(rgb.data());
    bool any = false;
    for (uint32_t p : rgb) {
        if (p) { any = true; break; }
    }
    EXPECT_TRUE(any);
    bool heard = false;
    for (float s : m.audio) if (s != 0.0f) { heard = true; break; }
    EXPECT_FALSE(heard);
}
