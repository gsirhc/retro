#include <gtest/gtest.h>

#include "hwtest_roms.h"
#include "machine.h"

#include <algorithm>

TEST(Machine, MirrorMasksTo14Bits) {
    centipede::Machine m;
    m.program[0] = 0xA5;
    EXPECT_EQ(m.mem_read(0x2000), 0xA5);
    EXPECT_EQ(m.mem_read(0x6000), 0xA5);
    EXPECT_EQ(m.mem_read(0xA000), 0xA5);
    EXPECT_EQ(m.mem_read(0xE000), 0xA5);
}

TEST(Machine, In1IdleIsActiveLowOnes) {
    centipede::Machine m;
    EXPECT_EQ(m.mem_read(0x0C01), 0xFF);
    m.inputs.in1 = 0xFB;  // fire pressed
    EXPECT_EQ(m.mem_read(0x0C01), 0xFB);
}

TEST(Machine, DswPortsAt0800) {
    centipede::Machine m;
    m.inputs.dsw1 = 0x12;
    m.inputs.dsw2 = 0x34;
    EXPECT_EQ(m.mem_read(0x0800), 0x12);
    EXPECT_EQ(m.mem_read(0x0801), 0x34);
}

TEST(Machine, IrqAckClearsLine) {
    centipede::Machine m;
    m.cpu.irq_line = true;
    m.mem_write(0x1800, 0x00);
    EXPECT_FALSE(m.cpu.irq_line);
}

TEST(Machine, WatchdogKickPreventsReset) {
    centipede::Machine m;
    // Without kicks, 8 frames → reset. Kick every few lines via $2000.
    for (int i = 0; i < 20; i++) {
        m.mem_write(0x2000, 0);
        m.run_cycles(centipede::kCyclesPerFrame);
    }
    EXPECT_FALSE(m.watchdog_reset);
}

TEST(Machine, PokeyRandomReads) {
    centipede::Machine m;
    // SKCTL reset → RANDOM is 0xFF.
    m.mem_write(0x100F, 0x00);
    EXPECT_EQ(m.mem_read(0x100A), 0xFF);
}

TEST(Machine, PokeyRandomLeavesInitMode) {
    centipede::Machine m;
    m.mem_write(0x100F, 0x00);  // init
    m.mem_write(0x100F, 0x03);  // leave init — must re-seed the LFSR
    m.run_cycles(200);
    uint8_t a = m.mem_read(0x100A);
    m.run_cycles(200);
    uint8_t b = m.mem_read(0x100A);
    m.run_cycles(200);
    uint8_t c = m.mem_read(0x100A);
    // Not stuck at the init-mode constant, and not a frozen LFSR.
    EXPECT_FALSE(a == 0xFF && b == 0xFF && c == 0xFF);
    EXPECT_FALSE(a == b && b == c);
}

TEST(Machine, EaromEraseThenAndWrite) {
    centipede::Machine m;
    // WriteEaromInc: deselect, latch the byte, then select erase ($06/$0E).
    m.mem_write(0x1603, 0xAB);
    m.mem_write(0x1680, 0x06);
    m.mem_write(0x1680, 0x0E);
    EXPECT_EQ(m.earom[3], 0xFF);
    // Next call selects write ($02/$0A). The cell ANDs in the latched byte.
    m.mem_write(0x1680, 0x02);
    m.mem_write(0x1680, 0x0A);
    EXPECT_EQ(m.earom[3], 0xAB);
    // A write without a fresh erase can only clear bits.
    m.mem_write(0x1603, 0x0F);
    m.mem_write(0x1680, 0x02);
    m.mem_write(0x1680, 0x0A);
    EXPECT_EQ(m.earom[3], 0x0B);
    // A zero cell stays zero if the erase is skipped.
    m.mem_write(0x1604, 0xAB);
    m.mem_write(0x1680, 0x02);
    m.mem_write(0x1680, 0x0A);
    EXPECT_EQ(m.earom[4], 0x00);
    // ReadEarom latches on CK falling ($08, $09, $08), then reads $1700.
    m.mem_write(0x1603, 0x00);
    m.mem_write(0x1680, 0x08);
    m.mem_write(0x1680, 0x09);
    EXPECT_EQ(m.mem_read(0x1700), 0x00);
    m.mem_write(0x1680, 0x08);
    EXPECT_EQ(m.mem_read(0x1700), 0x0B);
}

TEST(Machine, Ls259FlipUsesD7) {
    centipede::Machine m;
    m.mem_write(0x1C07, 0x00);
    EXPECT_FALSE(m.flipscreen);
    m.mem_write(0x1C07, 0x80);
    EXPECT_TRUE(m.flipscreen);
}

TEST(Machine, HwtestHelpScreenShowsCopyrightPrompt) {
    centipede::RomSet s;
    std::copy(centipede::hwtest::program.begin(), centipede::hwtest::program.end(),
              s.program.begin());
    std::copy(centipede::hwtest::gfx.begin(), centipede::hwtest::gfx.end(), s.gfx.begin());
    centipede::Machine m;
    m.load_roms(s);
    m.reset();
    m.run_cycles(centipede::kCyclesPerFrame * centipede::hwtest::kHelpWaitFrames);

    for (size_t i = 0; i < centipede::hwtest::help_title_codes.size(); i++) {
        EXPECT_EQ(m.videoram[unsigned(centipede::hwtest::help_title_offs[i])],
                  centipede::hwtest::help_title_codes[i])
            << "help title char " << i;
    }
    for (size_t i = 0; i < centipede::hwtest::help_copy_codes.size(); i++) {
        EXPECT_EQ(m.videoram[unsigned(centipede::hwtest::help_copy_offs[i])],
                  centipede::hwtest::help_copy_codes[i])
            << "copyright line char " << i;
    }
}
