#include <gtest/gtest.h>

#include "hwtest_roms.h"
#include "hwtest_set.h"
#include "machine.h"

#include <cstdint>

namespace {

// NOP sled at $5000 with an NMI handler that counts into $00.
battlezone::RomSet sled() {
    battlezone::RomSet s = battlezone::hwtest_set();
    s.program.fill(0xEA);
    s.program[0x0000] = 0x8D;  // STA $1400
    s.program[0x0001] = 0x00;
    s.program[0x0002] = 0x14;
    s.program[0x0003] = 0x4C;  // JMP $5000
    s.program[0x0004] = 0x00;
    s.program[0x0005] = 0x50;
    s.program[0x0100] = 0xE6;  // INC $00
    s.program[0x0101] = 0x00;
    s.program[0x0102] = 0x40;  // RTI
    s.program[0x2FFA] = 0x00;
    s.program[0x2FFB] = 0x51;
    s.program[0x2FFC] = 0x00;
    s.program[0x2FFD] = 0x50;
    return s;
}

}  // namespace

TEST(Machine, A15IsIgnored) {
    battlezone::Machine m;
    m.mem_write(0x0123, 0x5A);
    EXPECT_EQ(m.mem_read(0x8123), 0x5A);
    m.load_roms(battlezone::hwtest_set());
    EXPECT_EQ(m.mem_read(0xFFFC), m.mem_read(0x7FFC));
    EXPECT_EQ(m.cpu.pc, uint16_t(m.mem_read(0x7FFC) | (m.mem_read(0x7FFD) << 8)));
}

TEST(Machine, MemoryMap) {
    battlezone::Machine m;
    m.load_roms(battlezone::hwtest_set());
    EXPECT_EQ(m.mem_read(0x3000), battlezone::hwtest::vector[0]);
    EXPECT_EQ(m.mem_read(0x5000), battlezone::hwtest::program[0]);
    m.mem_write(0x2FFF, 0x77);
    EXPECT_EQ(m.vector_ram[0x0FFF], 0x77);
    m.mem_write(0x5000, 0x00);
    EXPECT_EQ(m.mem_read(0x5000), battlezone::hwtest::program[0]);
    m.mem_write(0x03FF, 0x11);
    EXPECT_EQ(m.mem_read(0x03FF), 0x11);
    EXPECT_EQ(m.mem_read(0x0400), 0x00);
}

TEST(Machine, In0SwitchesAreActiveLow) {
    battlezone::Machine m;
    EXPECT_EQ(m.mem_read(0x0800) & 0x3F, 0x3F);
    m.inputs.in0 = 0x3F & ~0x01;
    EXPECT_EQ(m.mem_read(0x0800) & 0x3F, 0x3E);
}

TEST(Machine, In0D6IsAvgHalt) {
    battlezone::Machine m;
    m.load_roms(battlezone::hwtest_set());
    EXPECT_EQ(m.mem_read(0x0800) & 0x40, 0x40);
    m.mem_write(0x2000, 0x00);
    m.mem_write(0x2001, 0x00);  // VCTR
    m.mem_write(0x2002, 0x00);
    m.mem_write(0x2003, 0x0F);
    m.mem_write(0x1200, 0);
    EXPECT_EQ(m.mem_read(0x0800) & 0x40, 0x00);
    m.mem_write(0x1600, 0);
    EXPECT_EQ(m.mem_read(0x0800) & 0x40, 0x40);
}

TEST(Machine, In0D7Is3kHzSquare) {
    battlezone::Machine m;
    m.load_roms(sled());
    uint8_t first = m.mem_read(0x0800) & 0x80;
    int flips = 0;
    while (m.cpu.cycles < 5000) {
        m.run_cycles(1);
        uint8_t now = m.mem_read(0x0800) & 0x80;
        if (now != first) {
            flips++;
            first = now;
        }
    }
    // 256 CPU cycles per half period, 2953 Hz.
    EXPECT_NEAR(flips, 5000 / 256, 2);
}

TEST(Machine, OptionSwitches) {
    battlezone::Machine m;
    m.inputs.dsw0 = 0xA5;
    m.inputs.dsw1 = 0x5A;
    EXPECT_EQ(m.mem_read(0x0A00), 0xA5);
    EXPECT_EQ(m.mem_read(0x0C00), 0x5A);
}

