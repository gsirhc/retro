#include <gtest/gtest.h>

#include "machine.h"
#include "mb88.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <iterator>

using namespace galaga;

TEST(Map, DipBitsAndSharedRam) {
    Machine m;
    // Factory: DSWB bit0 = 1, DSWA bit0 = 1 → $6800 reads 0b11.
    EXPECT_EQ(m.mem_read(0x6800), 0x03);
    // Unused SWB bit 6 reads 1. The sub CPU resets if this bit is clear.
    EXPECT_EQ(m.mem_read(0x6806), 0x02);
    m.inputs.dsw_b = 0x00;
    m.inputs.dsw_a = 0x80;
    EXPECT_EQ(m.mem_read(0x6807), 0x02);
    m.mem_write(0x8800, 0x11);
    m.mem_write(0x8C00, 0x22);
    EXPECT_EQ(m.mem_read(0x8800), 0x22);
    EXPECT_EQ(m.ram1[0], 0x22);
    m.mem_write(0x83ED, 0x24);
    EXPECT_EQ(m.video.videoram[0x3ED], 0x24);
}

TEST(Map, SpriteRamBanksTakeBusWrites) {
    Machine m;
    m.mem_write(0x9000, 0x11);
    m.mem_write(0x9BFF, 0x22);
    EXPECT_EQ(m.ram2[0], 0x11);
    EXPECT_EQ(m.mem_read(0x9000), 0x11);
    EXPECT_EQ(m.ram3[0x3FF], 0x22);
    EXPECT_EQ(m.mem_read(0x9BFF), 0x22);
}

TEST(Map, Irq2MaskAndSubResetLine) {
    Machine m;
    m.mem_write(0x6823, 0x01);
    m.mem_write(0x6821, 0x01);
    EXPECT_TRUE(m.irq2_enable);
    m.sub.set_int(true);
    m.mem_write(0x6821, 0x00);
    EXPECT_FALSE(m.irq2_enable);
    EXPECT_FALSE(m.sub.int_line) << "clearing the mask drops the held IRQ";
    m.sub.pc = 0x1234;
    m.mem_write(0x6823, 0x00);
    EXPECT_TRUE(m.sub_reset);
    EXPECT_TRUE(m.sound_reset);
    EXPECT_EQ(m.sub.pc, 0);
}

TEST(Map, WatchdogAndIrqLatch) {
    Machine m;
    EXPECT_TRUE(m.sub_reset);
    m.mem_write(0x6823, 0x01);
    EXPECT_FALSE(m.sub_reset);
    EXPECT_FALSE(m.sound_reset);
    m.mem_write(0x6820, 0x01);
    EXPECT_TRUE(m.irq1_enable);
    m.mem_write(0x6822, 0x01);
    EXPECT_TRUE(m.nmi_disable);
    m.watchdog_ = 1;
    m.mem_write(0x6830, 0);
    EXPECT_EQ(m.watchdog_, kWatchdogFrames);
}

TEST(Io, SwitchModeReturnsActiveLowInputs) {
    Machine m;
    m.inputs.in0 = 0x02;
    m.inputs.in1 = 0x04;
    m.mem_write(0x7100, 0xA1);
    m.mem_write(0x7000, 0x05);
    m.mem_write(0x7100, 0x71);
    EXPECT_EQ(m.mem_read(0x7000), uint8_t((~0x02) & 0xFF));
    uint8_t buttons = uint8_t(~0x04);
    EXPECT_EQ(m.mem_read(0x7000), buttons);
    EXPECT_EQ(m.mem_read(0x7000), 0xFF);
    EXPECT_TRUE(m.mcu51_hle);
}

TEST(Io, MissingMcuImageStaysHleWrongSizeToo) {
    Machine m;
    RomSet set;
    set.has51 = true;
    set.mcu51[0] = 0x00;
    // has51 with a full 1024-byte image runs the real core.
    m.load_roms(set);
    EXPECT_FALSE(m.mcu51_hle);
    set.has51 = false;
    m.load_roms(set);
    EXPECT_TRUE(m.mcu51_hle);
}

