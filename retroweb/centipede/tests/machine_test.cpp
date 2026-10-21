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
    m.mem_write(0x100F, 0x03);  // leave init, must re-seed the LFSR
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

namespace {

uint32_t rgba(int r, int g, int b) {
    return 0xFF000000u | uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16);
}

struct Frame {
    std::array<uint32_t, centipede::kUprightW * centipede::kUprightH> px{};
    explicit Frame(const centipede::Machine& m) { m.render(px.data()); }
    uint32_t at(int x, int y) const {
        return px[size_t((centipede::kFbW - 1 - x) * centipede::kUprightW + y)];
    }
};

void set_pf_colours(centipede::Machine& m) {
    m.paletteram[0x04] = 0x0F;
    m.paletteram[0x05] = 0x0E;
    m.paletteram[0x06] = 0x0D;
    m.paletteram[0x07] = 0x0B;
}

void set_mo_colours(centipede::Machine& m) {
    m.paletteram[0x0D] = 0x0E;
    m.paletteram[0x0E] = 0x0D;
    m.paletteram[0x0F] = 0x0B;
}

}  // namespace

TEST(Video, ColourRamIsActiveLowRedGreenBlue) {
    centipede::Machine m;
    const struct { uint8_t cell; uint32_t want; } cases[] = {
        {0x0F, rgba(0, 0, 0)},
        {0x0E, rgba(255, 0, 0)},
        {0x0D, rgba(0, 255, 0)},
        {0x0B, rgba(0, 0, 255)},
        {0x08, rgba(255, 255, 255)},
    };
    for (const auto& c : cases) {
        m.paletteram[0x04] = c.cell;
        EXPECT_EQ(Frame(m).at(0, 0), c.want) << "cell " << int(c.cell);
    }
}

TEST(Video, AlternateBitDimsBlueWhileBlueIsLit) {
    centipede::Machine m;
    m.paletteram[0x04] = 0x03;
    uint32_t p = Frame(m).at(0, 0);
    int blue = int((p >> 16) & 0xFF);
    EXPECT_GT(blue, 100);
    EXPECT_LT(blue, 160);
    EXPECT_EQ(p & 0xFFFF, 0u);
    m.paletteram[0x04] = 0x01;
    uint32_t q = Frame(m).at(0, 0);
    EXPECT_EQ((q >> 16) & 0xFF, uint32_t(blue));
    EXPECT_EQ((q >> 8) & 0xFF, 255u);
}

TEST(Video, AlternateBitDimsGreenOnlyWhileBlueIsDark) {
    centipede::Machine m;
    m.paletteram[0x04] = 0x05;
    uint32_t p = Frame(m).at(0, 0);
    int green = int((p >> 8) & 0xFF);
    EXPECT_GT(green, 100);
    EXPECT_LT(green, 160);
    m.paletteram[0x04] = 0x04;
    uint32_t q = Frame(m).at(0, 0);
    EXPECT_EQ((q >> 8) & 0xFF, uint32_t(green));
    EXPECT_EQ(q & 0xFF, 255u);
    m.paletteram[0x04] = 0x0C;
    EXPECT_EQ(Frame(m).at(0, 0), rgba(255, 255, 0));
}

TEST(Video, AlternateBitLeavesRedAndDarkGunsAlone) {
    centipede::Machine m;
    m.paletteram[0x04] = 0x06;
    EXPECT_EQ(Frame(m).at(0, 0), rgba(255, 0, 0));
    m.paletteram[0x04] = 0x07;
    EXPECT_EQ(Frame(m).at(0, 0), rgba(0, 0, 0));
}

TEST(Video, PlayfieldTileUsesCodeOffsetAndPlanes) {
    centipede::Machine m;
    set_pf_colours(m);
    m.videoram[0] = 0x05;
    m.gfx[0x200 + 5 * 8 + 2] = 0x80;
    m.gfx[0x800 + 0x200 + 5 * 8 + 2] = 0x40;
    Frame f(m);
    EXPECT_EQ(f.at(0, 2), rgba(255, 0, 0));
    EXPECT_EQ(f.at(1, 2), rgba(0, 255, 0));
    EXPECT_EQ(f.at(2, 2), rgba(0, 0, 0));
    EXPECT_EQ(f.at(0, 1), rgba(0, 0, 0));
}

TEST(Video, PlayfieldBit6FlipsHorizontallyBit7FlipsLinesNotTile) {
    centipede::Machine m;
    set_pf_colours(m);
    m.gfx[0x200 + 5 * 8 + 2] = 0x80;
    m.gfx[0x200 + 4 * 8 + 5] = 0x01;
    m.videoram[0] = 0x45;
    EXPECT_EQ(Frame(m).at(7, 2), rgba(255, 0, 0));
    EXPECT_EQ(Frame(m).at(0, 2), rgba(0, 0, 0));
    m.videoram[0] = 0x85;
    Frame f(m);
    EXPECT_EQ(f.at(0, 5), rgba(255, 0, 0));
    EXPECT_EQ(f.at(7, 5), rgba(0, 0, 0));
}

TEST(Video, PlayfieldRowsAndColumnsAre32PerRow) {
    centipede::Machine m;
    set_pf_colours(m);
    m.gfx[0x200 + 3 * 8] = 0x80;
    m.videoram[2 * 32 + 5] = 0x03;
    EXPECT_EQ(Frame(m).at(5 * 8, 2 * 8), rgba(255, 0, 0));
}

