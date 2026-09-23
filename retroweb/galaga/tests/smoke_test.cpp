#include <gtest/gtest.h>

#include "hwtest_roms.h"
#include "machine.h"

#include <algorithm>
#include <array>

namespace {

galaga::RomSet test_set() {
    galaga::RomSet s;
    auto copy = [](const auto& src, auto& dst) {
        std::copy(src.begin(), src.end(), dst.begin());
    };
    copy(galaga::hwtest::main, s.main);
    copy(galaga::hwtest::sub, s.sub);
    copy(galaga::hwtest::sound, s.sound);
    copy(galaga::hwtest::tiles, s.tiles);
    copy(galaga::hwtest::sprites, s.sprites);
    copy(galaga::hwtest::palette, s.palette);
    copy(galaga::hwtest::char_lut, s.char_lut);
    copy(galaga::hwtest::sprite_lut, s.sprite_lut);
    copy(galaga::hwtest::wave, s.wave);
    return s;
}

}  // namespace

TEST(Smoke, HwtestBootsSignatureWatchdogAndPaints) {
    galaga::Machine m;
    m.load_roms(test_set());
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
