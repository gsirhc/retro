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

pacman::RomSet test_set() {
    pacman::RomSet s;
    std::copy(pacman::hwtest::program.begin(), pacman::hwtest::program.end(), s.program.begin());
    std::copy(pacman::hwtest::tiles.begin(), pacman::hwtest::tiles.end(), s.tiles.begin());
    std::copy(pacman::hwtest::sprites.begin(), pacman::hwtest::sprites.end(), s.sprites.begin());
    std::copy(pacman::hwtest::color_prom.begin(), pacman::hwtest::color_prom.end(), s.color_prom.begin());
    std::copy(pacman::hwtest::lookup_prom.begin(), pacman::hwtest::lookup_prom.end(), s.lookup_prom.begin());
    std::copy(pacman::hwtest::wave_prom.begin(), pacman::hwtest::wave_prom.end(), s.wave_prom.begin());
    return s;
}

}  // namespace

TEST(Smoke, HwtestBootsSignatureWatchdogAndPaints) {
    pacman::Machine m;
    m.load_roms(test_set());
    m.reset();
    m.run_cycles(pacman::kCpuHz / 10);
    EXPECT_EQ(m.ram[0x4C00 - 0x4800], 'T');
    EXPECT_EQ(m.ram[0x4C01 - 0x4800], 'S');
    EXPECT_EQ(m.ram[0x4C02 - 0x4800], 'T');
    EXPECT_EQ(m.ram[0x4C03 - 0x4800], '1');
    EXPECT_FALSE(m.watchdog_reset);
    EXPECT_GT(m.frames, 0);

    std::array<uint32_t, pacman::kUprightW * pacman::kUprightH> fb{};
    m.render(fb.data());
    int lit = 0;
    for (uint32_t p : fb)
        if (p != 0) lit++;
    EXPECT_GT(lit, 1000);
}
