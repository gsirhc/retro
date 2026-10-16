#include <gtest/gtest.h>

#include "hwtest_roms.h"
#include "hwtest_set.h"
#include "machine.h"

#include <algorithm>
#include <array>

namespace {


void run_frames(missile::Machine& m, int n) {
    for (int i = 0; i < n; i++) m.run_cycles(missile::kCyclesPerFrame);
}

template <size_t N, size_t M>
void expect_probe(const missile::Machine& m, const std::array<int, N>& offs,
                  const std::array<uint8_t, M>& vals) {
    static_assert(N == M, "probe arrays differ");
    for (size_t i = 0; i < N; i++) EXPECT_EQ(m.vram[size_t(offs[i])], vals[i]) << i;
}

}  // namespace

TEST(Smoke, HwtestBootsSignatureWatchdogAndPaints) {
    missile::Machine m;
    m.load_roms(missile::hwtest_set());
    m.reset();
    m.run_cycles(missile::kCpuHz / 10);
    EXPECT_EQ(m.mem_read(0xF0), 'M');
    EXPECT_EQ(m.mem_read(0xF1), 'I');
    EXPECT_EQ(m.mem_read(0xF2), 'S');
    EXPECT_EQ(m.mem_read(0xF3), '1');
    EXPECT_FALSE(m.watchdog_reset);
    EXPECT_GT(m.frames, 0);

    std::array<uint32_t, missile::kFbW * missile::kFbH> fb{};
    m.render(fb.data());
    int lit = 0;
    for (uint32_t p : fb)
        if ((p & 0xFFFFFF) != 0) lit++;
    EXPECT_GT(lit, 400);
}

TEST(Smoke, HwtestHoldsHelpScreen) {
    missile::Machine m;
    m.load_roms(missile::hwtest_set());
    m.reset();
    run_frames(m, 30);
    uint8_t probe = m.vram[size_t(missile::hwtest::kMadselProbeOff)];
    EXPECT_EQ(probe & missile::hwtest::kMadselProbeMask, missile::hwtest::kMadselProbeMask);

    run_frames(m, missile::hwtest::kHelpWaitFrames - 30);
    EXPECT_FALSE(m.watchdog_reset);
    EXPECT_EQ(m.mem_read(0xF6), 0xFF);
    expect_probe(m, missile::hwtest::help_title_offs, missile::hwtest::help_title_bytes);
    expect_probe(m, missile::hwtest::help_copy_offs, missile::hwtest::help_copy_bytes);

    run_frames(m, 20);
    EXPECT_FALSE(m.watchdog_reset);
    expect_probe(m, missile::hwtest::help_title_offs, missile::hwtest::help_title_bytes);
}

TEST(Smoke, HwtestHiscoreStandInLands) {
    missile::Machine m;
    m.load_roms(missile::hwtest_set());
    m.reset();
    run_frames(m, missile::hwtest::kHelpWaitFrames);
    const auto& rom = missile::hwtest::program;
    for (int i = 0; i < 24; i++) {
        EXPECT_EQ(m.mem_read(uint16_t(missile::kHiscoreAddr + i)),
                  rom[size_t(missile::hwtest::kInitialsRom - 0x5000 + i)]) << i;
        EXPECT_EQ(m.mem_read(uint16_t(0x44 + i)),
                  rom[size_t(missile::hwtest::kScoresRom - 0x5000 + i)]) << i;
    }
}