TEST(Io, ReadModeClockIrqs51xx) {
    Machine m;
    RomSet set;
    set.has51 = true;
    // JP $10, then an IRQ handler at $02 that presents 5 on O.
    set.mcu51[0x00] = 0xD0;
    set.mcu51[0x02] = 0x95;
    set.mcu51[0x03] = 0x01;
    set.mcu51[0x04] = 0x3C;
    set.mcu51[0x10] = 0x3E;
    set.mcu51[0x11] = 0x04;
    set.mcu51[0x12] = 0xD2;
    m.load_roms(set);
    m.rom_main[0] = 0x18;
    m.rom_main[1] = 0xFE;
    m.mem_write(0x6823, 0x01);
    m.run_cycles(500);
    m.mem_write(0x7100, 0x71);
    EXPECT_EQ(m.mem_read(0x7000), 0x00);
    // First falling edge is a read stretch: /IO only, no host NMI.
    m.run_cycles((64 << 3) + 80);
    EXPECT_EQ(m.mem_read(0x7000), 0x05);
    EXPECT_FALSE(m.watchdog_reset);
}

TEST(Io, ReadWriteLineLatchesOnTheClockEdge) {
    Machine m;
    m.rom_main[0] = 0x18;
    m.rom_main[1] = 0xFE;
    m.reset();
    m.mem_write(0x7100, 0xA1);
    m.run_cycles(96);
    EXPECT_FALSE(m.io06_rw());
    m.mem_write(0x7100, 0x10);
    m.run_cycles(4096);
    EXPECT_FALSE(m.io06_rw()) << "a stopped clock leaves R/W as it was";
    m.mem_write(0x7100, 0xB1);
    EXPECT_FALSE(m.io06_rw());
    m.run_cycles(76);
    EXPECT_TRUE(m.io06_rw()) << "latched on the next 06XX tick";
}

TEST(Io, Mcu51SeesTheLatchedReadWriteLineOnK3) {
    Machine m;
    RomSet set;
    set.has51 = true;
    // INK; OUTO; JMP $00 echoes K onto O.
    set.mcu51[0] = 0x12;
    set.mcu51[1] = 0x01;
    set.mcu51[2] = 0xC0;
    m.load_roms(set);
    m.rom_main[0] = 0x18;
    m.rom_main[1] = 0xFE;
    m.reset();
    m.mem_write(0x6823, 0x01);
    m.mem_write(0x7100, 0xA1);
    m.run_cycles(200);
    m.mem_write(0x7100, 0x11);
    m.run_cycles(200);
    EXPECT_EQ(m.mem_read(0x7000) & 0x08, 0) << "still the write it latched";
    m.mem_write(0x7100, 0xB1);
    m.run_cycles(200);
    m.mem_write(0x7100, 0x11);
    m.run_cycles(200);
    EXPECT_EQ(m.mem_read(0x7000) & 0x08, 0x08);
}

TEST(Io, ClockRestartsOnTheNext06xxTick) {
    Machine m;
    // LD SP,$8C00; JR $. NMI handler counts and returns.
    const uint8_t boot[] = {0x31, 0x00, 0x8C, 0x18, 0xFE};
    std::copy(std::begin(boot), std::end(boot), m.rom_main.begin());
    const uint8_t nmi[] = {0x21, 0x00, 0x88, 0x34, 0xED, 0x45};
    std::copy(std::begin(nmi), std::end(nmi), m.rom_main.begin() + 0x66);
    m.reset();
    m.mem_write(0x6823, 0x01);
    m.mem_write(0x7100, 0xA1);
    m.run_cycles(64 + 40);
    EXPECT_EQ(m.ram1[0], 1);
    m.run_cycles((64 << 5) - 64);
    EXPECT_EQ(m.ram1[0], 1);
    m.run_cycles(64);
    EXPECT_EQ(m.ram1[0], 2);
}

TEST(Io, McuSpendsTwoSlotsOnTwoCycleInstructions) {
    Machine m;
    RomSet set;
    set.has51 = true;
    // A chain of JPLs, two instruction cycles each.
    for (int a = 0; a < 0x3E; a += 2) {
        set.mcu51[unsigned(a)] = 0x68;
        set.mcu51[unsigned(a + 1)] = uint8_t(a + 2);
    }
    m.load_roms(set);
    m.rom_main[0] = 0x18;
    m.rom_main[1] = 0xFE;
    m.reset();
    m.mem_write(0x6823, 0x01);
    m.run_cycles(12 * 20);
    EXPECT_EQ(m.mcu51_pc(), 22);
}