TEST(Machine, TreadsAndButtonsReadThroughAllpot) {
    battlezone::Machine m;
    m.load_roms(sled());
    m.inputs.in3 = 0x2A;
    m.run_cycles(10);
    EXPECT_EQ(m.mem_read(0x1828), 0x2A);
}

TEST(Machine, MathboxPorts) {
    battlezone::Machine m;
    m.load_roms(battlezone::hwtest_set());
    m.mem_write(0x1860, 0x34);
    EXPECT_EQ(m.mem_read(0x1800), 0x80);
    m.run_cycles(4);
    EXPECT_EQ(m.mem_read(0x1800), 0x00);
    m.mem_write(0x1861, 0x12);
    m.run_cycles(4);
    EXPECT_EQ(m.mem_read(0x1810), 0x34);
    EXPECT_EQ(m.mem_read(0x1818), 0x12);
}

TEST(Machine, SoundLatchDrivesTheStartLamp) {
    battlezone::Machine m;
    m.mem_write(0x1840, 0x40);
    EXPECT_TRUE(m.start_lamp());
    EXPECT_EQ(m.sound.latch, 0x40);
    m.mem_write(0x1840, 0x00);
    EXPECT_FALSE(m.start_lamp());
}

TEST(Machine, CoinCounterLatch) {
    battlezone::Machine m;
    m.mem_write(0x1000, 0x03);
    EXPECT_EQ(m.coin_counters, 0x03);
}

TEST(Machine, NmiIs3kHzOver12) {
    battlezone::Machine m;
    m.load_roms(sled());
    m.run_cycles(battlezone::kCpuHz);
    EXPECT_NEAR(m.ram[0], 246 & 0xFF, 1);
    EXPECT_NEAR(m.nmis, 246, 1);
}

TEST(Machine, SelfTestSwitchBlocksNmi) {
    battlezone::Machine m;
    m.load_roms(sled());
    m.inputs.in0 &= ~0x10;
    m.run_cycles(battlezone::kCpuHz / 10);
    EXPECT_EQ(m.ram[0], 0);
    m.inputs.in0 |= 0x10;
    m.run_cycles(battlezone::kCpuHz / 10);
    EXPECT_GT(m.ram[0], 20);
}

TEST(Machine, WatchdogResetsAStarvedBoard) {
    battlezone::Machine m;
    auto s = sled();
    s.program[0x0000] = 0xEA;  // drop the STA $1400
    s.program[0x0001] = 0xEA;
    s.program[0x0002] = 0xEA;
    m.load_roms(s);
    m.run_cycles(battlezone::kWatchdogCycles - 1000);
    EXPECT_FALSE(m.watchdog_reset);
    m.run_cycles(2000);
    EXPECT_TRUE(m.watchdog_reset);
}

TEST(Machine, FedWatchdogNeverFires) {
    battlezone::Machine m;
    m.load_roms(sled());
    m.run_cycles(battlezone::kWatchdogCycles * 3);
    EXPECT_FALSE(m.watchdog_reset);
}

TEST(Machine, AudioRunsAtTheRequestedRate) {
    battlezone::Machine m;
    m.load_roms(sled());
    m.audio_hz = 44100;
    m.run_cycles(battlezone::kCpuHz / 10);
    EXPECT_NEAR(double(m.audio.size()), 4410, 2);
}

TEST(Machine, OverlayTintsTheTopFifthRed) {
    battlezone::Machine m;
    m.avg.frame.push_back({10, 10, 100, 10, 15});
    m.avg.frame.push_back({10, 300, 100, 300, 15});
    std::vector<uint32_t> fb(size_t(battlezone::kFbW) * battlezone::kFbH);
    m.render(fb.data());
    uint32_t top = fb[size_t(10 * battlezone::kFbW + 50)];
    uint32_t low = fb[size_t(300 * battlezone::kFbW + 50)];
    EXPECT_GT(top & 0xFF, (top >> 8) & 0xFF);
    EXPECT_GT((low >> 8) & 0xFF, low & 0xFF);
}
