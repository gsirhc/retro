#include <gtest/gtest.h>

#include "hwtest_roms.h"
#include "machine.h"

#include <array>

namespace {

asteroids::RomSet test_set() {
    asteroids::RomSet s;
    std::copy(asteroids::hwtest::program.begin(), asteroids::hwtest::program.end(),
              s.program.begin());
    std::copy(asteroids::hwtest::vector.begin(), asteroids::hwtest::vector.end(),
              s.vector.begin());
    return s;
}

}  // namespace

TEST(Smoke, HwtestBootsSignatureWatchdogAndPaints) {
    asteroids::Machine m;
    m.load_roms(test_set());
    m.reset();
    m.run_cycles(asteroids::kCpuHz / 10);
    EXPECT_EQ(m.mem_read(0), 'A');
    EXPECT_EQ(m.mem_read(1), 'S');
    EXPECT_EQ(m.mem_read(2), 'T');
    EXPECT_EQ(m.mem_read(3), '1');
    EXPECT_FALSE(m.watchdog_reset);
    EXPECT_GT(m.frames, 0);

    std::array<uint32_t, asteroids::kFbW * asteroids::kFbH> fb{};
    m.render(fb.data());
    int lit = 0;
    for (uint32_t p : fb)
        if ((p & 0xFFFFFF) != 0) lit++;
    EXPECT_GT(lit, 400);
}