TEST(Io, WriteModeDividerNmisMain) {
    Machine m;
    // LD SP,$8C00; JR $. NMI handler counts and idles the 06XX like galagamw's $0066.
    const uint8_t boot[] = {0x31, 0x00, 0x8C, 0x18, 0xFE};
    std::copy(std::begin(boot), std::end(boot), m.rom_main.begin());
    m.rom_main[0x66] = 0x21;  // LD HL,$8800
    m.rom_main[0x67] = 0x00;
    m.rom_main[0x68] = 0x88;
    m.rom_main[0x69] = 0x34;  // INC (HL)
    m.rom_main[0x6A] = 0x3E;  // LD A,$10
    m.rom_main[0x6B] = 0x10;
    m.rom_main[0x6C] = 0x32;  // LD ($7100),A
    m.rom_main[0x6D] = 0x00;
    m.rom_main[0x6E] = 0x71;
    m.rom_main[0x6F] = 0xED;  // RETN
    m.rom_main[0x70] = 0x45;
    m.mem_write(0x6823, 0x01);
    m.mem_write(0x7100, 0xA1);
    // First edge is the next 06XX tick; a second NMI would follow 64<<5 T-states later.
    m.run_cycles((64 << 5) + 64);
    EXPECT_EQ(m.ram1[0], 1);
    EXPECT_EQ(m.mem_read(0x7100), 0x10);
    EXPECT_FALSE(m.watchdog_reset);
}

TEST(Io, ExplosionCommandArmsNoise) {
    Machine m;
    m.audio_hz = 48000;
    m.mem_write(0x7100, 0xA8);
    m.mem_write(0x7000, 0x18);
    m.run_cycles(4000);
    bool any = false;
    for (float s : m.audio) if (s != 0) any = true;
    EXPECT_TRUE(any);
    EXPECT_TRUE(m.mcu54_hle);
}

TEST(Io, HleExplosionPlaysThroughTheDacNetworkAndDecays) {
    Machine m;
    const uint8_t kick[] = {0x32, 0x30, 0x68, 0x18, 0xFB};
    std::copy(std::begin(kick), std::end(kick), m.rom_main.begin());
    m.reset();
    m.audio_hz = 48000;
    m.run_cycles(kCpuHz / 2);
    m.audio.clear();
    m.mem_write(0x7100, 0xA8);
    m.mem_write(0x7000, 0x18);
    m.run_cycles(kCpuHz / 10);
    float peak = 0;
    for (float s : m.audio) peak = std::max(peak, std::fabs(s));
    EXPECT_NEAR(peak, 0.18f, 0.05f) << "MAME's absolute scale";
    m.run_cycles(kCpuHz / 2);
    EXPECT_EQ(m.dac54(0), 0);
    m.audio.clear();
    m.run_cycles(kCpuHz / 10);
    float tail = 0;
    for (float s : m.audio) tail = std::max(tail, std::fabs(s));
    EXPECT_LT(tail, 1e-3f);
}

TEST(Io, HostWriteLandsOnTheReal51xxPortO) {
    Machine m;
    RomSet set;
    set.has51 = true;
    m.load_roms(set);
    m.mem_write(0x7100, 0x01);
    m.mem_write(0x7000, 0x5A);
    m.mem_write(0x7100, 0x11);
    EXPECT_EQ(m.mem_read(0x7000), 0x5A);
}

TEST(Io, Reading54xxGivesTheOpenBus) {
    Machine m;
    RomSet set;
    set.has54 = true;
    m.load_roms(set);
    m.mem_write(0x7100, 0x08);
    m.mem_write(0x7000, 0x5A);
    m.mem_write(0x7100, 0x18);
    EXPECT_EQ(m.mem_read(0x7000), 0xFF);
}

