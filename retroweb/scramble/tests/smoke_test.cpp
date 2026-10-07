// Board smoke: the generated self-test ROM boots, writes the RAM signature, kicks the watchdog, paints.

#include <gtest/gtest.h>

#include "hwtest_roms.h"
#include "machine.h"

#include <algorithm>
#include <array>

namespace {

scramble::RomSet test_set() {
    scramble::RomSet s;
    std::copy(scramble::hwtest::program.begin(), scramble::hwtest::program.end(), s.program.begin());
    std::copy(scramble::hwtest::sound.begin(), scramble::hwtest::sound.end(), s.sound.begin());
    std::copy(scramble::hwtest::gfx.begin(), scramble::hwtest::gfx.end(), s.gfx.begin());
    std::copy(scramble::hwtest::color_prom.begin(), scramble::hwtest::color_prom.end(),
              s.color_prom.begin());
    return s;
}

}  // namespace

TEST(Smoke, HwtestBootsSignatureWatchdogAndPaints) {
    scramble::Machine m;
    m.load_roms(test_set());
    m.reset();
    m.run_cycles(scramble::kCpuHz / 10);
    EXPECT_EQ(m.ram[0], 'T');
    EXPECT_EQ(m.ram[1], 'S');
    EXPECT_EQ(m.ram[2], 'T');
    EXPECT_EQ(m.ram[3], '1');
    EXPECT_FALSE(m.watchdog_reset);
    EXPECT_GT(m.frames, 0);

    std::array<uint32_t, scramble::kUprightW * scramble::kUprightH> rgb{};
    m.render(rgb.data());
    bool any = false;
    for (uint32_t p : rgb) {
        if (p) { any = true; break; }
    }
    EXPECT_TRUE(any);
}
