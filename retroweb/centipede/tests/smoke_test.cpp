#include <gtest/gtest.h>

#include "hwtest_roms.h"
#include "machine.h"

#include <array>

namespace {

centipede::RomSet test_set() {
    centipede::RomSet s;
    std::copy(centipede::hwtest::program.begin(), centipede::hwtest::program.end(),
              s.program.begin());
    std::copy(centipede::hwtest::gfx.begin(), centipede::hwtest::gfx.end(),
              s.gfx.begin());
    return s;
}

}  // namespace

TEST(Smoke, HwtestBootsSignatureWatchdogAndPaints) {
    centipede::Machine m;
    m.load_roms(test_set());
    m.reset();
    m.run_cycles(centipede::kCpuHz / 10);
    EXPECT_EQ(m.mem_read(0), 'C');
    EXPECT_EQ(m.mem_read(1), 'E');
    EXPECT_EQ(m.mem_read(2), 'N');
    EXPECT_EQ(m.mem_read(3), '1');
    EXPECT_FALSE(m.watchdog_reset);
    EXPECT_GT(m.frames, 0);

    std::array<uint32_t, centipede::kUprightW * centipede::kUprightH> fb{};
    m.render(fb.data());
    int lit = 0;
    for (uint32_t p : fb)
        if ((p & 0xFFFFFF) != 0) lit++;
    EXPECT_GT(lit, 400);
}