TEST(Io, Real54xxReadsTheCommandOnKAndR0AndDrivesThreeDacs) {
    Machine m;
    RomSet set;
    set.has54 = true;
    // INK; OUTO; LYI 0; INR; SETC; OUTO; LYI 1; OUTR; RSTC; JMP $00
    const uint8_t prog[] = {0x12, 0x01, 0x80, 0x13, 0x21, 0x01, 0x81, 0x03, 0x23, 0xC0};
    std::copy(std::begin(prog), std::end(prog), set.mcu54.begin());
    m.load_roms(set);
    m.rom_main[0] = 0x18;
    m.rom_main[1] = 0xFE;
    m.mem_write(0x7100, 0x08);
    m.mem_write(0x7000, 0x5A);
    m.mem_write(0x6823, 0x01);
    m.run_cycles(12 * 40);
    EXPECT_EQ(m.dac54(0), 0x5);
    EXPECT_EQ(m.dac54(1), 0xA);
    EXPECT_EQ(m.dac54(2), 0xA);
}

namespace {

bool hle54_sounds(Machine& m, std::initializer_list<uint8_t> bytes) {
    m.mem_write(0x7100, 0x08);
    for (uint8_t b : bytes) m.mem_write(0x7000, b);
    for (int i = 0; i < 400; i++) {
        m.run_cycles(32);
        if (m.dac54(0) != 0) return true;
    }
    return false;
}

Machine idle_main() {
    Machine m;
    m.rom_main[0] = 0x18;
    m.rom_main[1] = 0xFE;
    m.reset();
    return m;
}

}  // namespace

TEST(Mcu54Hle, ParameterCommandsSwallowTheirArguments) {
    {
        Machine m = idle_main();
        EXPECT_FALSE(hle54_sounds(m, {0x30, 0x1F, 0x1F, 0x1F, 0x1F})) << "3x takes four";
        EXPECT_TRUE(hle54_sounds(m, {0x1F}));
    }
    {
        Machine m = idle_main();
        EXPECT_FALSE(hle54_sounds(m, {0x40, 0x1F, 0x1F, 0x1F, 0x1F}));
    }
    {
        Machine m = idle_main();
        EXPECT_FALSE(hle54_sounds(m, {0x60, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F})) << "6x takes five";
        EXPECT_TRUE(hle54_sounds(m, {0x5F}));
    }
}

TEST(Mcu54Hle, VolumeCommandScalesTheBurst) {
    Machine m = idle_main();
    EXPECT_FALSE(hle54_sounds(m, {0x70, 0x2F}));
    Machine n = idle_main();
    EXPECT_TRUE(hle54_sounds(n, {0x7F, 0x2F}));
}

TEST(Video, TileAndSpritePaint) {
    Machine m;
    m.video.char_lut.fill(0x0F);
    m.video.sprite_lut.fill(0x0F);
    m.video.palette[1] = 0x3F;
    m.video.palette[0x11] = 0x3F;
    m.video.char_lut[7] = 1;
    m.video.sprite_lut[7] = 1;
    for (int i = 0; i < 16; i++) m.video.tile_rom[16 + i] = 0xFF;
    // Playfield origin is memory row 2; row 1 column 10 is the right score strip.
    m.video.videoram[2 * 32] = 1;
    m.video.videoram[0x400 + 2 * 32] = 1;
    m.video.videoram[1 * 32 + 10] = 1;
    m.video.videoram[0x400 + 1 * 32 + 10] = 1;
    for (int i = 0; i < 64; i++) m.video.sprite_rom[i] = 0xFF;
    m.ram1[0x380] = 0;
    m.ram1[0x381] = 1;
    m.ram2[0x380] = 40;
    m.ram2[0x381] = 40;
    std::array<uint32_t, kUprightW * kUprightH> rgb{};
    m.video.advance(2 * kCpuPerFrame);
    m.render(rgb.data());
    int nonzero = 0;
    for (uint32_t p : rgb) if (p) nonzero++;
    EXPECT_GT(nonzero, 50);
    EXPECT_NE(rgb[16 * kUprightW + (kVisH - 1)], 0u);
    EXPECT_NE(rgb[280 * kUprightW + (kVisH - 1 - 8 * 8)], 0u);
}

