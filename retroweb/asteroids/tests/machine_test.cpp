#include <gtest/gtest.h>

#include "hwtest_roms.h"
#include "machine.h"

#include <algorithm>

TEST(Machine, MirrorIgnoresA15) {
    asteroids::Machine m;
    m.mem_write(0x0010, 0x5A);
    EXPECT_EQ(m.mem_read(0x8010), 0x5A);
}

// HALT D7=0 when halted (ASTDEC.XX); 10K pull-ups hold D6-D0 high.
TEST(Machine, SwitchReadsDriveD7Only) {
    asteroids::Machine m;
    EXPECT_EQ(m.mem_read(0x2002), 0x7F);
    m.inputs.in1 = 0x20;
    EXPECT_EQ(m.mem_read(0x2405), 0xFF);
    EXPECT_EQ(m.mem_read(0x2406), 0x7F);
}

TEST(Machine, SwitchPortsIgnoreA9ToA3) {
    asteroids::Machine m;
    m.inputs.in1 = 0x08;
    EXPECT_EQ(m.mem_read(0x27FB), 0xFF);
}

TEST(Machine, DswPairsInLowBits) {
    asteroids::Machine m;
    m.inputs.dsw1 = 0xE4;  // pairs: coin=0, right=1, mid=2, lang=3
    EXPECT_EQ(m.mem_read(0x2800), 0xFC);
    EXPECT_EQ(m.mem_read(0x2801), 0xFD);
    EXPECT_EQ(m.mem_read(0x2802), 0xFE);
    EXPECT_EQ(m.mem_read(0x2803), 0xFF);
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
    m.mem_write(0x3C03, 0x7F);
    EXPECT_FALSE(m.sound.latch(3));
    m.mem_write(0x3C03, 0x80);
    EXPECT_TRUE(m.sound.latch(3));
}

TEST(Machine, SoundWriteStrobesDecodeA11ToA9) {
    asteroids::Machine m;
    m.mem_write(0x37FF, 0xC4);
    EXPECT_EQ(m.sound.explode(), 0xC4);
    m.mem_write(0x3A55, 0x1A);
    EXPECT_EQ(m.sound.thump(), 0x1A);
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

TEST(Machine, WatchdogBitesAfter128ThreeKilohertzTicks) {
    asteroids::Machine m;
    m.program.fill(0xEA);
    m.program[0] = 0x4C;
    m.program[1] = 0x00;
    m.program[2] = 0x68;
    m.program[0x7FFC - 0x6800] = 0x00;
    m.program[0x7FFD - 0x6800] = 0x68;
    m.program[0x7FFA - 0x6800] = 0x00;
    m.program[0x7FFB - 0x6800] = 0x68;
    m.reset();
    m.inputs.in0 = 0x80;
    m.run_cycles(asteroids::kCyclesPer3kHz * 120);
    EXPECT_FALSE(m.watchdog_reset);
    m.run_cycles(asteroids::kCyclesPer3kHz * 16);
    EXPECT_TRUE(m.watchdog_reset);
}

TEST(Machine, AudioRunsAtTheRequestedRate) {
    asteroids::Machine m;
    m.audio.clear();
    m.run_cycles(asteroids::kCpuHz / 40);
    EXPECT_NEAR(double(m.audio.size()), m.audio_hz / 40.0, 2);
}

TEST(Machine, HwtestHelpScreenShowsCopyrightPrompt) {
    asteroids::RomSet s;
    std::copy(asteroids::hwtest::program.begin(), asteroids::hwtest::program.end(),
              s.program.begin());
    std::copy(asteroids::hwtest::vector.begin(), asteroids::hwtest::vector.end(),
              s.vector.begin());
    asteroids::Machine m;
    m.load_roms(s);
    m.reset();
    m.run_cycles(asteroids::kCyclesPerNmi * asteroids::hwtest::kHelpWaitNmis);

    for (size_t i = 0; i < asteroids::hwtest::help_vector_prefix.size(); i++) {
        EXPECT_EQ(m.vector_ram[i], asteroids::hwtest::help_vector_prefix[i])
            << "help display-list byte " << i;
    }
}
