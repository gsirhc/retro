// Board-level smoke: the shared Z80 actually runs this machine's generated
// hardware self-test ROM. ISA coverage lives in retroweb/shared/cpu (zexdoc +
// GoogleTest); this only checks the board boots, writes the RAM signature,
// kicks the watchdog, and paints.

#include <gtest/gtest.h>

#include "hwtest_roms.h"
#include "machine.h"

#include <algorithm>
#include <array>

namespace {

frogger::RomSet test_set() {
    frogger::RomSet s;
    std::copy(frogger::hwtest::program.begin(), frogger::hwtest::program.end(), s.program.begin());
    std::copy(frogger::hwtest::sound.begin(), frogger::hwtest::sound.end(), s.sound.begin());
    std::copy(frogger::hwtest::gfx.begin(), frogger::hwtest::gfx.end(), s.gfx.begin());
    std::copy(frogger::hwtest::color_prom.begin(), frogger::hwtest::color_prom.end(),
              s.color_prom.begin());
    return s;
}

}  // namespace

TEST(Smoke, HwtestBootsSignatureWatchdogAndPaints) {
    frogger::Machine m;
    m.load_roms(test_set());
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