TEST(Video, SpriteColumnOrder) {
    Machine m;
    m.video.char_lut.fill(0x0F);
    m.video.sprite_lut.fill(0x0F);
    m.video.palette[1] = 0x3F;
    m.video.sprite_lut[7] = 1;
    // Byte 0 is sphcnt 3:2 = 0. Bits 7 and 3 are that group's first pixel.
    m.video.sprite_rom[0] = 0x88;
    m.ram1[0x381] = 1;
    m.ram2[0x380] = 40;
    m.ram2[0x381] = 40;
    std::array<uint32_t, kUprightW * kUprightH> rgb{};
    m.video.advance(2 * kCpuPerFrame);
    m.render(rgb.data());
    // sx = 0, sy = 185. Upright y follows native x, so the pixel is on row 0.
    EXPECT_EQ(rgb[0 * kUprightW + (kVisH - 1 - 185)], 0x00FFFF00u);
    EXPECT_NE(rgb[12 * kUprightW + (kVisH - 1 - 185)], 0x00FFFF00u);
}

TEST(Video, StarfieldEnableAndScoreStripStayClear) {
    Machine m;
    m.video.char_lut.fill(0x0F);
    m.video.sprite_lut.fill(0x0F);
    std::array<uint32_t, kUprightW * kUprightH> rgb{};
    m.video.advance(2 * kCpuPerFrame);
    m.render(rgb.data());
    int stars_off = 0;
    for (uint32_t p : rgb) if (p) stars_off++;
    EXPECT_EQ(stars_off, 0);

    m.video.star_latch[5] = 1;
    m.video.star_latch[3] = 0;
    m.video.star_latch[4] = 0;
    m.video.advance(2 * kCpuPerFrame);
    m.render(rgb.data());
    int stars_on = 0;
    for (uint32_t p : rgb) if (p) stars_on++;
    EXPECT_GT(stars_on, 20);
    // Score strips are outside the 256-wide star window, so those rows stay black.
    int edge = 0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < kUprightW; x++)
            if (rgb[y * kUprightW + x]) edge++;
    for (int y = kUprightH - 16; y < kUprightH; y++)
        for (int x = 0; x < kUprightW; x++)
            if (rgb[y * kUprightW + x]) edge++;
    EXPECT_EQ(edge, 0);
}

TEST(Video, TilesCoverSpritesInScoreStrip) {
    Machine m;
    m.video.char_lut.fill(0x0F);
    m.video.sprite_lut.fill(0x0F);
    m.video.palette[1] = 0x3F;
    m.video.palette[0x11] = 0x3F;
    m.video.char_lut[7] = 1;
    m.video.sprite_lut[7] = 1;
    for (int i = 0; i < 16; i++) m.video.tile_rom[16 + i] = 0xFF;
    for (int i = 0; i < 64; i++) m.video.sprite_rom[i] = 0xFF;
    // Left score strip cell (screen col 0, row 0) is memory row 30 col 2.
    m.video.videoram[30 * 32 + 2] = 1;
    m.video.videoram[0x400 + 30 * 32 + 2] = 1;
    m.ram1[0x380] = 0;
    m.ram1[0x381] = 1;
    m.ram2[0x380] = 225;  // sy = 0 after the 04XX countdown
    m.ram2[0x381] = 40;   // sx = 0
    std::array<uint32_t, kUprightW * kUprightH> rgb{};
    m.video.advance(2 * kCpuPerFrame);
    m.render(rgb.data());
    // Native (0,0) → upright (223, 0). Tile paints after the sprite.
    EXPECT_NE(rgb[0 * kUprightW + (kVisH - 1)], 0u);
}

TEST(Video, FlipUsesSecondCharSetAndInvertedCounters) {
    Machine m;
    m.video.char_lut.fill(0x0F);
    m.video.char_lut[3] = 1;
    m.video.palette[0x11] = 0x3F;
    m.video.tile_rom[16 + 8] = 0x88;            // char 1, pixel (0,0)
    m.video.tile_rom[0x81 * 16 + 8] = 0x11;     // char 0x81, same pixel nibble-reversed to (3,0)
    m.video.videoram[2 + (2 << 5)] = 1;         // screen tile 4,0
    m.video.flip = true;
    m.video.advance(2 * kCpuPerFrame);
    std::array<uint32_t, kUprightW * kUprightH> rgb{};
    m.render(rgb.data());
    // Fully mirrored: native (32,0) lands at (255,223), upright (0,255).
    EXPECT_NE(rgb[255 * kUprightW + 0], 0u);
    int lit = 0;
    for (uint32_t p : rgb) if (p) lit++;
    EXPECT_EQ(lit, 1);
}