TEST(Video, ObjectUsesPicBitsLineFromVerticalSumAndIndirectColour) {
    centipede::Machine m;
    set_pf_colours(m);
    set_mo_colours(m);
    m.spriteram[0] = 0x01;
    m.spriteram[0x10] = 220;
    m.spriteram[0x20] = 40;
    m.spriteram[0x30] = 0x1E;
    m.gfx[0x400 + 3] = 0x80;
    m.gfx[0x800 + 0x400 + 3] = 0x40;
    m.gfx[0x400 + 3 + 1] = 0x20;
    m.gfx[0x800 + 0x400 + 3 + 1] = 0x20;
    Frame f(m);
    EXPECT_EQ(f.at(40, 23), rgba(0, 255, 0));
    EXPECT_EQ(f.at(41, 23), rgba(0, 0, 255));
    EXPECT_EQ(f.at(42, 24), rgba(255, 0, 0));
    EXPECT_EQ(f.at(40, 22), rgba(0, 0, 0));
}

TEST(Video, ObjectFlipsFollowPicBits) {
    centipede::Machine m;
    set_mo_colours(m);
    m.spriteram[0x10] = 220;
    m.spriteram[0x20] = 40;
    m.spriteram[0x30] = 0x01;
    m.gfx[3] = 0x80;
    m.spriteram[0] = 0x00;
    EXPECT_EQ(Frame(m).at(40, 23), rgba(255, 0, 0));
    m.spriteram[0] = 0x40;
    EXPECT_EQ(Frame(m).at(47, 23), rgba(255, 0, 0));
    m.spriteram[0] = 0x80;
    EXPECT_EQ(Frame(m).at(40, 20 + 12), rgba(255, 0, 0));
}

TEST(Video, ObjectColourFieldZeroLetsPlayfieldShowThroughAndErasesEarlierObject) {
    centipede::Machine m;
    set_mo_colours(m);
    m.paletteram[0x04] = 0x0B;
    m.gfx[3] = 0x80;
    m.gfx[0x800 + 3] = 0x80;
    for (int n = 0; n < 2; n++) {
        m.spriteram[size_t(0x10 + n)] = 220;
        m.spriteram[size_t(0x20 + n)] = 40;
    }
    m.spriteram[0x30] = 0x10;
    m.spriteram[0x31] = 0x00;
    EXPECT_EQ(Frame(m).at(40, 23), rgba(0, 0, 255));
    m.spriteram[0x31] = 0x20;
    EXPECT_EQ(Frame(m).at(40, 23), rgba(0, 255, 0));
    m.spriteram[0x30] = 0x00;
    m.spriteram[0x31] = 0x10;
    EXPECT_EQ(Frame(m).at(40, 23), rgba(255, 0, 0));
}

TEST(Video, LaterObjectSlotWins) {
    centipede::Machine m;
    set_mo_colours(m);
    m.gfx[3] = 0x80;
    m.gfx[0x800 + 3] = 0x80;
    for (int n = 0; n < 2; n++) {
        m.spriteram[size_t(0x10 + n)] = 220;
        m.spriteram[size_t(0x20 + n)] = 40;
    }
    m.spriteram[0x30] = 0x10;
    m.spriteram[0x31] = 0x20;
    EXPECT_EQ(Frame(m).at(40, 23), rgba(0, 255, 0));
    m.spriteram[0x30] = 0x20;
    m.spriteram[0x31] = 0x10;
    EXPECT_EQ(Frame(m).at(40, 23), rgba(255, 0, 0));
}

TEST(Video, ObjectHorizontalPositionWrapsInside256) {
    centipede::Machine m;
    set_mo_colours(m);
    m.spriteram[0x10] = 220;
    m.spriteram[0x20] = 252;
    m.spriteram[0x30] = 0x01;
    m.gfx[3] = 0x08;
    EXPECT_EQ(Frame(m).at(0, 23), rgba(255, 0, 0));
}

TEST(Video, ObjectVerticalSumWrapsInside256) {
    centipede::Machine m;
    set_mo_colours(m);
    m.spriteram[0x10] = 0x10;
    m.spriteram[0x20] = 40;
    m.spriteram[0x30] = 0x01;
    m.gfx[5] = 0x80;
    EXPECT_EQ(Frame(m).at(40, 224 + 5), rgba(255, 0, 0));
}

TEST(Video, ObjectsAreHiddenInTheLastEightColumnsUnlessFlipped) {
    centipede::Machine m;
    set_pf_colours(m);
    set_mo_colours(m);
    m.spriteram[0x10] = 220;
    m.spriteram[0x20] = 248;
    m.spriteram[0x30] = 0x01;
    m.gfx[3] = 0x80;
    EXPECT_EQ(Frame(m).at(248, 23), rgba(0, 0, 0));
    m.spriteram[0x20] = 247;
    EXPECT_EQ(Frame(m).at(247, 23), rgba(255, 0, 0));
    m.flipscreen = true;
    m.spriteram[0x20] = 248;
    EXPECT_EQ(Frame(m).at(248, 23), rgba(255, 0, 0));
}
