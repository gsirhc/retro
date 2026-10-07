#include <gtest/gtest.h>

#include "machine.h"

TEST(Machine, MirrorIgnoresA15) {
    asteroids::Machine m;
    m.mem_write(0x0010, 0x5A);
    EXPECT_EQ(m.mem_read(0x8010), 0x5A);
}

TEST(Machine, HaltBusyPolarity) {
    asteroids::Machine m;
    // Idle → D7 clear; panel reads return $7F when clear (MAME IN0_r).
    EXPECT_EQ(m.mem_read(0x2002), 0x7F);
}

TEST(Machine, DswPairsInLowBits) {
    asteroids::Machine m;
    m.inputs.dsw1 = 0xE4;  // pairs: coin=0, right=1, mid=2, lang=3
    EXPECT_EQ(m.mem_read(0x2800), 0x00);
    EXPECT_EQ(m.mem_read(0x2801), 0x01);
    EXPECT_EQ(m.mem_read(0x2802), 0x02);
    EXPECT_EQ(m.mem_read(0x2803), 0x03);
}

TEST(Machine, RamselSwapsPlayerBanks) {
    asteroids::Machine m;
    m.mem_write(0x0200, 0x11);
    m.mem_write(0x0300, 0x22);
    EXPECT_EQ(m.mem_read(0x0200), 0x11);
    EXPECT_EQ(m.mem_read(0x0300), 0x22);
    m.mem_write(0x3200, 0x04);  // RAMSEL
    EXPECT_TRUE(m.ramsel);
    EXPECT_EQ(m.mem_read(0x0200), 0x22);
    EXPECT_EQ(m.mem_read(0x0300), 0x11);
}

TEST(Machine, SoundLatchUsesD7) {
    asteroids::Machine m;
    m.mem_write(0x3C03, 0x01);  // D7 clear → off
    EXPECT_FALSE(m.thrust);
    m.mem_write(0x3C03, 0x80);
    EXPECT_TRUE(m.thrust);
}

TEST(Machine, SelfTestBlocksNmi) {
    asteroids::Machine m;
    m.program.fill(0xEA);
    m.program[0] = 0x4C;
    m.program[1] = 0x00;
    m.program[2] = 0x68;  // JMP $6800
    m.program[0x7FFC - 0x6800] = 0x00;
    m.program[0x7FFD - 0x6800] = 0x68;
    m.program[0x7FFA - 0x6800] = 0x00;
    m.program[0x7FFB - 0x6800] = 0x40;
    m.program[0x7FFA - 0x6800] = 0x03;
    m.program[0x7FFB - 0x6800] = 0x68;
    m.program[3] = 0x40;  // RTI
    m.reset();
    uint64_t before = m.cpu.cycles;
    m.inputs.in0 = 0x80;  // self-test
    m.run_cycles(asteroids::kCyclesPerNmi * 4);
    EXPECT_GE(m.cpu.pc, 0x6800);
    EXPECT_LT(m.cpu.pc, 0x6810);
    EXPECT_GT(m.cpu.cycles, before);
    EXPECT_GT(m.frames, 0);
}

TEST(Machine, WatchdogKickPreventsReset) {
    asteroids::Machine m;
    m.program.fill(0xEA);
    m.program[0] = 0x8D;
    m.program[1] = 0x00;
    m.program[2] = 0x34;
    m.program[3] = 0x4C;
    m.program[4] = 0x00;
    m.program[5] = 0x68;
    m.program[0x7FFC - 0x6800] = 0x00;
    m.program[0x7FFD - 0x6800] = 0x68;
    m.program[0x7FFA - 0x6800] = 0x00;
    m.program[0x7FFB - 0x6800] = 0x68;
    m.program[0x7FFE - 0x6800] = 0x00;
    m.program[0x7FFF - 0x6800] = 0x68;
    m.reset();
    m.run_cycles(asteroids::kCpuHz);
    EXPECT_FALSE(m.watchdog_reset);
}

namespace {

void parked(asteroids::Machine& m) {
    m.program.fill(0xEA);
    m.program[0] = 0x8D;
    m.program[1] = 0x00;
    m.program[2] = 0x34;  // STA $3400, kick the watchdog
    m.program[3] = 0x4C;
    m.program[4] = 0x00;
    m.program[5] = 0x68;  // JMP $6800
    m.program[6] = 0x40;  // RTI
    m.program[0x7FFA - 0x6800] = 0x06;
    m.program[0x7FFB - 0x6800] = 0x68;
    m.program[0x7FFC - 0x6800] = 0x00;
    m.program[0x7FFD - 0x6800] = 0x68;
    m.reset();
}

// Rising-edge rate after the filter has settled.
float rising_hz(const asteroids::Machine& m, int skip) {
    int crossings = 0;
    for (size_t i = size_t(skip) + 1; i < m.audio.size(); i++) {
        if (m.audio[i - 1] <= 0.0f && m.audio[i] > 0.0f) crossings++;
    }
    float seconds = float(m.audio.size() - size_t(skip)) / float(m.audio_hz);
    return float(crossings) / seconds;
}

}  // namespace

TEST(Machine, ThumpNibble0IsAbout82Hz) {
    asteroids::Machine m;
    parked(m);
    m.mem_write(0x3A00, 0x10);  // enable, nibble 0 (the gameplay beat)
    m.audio.clear();
    m.run_cycles(asteroids::kCpuHz / 4);
    float hz = rising_hz(m, m.audio_hz / 50);
    EXPECT_GT(hz, 70.0f);
    EXPECT_LT(hz, 95.0f);
}

TEST(Machine, ThumpHigherNibbleIsLower) {
    asteroids::Machine low;
    parked(low);
    low.mem_write(0x3A00, 0x1F);  // nibble 15, slowest 555 code
    low.audio.clear();
    low.run_cycles(asteroids::kCpuHz / 4);
    asteroids::Machine high;
    parked(high);
    high.mem_write(0x3A00, 0x10);
    high.audio.clear();
    high.run_cycles(asteroids::kCpuHz / 4);
    EXPECT_LT(rising_hz(low, low.audio_hz / 50), rising_hz(high, high.audio_hz / 50));
}

TEST(Machine, LifeToneFollowsTheLatch) {
    asteroids::Machine m;
    parked(m);
    m.mem_write(0x3C05, 0x80);
    m.audio.clear();
    m.run_cycles(asteroids::kCpuHz / 20);
    float hz = rising_hz(m, m.audio_hz / 100);
    EXPECT_GT(hz, 2800.0f);
    EXPECT_LT(hz, 3200.0f);

    m.mem_write(0x3C05, 0x00);
    m.audio.clear();
    m.run_cycles(asteroids::kCpuHz / 10);
    float peak = 0;
    for (float s : m.audio) {
        float a = s < 0 ? -s : s;
        if (a > peak) peak = a;
    }
    EXPECT_LT(peak, 0.01f);
}

TEST(Machine, SmallSaucerSitsAbove750Hz) {
    asteroids::Machine m;
    parked(m);
    m.mem_write(0x3C00, 0x80);  // saucer on, SEL clear = small
    m.audio.clear();
    m.run_cycles(asteroids::kCpuHz / 5);
    EXPECT_GT(rising_hz(m, m.audio_hz / 50), 700.0f);
}