TEST(Video, SpritePenIsClearOnlyWhenItsLutEntryIs0F) {
    Machine m;
    m.video.char_lut.fill(0x0F);
    m.video.sprite_lut.fill(0x0F);
    m.video.palette[2] = 0x07;
    m.video.sprite_lut[4] = 2;                  // colour 1, pen 0 opaque
    m.ram1[0x381] = 1;
    m.ram2[0x380] = 40;
    m.ram2[0x381] = 40;
    m.video.advance(2 * kCpuPerFrame);
    std::array<uint32_t, kUprightW * kUprightH> rgb{};
    m.render(rgb.data());
    EXPECT_EQ(rgb[5 * kUprightW + (kVisH - 1 - 190)], m.video.prom_rgb(2));
}

TEST(Video, StarfieldAdvancesWithTheBeamNotRender) {
    Machine m;
    m.video.star_latch[5] = 1;
    m.video.advance(kCpuPerFrame);
    uint16_t a = m.video.star_lfsr();
    std::array<uint32_t, kUprightW * kUprightH> rgb{};
    for (int i = 0; i < 5; i++) m.render(rgb.data());
    EXPECT_EQ(m.video.star_lfsr(), a);
    m.video.advance(kCpuPerFrame);
    EXPECT_NE(m.video.star_lfsr(), a);
}

TEST(Machine, VblankIrqHoldsUntilMaskCleared) {
    Machine m;
    m.mem_write(0x6820, 1);
    m.main.iff1 = false;
    m.video.advance(kVBlankLine * kCpuPerLine);
    m.run_cycles(8);
    EXPECT_TRUE(m.main.int_line);
    m.mem_write(0x6820, 0);
    EXPECT_FALSE(m.main.int_line);
}

TEST(Machine, WatchdogResetKeepsRam) {
    Machine m;
    m.rom_main[0] = 0x18;
    m.rom_main[1] = 0xFE;
    m.reset();
    m.ram1[0x10] = 0xA5;
    m.video.videoram[5] = 0x5A;
    m.run_cycles(kCpuPerFrame * (kWatchdogFrames + 1));
    ASSERT_TRUE(m.watchdog_reset);
    EXPECT_EQ(m.ram1[0x10], 0xA5);
    EXPECT_EQ(m.video.videoram[5], 0x5A);
    EXPECT_TRUE(m.sub_reset);
}

namespace {

void send51(Machine& m, std::initializer_list<uint8_t> bytes) {
    m.mem_write(0x7100, 0x01);
    for (uint8_t b : bytes) m.mem_write(0x7000, b);
}

void tap(Machine& m, uint8_t bit) {
    m.inputs.in1 |= bit;
    m.run_cycles(64);
    m.inputs.in1 = uint8_t(m.inputs.in1 & ~bit);
    m.run_cycles(64);
}

}  // namespace

TEST(Mcu51Hle, CoinageTwoCoinsOneCredit) {
    Machine m;
    m.rom_main[0] = 0x18;
    m.rom_main[1] = 0xFE;
    m.reset();
    send51(m, {0x01, 2, 1, 1, 1, 0x02});
    tap(m, 0x10);
    EXPECT_EQ(m.credits, 0);
    tap(m, 0x10);
    EXPECT_EQ(m.credits, 1);
    tap(m, 0x20);
    EXPECT_EQ(m.credits, 2);
}

