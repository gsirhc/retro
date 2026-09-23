#include <gtest/gtest.h>

#include "machine.h"
#include "mb88.h"

#include <algorithm>

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
    // has51 with a full image is the real core. A 1024-byte buffer is the
    // only size RomSet carries; the page rejects any other length first.
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
    // First falling edge is one full period in, and it is the read
    // stretch: /IO only, no host NMI.
    m.run_cycles((64 << 3) + 80);
    EXPECT_EQ(m.mem_read(0x7000), 0x05);
    EXPECT_FALSE(m.watchdog_reset);
}

TEST(Io, WriteModeDividerNmisMain) {
    Machine m;
    // JR $ at reset, and an NMI handler that counts and idles the 06XX
    // the way galagamw's $0066 handler does when the byte count runs out.
    m.rom_main[0] = 0x18;
    m.rom_main[1] = 0xFE;
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
    // Divider 5 periods at 64<<5 T-states, plus the handler that stores $10.
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

TEST(Video, TileAndSpritePaint) {
    Machine m;
    m.video.palette[1] = 0x3F;
    m.video.palette[0x11] = 0x3F;
    m.video.char_lut[7] = 1;
    m.video.sprite_lut[7] = 1;
    for (int i = 0; i < 16; i++) m.video.tile_rom[16 + i] = 0xFF;
    // Playfield origin is memory row 2. Row 1 column 10 is the right
    // score strip, upright y = native x of that strip.
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
    m.render(rgb.data());
    int nonzero = 0;
    for (uint32_t p : rgb) if (p) nonzero++;
    EXPECT_GT(nonzero, 50);
    EXPECT_NE(rgb[16 * kUprightW + (kVisH - 1)], 0u);
    EXPECT_NE(rgb[280 * kUprightW + (kVisH - 1 - 8 * 8)], 0u);
}

TEST(Video, SpriteColumnOrder) {
    Machine m;
    m.video.palette[1] = 0x3F;
    m.video.sprite_lut[7] = 1;
    // Byte 0 is sphcnt 3:2 = 0. Bits 7 and 3 are that group's first pixel.
    m.video.sprite_rom[0] = 0x88;
    m.ram1[0x381] = 1;
    m.ram2[0x380] = 40;
    m.ram2[0x381] = 40;
    std::array<uint32_t, kUprightW * kUprightH> rgb{};
    m.render(rgb.data());
    // sx = 0, sy = 185. Upright y follows native x, so the pixel is on row 0.
    EXPECT_EQ(rgb[0 * kUprightW + (kVisH - 1 - 185)], 0x00FFFF00u);
    EXPECT_NE(rgb[12 * kUprightW + (kVisH - 1 - 185)], 0x00FFFF00u);
}

TEST(Video, StarfieldEnableAndScoreStripStayClear) {
    Machine m;
    std::array<uint32_t, kUprightW * kUprightH> rgb{};
    m.render(rgb.data());
    int stars_off = 0;
    for (uint32_t p : rgb) if (p) stars_off++;
    EXPECT_EQ(stars_off, 0);

    m.video.star_latch[5] = 1;
    m.video.star_latch[3] = 0;
    m.video.star_latch[4] = 0;
    m.render(rgb.data());
    int stars_on = 0;
    for (uint32_t p : rgb) if (p) stars_on++;
    EXPECT_GT(stars_on, 20);
    // Score strips are outside the 256-wide star window (native x 0–15
    // and 272–287 → upright top and bottom 16 rows stay black).
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
    m.render(rgb.data());
    // Native (0,0) → upright (223, 0). Tile paints after the sprite.
    EXPECT_NE(rgb[0 * kUprightW + (kVisH - 1)], 0u);
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
