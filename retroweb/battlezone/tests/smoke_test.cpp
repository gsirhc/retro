#include <gtest/gtest.h>

#include "hwtest_roms.h"
#include "hwtest_set.h"
#include "machine.h"

#include <vector>

namespace {

uint16_t vram_word(const battlezone::Machine& m, int i) {
    return uint16_t(m.vector_ram[size_t(2 * i)] | (m.vector_ram[size_t(2 * i + 1)] << 8));
}

}  // namespace

TEST(Smoke, HwtestBootsSignatureWatchdogAndPaints) {
    battlezone::Machine m;
    m.load_roms(battlezone::hwtest_set());
    m.run_cycles(battlezone::kCpuHz / 10);
    EXPECT_EQ(m.mem_read(0), 'B');
    EXPECT_EQ(m.mem_read(1), 'Z');
    EXPECT_EQ(m.mem_read(2), 'N');
    EXPECT_EQ(m.mem_read(3), '1');
    EXPECT_FALSE(m.watchdog_reset);
    EXPECT_GT(m.avg.frames, 0);

    std::vector<uint32_t> fb(size_t(battlezone::kFbW) * battlezone::kFbH);
    m.render(fb.data());
    int lit = 0;
    for (uint32_t p : fb)
        if ((p & 0xFFFFFF) != 0) lit++;
    EXPECT_GT(lit, 400);
}

TEST(Smoke, MathBoxMicrocodeRunsAndThePageWaitsOnBusy) {
    battlezone::Machine m;
    m.load_roms(battlezone::hwtest_set());
    m.run_cycles(battlezone::kCpuHz / 10);
    EXPECT_EQ(m.ram[0x10], 1);
    EXPECT_GT(m.ram[0x11], 0);
    EXPECT_EQ(m.ram[0x12] << 8 | m.ram[0x13], battlezone::hwtest::kMulHi);
    EXPECT_EQ(m.ram[0x14] << 8 | m.ram[0x15], battlezone::hwtest::kMulLo);
    EXPECT_EQ(m.ram[0x16] << 8 | m.ram[0x17], battlezone::hwtest::kMax);
}

TEST(Smoke, HelpScreenFollowsTheDiagnostic) {
    battlezone::Machine m;
    m.load_roms(battlezone::hwtest_set());
    m.run_cycles(battlezone::kCyclesPerNmi * 40);
    EXPECT_EQ(vram_word(m, 0), 0xE000 | battlezone::hwtest::kDiagWord);
    m.run_cycles(battlezone::kCyclesPerNmi * battlezone::hwtest::kHelpWaitNmis);
    EXPECT_EQ(vram_word(m, 0), 0xA000 | battlezone::hwtest::kHelpWord);
    EXPECT_EQ(vram_word(m, 1), 0xA000 | battlezone::hwtest::kMbOkWord);
    EXPECT_EQ(vram_word(m, 2), 0x2000);
    EXPECT_GT(m.avg.frame.size(), 300u);
    EXPECT_FALSE(m.watchdog_reset);
}