TEST(Mcu51Hle, StartSpendsCreditsOnceUntilCreditModeReenters) {
    Machine m;
    m.rom_main[0] = 0x18;
    m.rom_main[1] = 0xFE;
    m.reset();
    send51(m, {0x01, 1, 1, 1, 1, 0x02});
    for (int i = 0; i < 3; i++) tap(m, 0x10);
    tap(m, 0x08);
    EXPECT_EQ(m.credits, 1) << "2P start takes two";
    tap(m, 0x04);
    EXPECT_EQ(m.credits, 1) << "a game is running";
    send51(m, {0x02});
    tap(m, 0x04);
    EXPECT_EQ(m.credits, 0);
}

TEST(Mcu51Hle, CoinMetersClickOncePerCoin) {
    Machine m;
    m.rom_main[0] = 0x18;
    m.rom_main[1] = 0xFE;
    m.reset();
    send51(m, {0x01, 2, 1, 1, 1, 0x02});
    tap(m, 0x10);
    tap(m, 0x10);
    tap(m, 0x20);
    EXPECT_EQ(m.coin_counter[0], 2);
    EXPECT_EQ(m.coin_counter[1], 1);
    m.reset();
    EXPECT_EQ(m.coin_counter[0], 2) << "the meter is mechanical";
}

TEST(Io, Real51xxDrivesCoinMetersActiveLowOnP) {
    Machine m;
    RomSet set;
    set.has51 = true;
    // JP $10; then OUTP F, 7, F, 3 and spin.
    const uint8_t prog[] = {0x9F, 0x02, 0x97, 0x02, 0x9F, 0x02, 0x93, 0x02, 0xD8};
    set.mcu51[0x00] = 0xD0;
    std::copy(std::begin(prog), std::end(prog), set.mcu51.begin() + 0x10);
    m.load_roms(set);
    m.rom_main[0] = 0x18;
    m.rom_main[1] = 0xFE;
    m.mem_write(0x6823, 0x01);
    m.run_cycles(500);
    EXPECT_EQ(m.coin_counter[0], 2);
    EXPECT_EQ(m.coin_counter[1], 1);
}

TEST(Mcu51Hle, CreditModeReportsBcdCreditsThenControls) {
    Machine m;
    m.rom_main[0] = 0x18;
    m.rom_main[1] = 0xFE;
    m.reset();
    send51(m, {0x01, 1, 1, 1, 1, 0x02});
    for (int i = 0; i < 12; i++) tap(m, 0x10);
    m.inputs.in0 = 0x0F;
    m.mem_write(0x7100, 0x11);
    EXPECT_EQ(m.mem_read(0x7000), 0x12);
    EXPECT_EQ(m.mem_read(0x7000), 0xF0) << "active-low IN0";
    EXPECT_EQ(m.mem_read(0x7000), 0xFF);
    EXPECT_EQ(m.mem_read(0x7000), 0x12);
    send51(m, {0x01, 0, 0, 0, 0, 0x02});
    tap(m, 0x10);
    m.mem_write(0x7100, 0x11);
    EXPECT_EQ(m.mem_read(0x7000), 0x99) << "free play shows 99";
}

TEST(Mcu51Hle, ZeroCoinsPerCreditIsFreePlay) {
    Machine m;
    m.rom_main[0] = 0x18;
    m.rom_main[1] = 0xFE;
    m.reset();
    send51(m, {0x01, 0, 0, 0, 0, 0x02});
    tap(m, 0x10);
    EXPECT_EQ(m.credits, 100);
}

TEST(Video, SoundNmiEdgesAtLines64And192) {
    Video v;
    v.reset();
    v.advance(64 * kCpuPerLine - 1);
    EXPECT_FALSE(v.sound_nmi_edge);
    v.advance(1);
    EXPECT_TRUE(v.sound_nmi_edge);
    v.sound_nmi_edge = false;
    v.advance(128 * kCpuPerLine - 1);
    EXPECT_FALSE(v.sound_nmi_edge);
    v.advance(1);
    EXPECT_TRUE(v.sound_nmi_edge);
}

TEST(Machine, Io06NmiPeriodIs64ShiftedByControlBits) {
    Machine m;
    // LD SP,$8C00; JR $. NMI: INC ($8800); RETN.
    const uint8_t main[] = {0x31, 0x00, 0x8C, 0x18, 0xFE};
    std::copy(std::begin(main), std::end(main), m.rom_main.begin());
    const uint8_t nmi[] = {0xF5, 0x3A, 0x00, 0x88, 0x3C, 0x32, 0x00, 0x88, 0xF1, 0xED, 0x45};
    std::copy(std::begin(nmi), std::end(nmi), m.rom_main.begin() + 0x66);
    m.reset();
    m.mem_write(0x6823, 1);
    m.mem_write(0x7100, 0x20 | 0x08);  // shift 1: 128 T-states, 54XX selected, write mode
    m.run_cycles(128 * 100);
    EXPECT_NEAR(m.ram1[0], 100, 2);
    m.mem_write(0x7100, 0x00);
    m.run_cycles(200);
    uint8_t held = m.ram1[0];
    m.run_cycles(128 * 100);
    EXPECT_EQ(m.ram1[0], held) << "control 0 stops the NMI clock";
}

TEST(Machine, SubAndSoundCpusRunInLockStepWithMain) {
    Machine m;
    m.reset();
    m.mem_write(0x6823, 1);
    uint64_t main0 = m.main.cycles, sub0 = m.sub.cycles, snd0 = m.sound.cycles;
    m.run_cycles(kCpuHz / 10);
    int64_t dm = int64_t(m.main.cycles - main0);
    EXPECT_NEAR(double(m.sub.cycles - sub0), double(dm), 32.0);
    EXPECT_NEAR(double(m.sound.cycles - snd0), double(dm), 32.0);
}

TEST(Mb88, LoadImmediateAddAndCall) {
    Mb88 cpu;
    cpu.reset();
    // LI 5; AI 3; CALL page0 $20; JMP $00  — at $20: LI 9; RTS
    cpu.rom[0] = 0x95;
    cpu.rom[1] = 0x73;
    cpu.rom[2] = 0x60;
    cpu.rom[3] = 0x20;
    cpu.rom[4] = 0xC0;  // jmp 0, st will be 1
    cpu.rom[0x20] = 0x99;
    cpu.rom[0x21] = 0x2C;
    cpu.step();
    EXPECT_EQ(cpu.a, 5);
    cpu.step();
    EXPECT_EQ(cpu.a, 8);
    EXPECT_EQ(cpu.cf, 0);
    cpu.step();
    EXPECT_EQ(cpu.pc_full(), 0x20);
    cpu.step();
    EXPECT_EQ(cpu.a, 9);
    cpu.step();
    EXPECT_EQ(cpu.pc_full(), 4);
}

TEST(Wsg, GalagaRegisterWriteReachesSharedChip) {
    Machine m;
    m.mem_write(0x6815, 0x0F);
    m.mem_write(0x6811, 0x01);
    EXPECT_EQ(m.wsg.regs[0x15], 0x0F);
    EXPECT_EQ(m.wsg.regs[0x11], 0x01);
    EXPECT_TRUE(m.wsg.enabled);
}

TEST(Mb88, InternalSerialClockSetsSfAfterFourShifts) {
    Mb88 cpu;
    cpu.reset();
    cpu.halted_reset = false;
    cpu.rom[0] = 0x3E;  // EN $20: serial on, internal clock
    cpu.rom[1] = 0x20;
    cpu.rom[6] = 0x27;  // TSTS
    cpu.sb = 0x0F;
    cpu.step();
    EXPECT_EQ(cpu.sf, 0);
    for (int i = 0; i < 4 && cpu.pc < 6; i++) cpu.step();
    EXPECT_EQ(cpu.sf, 1);
    EXPECT_EQ(cpu.sb, 0) << "SI reads 0 when unconnected";
    cpu.step();
    EXPECT_EQ(cpu.st, 0);
    EXPECT_EQ(cpu.sf, 0);
}

TEST(Mb88, SerialInterruptVectorsTo06) {
    Mb88 cpu;
    cpu.reset();
    cpu.halted_reset = false;
    cpu.rom[0] = 0x3E;  // EN $21: serial on with its interrupt
    cpu.rom[1] = 0x21;
    for (int i = 0; i < 8; i++) cpu.step();
    EXPECT_TRUE(cpu.in_irq);
    EXPECT_GE(cpu.pc_full(), 0x06);
    EXPECT_LT(cpu.pc_full(), 0x10);
}
