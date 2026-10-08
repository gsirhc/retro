#include <gtest/gtest.h>

#include "ega_render.h"
#include "machine.h"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using ibmpcat::Machine;

struct Regs {
    uint16_t ax = 0, bx = 0, cx = 0, dx = 0, bp = 0, es = 0, di = 0;
};

constexpr uint32_t kIret = 0x600;
constexpr uint32_t kCode = 0x500;

class EgaBiosTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::ifstream f(EGABIOS_PATH, std::ios::binary);
        rom.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        ASSERT_EQ(rom.size(), 16384u) << "make -C retroweb/ibmpc-at egabios";
        m.reset();
        m.chipset.mem[kIret] = 0xCF;
        for (int v = 0; v < 256; ++v) set_vector(v, 0, uint16_t(kIret));
        m.chipset.load_rom(0xC0000, rom.data(), rom.size());
        const uint8_t init[] = {0x9A, 0x03, 0x00, 0x00, 0xC0, 0xF4, 0xEB, 0xFD};
        run(init, sizeof init, Regs{});
    }

    void set_vector(int v, uint16_t seg, uint16_t off) {
        m.chipset.mem[v * 4] = uint8_t(off);
        m.chipset.mem[v * 4 + 1] = uint8_t(off >> 8);
        m.chipset.mem[v * 4 + 2] = uint8_t(seg);
        m.chipset.mem[v * 4 + 3] = uint8_t(seg >> 8);
    }
    uint16_t word(uint32_t a) const { return uint16_t(m.chipset.mem[a] | (m.chipset.mem[a + 1] << 8)); }
    uint8_t bda(uint32_t off) const { return m.chipset.mem[0x400 + off]; }
    uint16_t bda_word(uint32_t off) const { return word(0x400 + off); }

    Regs run(const uint8_t* code, size_t len, Regs in) {
        for (size_t i = 0; i < len; ++i) m.chipset.mem[kCode + i] = code[i];
        m.cpu.halted = false;
        m.cpu.cs = 0;
        m.cpu.ip = uint16_t(kCode);
        m.cpu.ss = 0;
        m.cpu.sp = 0x7000;
        m.cpu.ds = 0;
        m.cpu.es = in.es;
        m.cpu.ax = in.ax;
        m.cpu.bx = in.bx;
        m.cpu.cx = in.cx;
        m.cpu.dx = in.dx;
        m.cpu.bp = in.bp;
        m.cpu.di = in.di;
        for (int i = 0; i < 4000 && !m.cpu.halted; ++i) m.run_cycles(20000);
        EXPECT_TRUE(m.cpu.halted) << "the ROM never returned";
        Regs out;
        out.ax = uint16_t(m.cpu.ax);
        out.bx = uint16_t(m.cpu.bx);
        out.cx = uint16_t(m.cpu.cx);
        out.dx = uint16_t(m.cpu.dx);
        out.bp = uint16_t(m.cpu.bp);
        out.di = uint16_t(m.cpu.di);
        out.es = m.cpu.es;
        return out;
    }

    Regs int10(Regs in) {
        const uint8_t call[] = {0xCD, 0x10, 0xF4, 0xEB, 0xFD};
        return run(call, sizeof call, in);
    }
    Regs int10(uint16_t ax, uint16_t bx = 0, uint16_t cx = 0, uint16_t dx = 0) {
        Regs r;
        r.ax = ax;
        r.bx = bx;
        r.cx = cx;
        r.dx = dx;
        return int10(r);
    }

    std::string text_row(int row, int cols = 80) {
        std::string s;
        for (int c = 0; c < cols; ++c) s += char(m.chipset.ega.mem_read(0xB8000 + uint32_t((row * cols + c) * 2)));
        return s;
    }
    std::string text_mono_row(int row) {
        std::string s;
        for (int c = 0; c < 80; ++c) s += char(m.chipset.ega.mem_read(0xB0000 + uint32_t((row * 80 + c) * 2)));
        return s;
    }
    uint8_t text_attr(int row, int col, int cols = 80) {
        return m.chipset.ega.mem_read(0xB8000 + uint32_t((row * cols + col) * 2 + 1));
    }

    Machine m;
    std::vector<uint8_t> rom;
};

TEST_F(EgaBiosTest, RomHeaderAndChecksumAreValid) {
    EXPECT_EQ(rom[0], 0x55);
    EXPECT_EQ(rom[1], 0xAA);
    EXPECT_EQ(rom[2], 32);  // 16KB
    uint8_t sum = 0;
    for (uint8_t b : rom) sum = uint8_t(sum + b);
    EXPECT_EQ(sum, 0);
}

TEST_F(EgaBiosTest, InitHooksVectorsAndSetsMode3ForTheEnhancedDisplay) {
    EXPECT_EQ(word(0x10 * 4 + 2), 0xC000);
    EXPECT_EQ(word(0x42 * 4), kIret);
    EXPECT_EQ(word(0x43 * 4 + 2), 0xC000);
    EXPECT_EQ(word(0x1F * 4 + 2), 0xC000);
    EXPECT_EQ(bda(0x49), 3);
    EXPECT_EQ(bda_word(0x4A), 80);
    EXPECT_EQ(bda(0x84), 24);
    EXPECT_EQ(bda(0x85), 14);
    EXPECT_EQ(bda_word(0x4C), 0x1000);
    EXPECT_EQ(bda_word(0x63), 0x3D4);
    EXPECT_EQ(bda(0x87), 0x60);
    EXPECT_EQ(bda(0x88), 0x09);
    EXPECT_EQ(bda(0x10) & 0x30, 0x20);
    EXPECT_EQ(word(0x4A8 + 2), 0xC000);
    ibmpcat::RenderedFrame f;
    ibmpcat::RenderScreen(m.chipset.ega, f);
    EXPECT_EQ(f.width, 640);
    EXPECT_EQ(f.height, 350);
}

TEST_F(EgaBiosTest, BannerSaysItIsAStandIn) {
    EXPECT_EQ(text_row(0).substr(0, 37), "EGA BIOS: open stand-in for IBM's ROM");
    EXPECT_EQ(text_attr(0, 0), 0x07);
    EXPECT_EQ(bda_word(0x50), 0x0100);
}

TEST_F(EgaBiosTest, MonochromeSwitchesSetMode7AndTheMonoInfoBit) {
    m.chipset.ega.set_switches(0x0B);
    const uint8_t init[] = {0x9A, 0x03, 0x00, 0x00, 0xC0, 0xF4, 0xEB, 0xFD};
    run(init, sizeof init, Regs{});
    EXPECT_EQ(bda(0x88) & 0x0F, 0x0B);
    EXPECT_EQ(bda(0x87) & 0x02, 0x02);
    EXPECT_EQ(bda(0x10) & 0x30, 0x30);
    EXPECT_EQ(bda(0x49), 7);
    EXPECT_EQ(bda_word(0x63), 0x3B4);
    EXPECT_EQ(m.chipset.ega.cursor_start_scanline(), 0x0B);
    EXPECT_TRUE(m.chipset.ega.owns_port(0x3BA));
    EXPECT_EQ(text_mono_row(0).substr(0, 9), "EGA BIOS:");
    ibmpcat::RenderedFrame f;
    ibmpcat::RenderScreen(m.chipset.ega, f);
    EXPECT_EQ(f.width, 720);
    EXPECT_EQ(f.height, 350);
}

TEST_F(EgaBiosTest, CursorTypeIsEmulatedIntoTheFourteenLineCell) {
    int10(0x0100, 0, 0x0607);
    EXPECT_EQ(m.chipset.ega.cursor_start_scanline(), 0x0B);
    EXPECT_EQ(m.chipset.ega.cursor_end_scanline(), 0x0D);
    EXPECT_EQ(bda_word(0x60), 0x0607);
    int10(0x0100, 0, 0x0007);  // full block: the end just moves past the last line
    EXPECT_EQ(m.chipset.ega.cursor_start_scanline(), 0x00);
    EXPECT_EQ(m.chipset.ega.cursor_end_scanline(), 0x0D);
    int10(0x0100, 0, 0x2000);  // off, done by starting below the cell
    EXPECT_EQ(m.chipset.ega.cursor_start_scanline(), 0x1E);
    EXPECT_FALSE(m.chipset.ega.cursor_on_row(13));
}

TEST_F(EgaBiosTest, CursorEmulationCanBeTurnedOffInInfo) {
    m.chipset.mem[0x487] |= 0x01;
    int10(0x0100, 0, 0x0607);
    EXPECT_EQ(m.chipset.ega.cursor_start_scanline(), 0x06);
    EXPECT_EQ(m.chipset.ega.cursor_end_scanline(), 0x07);
}

TEST_F(EgaBiosTest, SetAndReadCursorPosition) {
    int10(0x0200, 0, 0, 0x050A);
    EXPECT_EQ(m.chipset.ega.cursor_offset(), 5 * 80 + 10);
    Regs r = int10(0x0300);
    EXPECT_EQ(r.dx, 0x050A);
    EXPECT_EQ(r.cx, 0x0607);
    int10(0x0200, 0x0200, 0, 0x0102);  // page 2 is not shown: the CRTC keeps page 0's
    EXPECT_EQ(bda_word(0x50 + 4), 0x0102);
    EXPECT_EQ(m.chipset.ega.cursor_offset(), 5 * 80 + 10);
}

TEST_F(EgaBiosTest, ActivePageMovesTheStartAddressInWords) {
    int10(0x0501);
    EXPECT_EQ(bda(0x62), 1);
    EXPECT_EQ(bda_word(0x4E), 0x1000);
    EXPECT_EQ(m.chipset.ega.start_offset(), 0x0800);
    Regs r = int10(0x0F00);
    EXPECT_EQ(r.ax, 0x5003);
    EXPECT_EQ(r.bx >> 8, 1);
}

TEST_F(EgaBiosTest, WriteCharAttrAndReadItBack) {
    int10(0x0200, 0, 0, 0x0304);
    int10(0x0941, 0x001E, 3);
    EXPECT_EQ(text_row(3).substr(4, 4), "AAA ");
    EXPECT_EQ(text_attr(3, 6), 0x1E);
    Regs r = int10(0x0800);
    EXPECT_EQ(r.ax, 0x1E41);
    int10(0x0A42, 0, 2);
    EXPECT_EQ(text_row(3).substr(4, 3), "BBA");
    EXPECT_EQ(text_attr(3, 4), 0x1E);
}

TEST_F(EgaBiosTest, TeletypeWrapsAndScrollsWithTheCursorAttribute) {
    int10(0x0200, 0, 0, 0x184F);
    int10(0x0941, 0x004F, 1);
    int10(0x0E58);
    EXPECT_EQ(bda_word(0x50), 0x1800);
    EXPECT_EQ(text_row(23).substr(79, 1), "X");
    EXPECT_EQ(text_attr(24, 0), 0x07);
    int10(0x0E0D);
    int10(0x0E0A);
    EXPECT_EQ(text_row(22).substr(79, 1), "X");
}

TEST_F(EgaBiosTest, ScrollWindowUpAndDown) {
    int10(0x0200, 0, 0, 0x0A0A);
    int10(0x0931, 0x0007, 1);
    int10(0x0200, 0, 0, 0x0B0A);
    int10(0x0932, 0x0007, 1);
    int10(0x0601, 0x1700, 0x0A0A, 0x0B0B);
    EXPECT_EQ(text_row(10).substr(10, 1), "2");
    EXPECT_EQ(text_row(11).substr(10, 2), "  ");
    EXPECT_EQ(text_attr(11, 10), 0x17);
    EXPECT_EQ(text_attr(11, 12), 0x07);
    int10(0x0701, 0x2000, 0x0A0A, 0x0B0B);
    EXPECT_EQ(text_row(11).substr(10, 1), "2");
    EXPECT_EQ(text_attr(10, 10), 0x20);
    int10(0x0600, 0x0700, 0, 0x184F);
    EXPECT_EQ(text_row(11).substr(10, 1), " ");
}

TEST_F(EgaBiosTest, WriteStringModes) {
    m.chipset.mem[0x800] = 'H';
    m.chipset.mem[0x801] = 0x1F;
    m.chipset.mem[0x802] = 'i';
    m.chipset.mem[0x803] = 0x2E;
    Regs r;
    r.ax = 0x1303;  // char/attr pairs, cursor moves
    r.bx = 0;
    r.cx = 2;
    r.dx = 0x0200;
    r.bp = 0x800;
    int10(r);
    EXPECT_EQ(text_row(2).substr(0, 2), "Hi");
    EXPECT_EQ(text_attr(2, 1), 0x2E);
    EXPECT_EQ(bda_word(0x50), 0x0202);
    r.ax = 0x1300;  // chars only in BL, cursor stays
    r.bx = 0x0047;
    r.cx = 4;
    r.dx = 0x0400;
    int10(r);
    EXPECT_EQ(text_row(4).substr(0, 4), "H\x1Fi.");
    EXPECT_EQ(text_attr(4, 0), 0x47);
    EXPECT_EQ(bda_word(0x50), 0x0202);
}

TEST_F(EgaBiosTest, Mode10hIsPlanar640x350) {
    int10(0x0010);
    EXPECT_EQ(bda(0x49), 0x10);
    EXPECT_EQ(bda(0x85), 14);
    EXPECT_EQ(bda_word(0x4C), 0x8000);
    EXPECT_TRUE(m.chipset.ega.graphics_mode_active());
    m.chipset.ega.mem_write(0xA0000 + 0x100, 0xA5);
    for (int p = 0; p < 4; ++p) EXPECT_EQ(m.chipset.ega.vram[(0x100 << 2) + p], 0xA5);
    ibmpcat::RenderedFrame f;
    ibmpcat::RenderScreen(m.chipset.ega, f);
    EXPECT_EQ(f.width, 640);
    EXPECT_EQ(f.height, 350);
}

TEST_F(EgaBiosTest, PlanarDotsWriteReadAndXor) {
    int10(0x0010);
    int10(0x0C0C, 0, 100, 50);
    EXPECT_EQ(int10(0x0D00, 0, 100, 50).ax & 0xFF, 0x0C);
    EXPECT_EQ(int10(0x0D00, 0, 101, 50).ax & 0xFF, 0x00);
    int10(0x0C85, 0, 100, 50);
    EXPECT_EQ(int10(0x0D00, 0, 100, 50).ax & 0xFF, 0x09);
    m.chipset.ega.mem_write(0xA0000 + 0x200, 0x5A);  // write mode 0, full bit mask again
    for (int p = 0; p < 4; ++p) EXPECT_EQ(m.chipset.ega.vram[(0x200 << 2) + p], 0x5A);
}

TEST_F(EgaBiosTest, PlanarCharactersWriteAndReadBack) {
    int10(0x0010);
    int10(0x0200, 0, 0, 0x0203);
    int10(0x0941, 0x000E, 2);
    EXPECT_EQ(int10(0x0800).ax & 0xFF, 'A');
    EXPECT_EQ(int10(0x0D00, 0, 3 * 8, 2 * 14 + 5).ax & 0xFF, 0x0E);  // left stroke of the 8x14 A
    int10(0x0200, 0, 0, 0x0205);
    EXPECT_EQ(int10(0x0800).ax & 0xFF, 0);
}

TEST_F(EgaBiosTest, PlanarTeletypeScrollsTheScreen) {
    int10(0x0010);
    int10(0x0200, 0, 0, 0x1800);
    int10(0x0E5A, 0x000F);
    int10(0x0E0A, 0x000F);
    int10(0x0200, 0, 0, 0x1700);
    EXPECT_EQ(int10(0x0800).ax & 0xFF, 'Z');
}

TEST_F(EgaBiosTest, CgaFourColorDotsAndCharacters) {
    int10(0x0004);
    EXPECT_EQ(bda(0x49), 4);
    EXPECT_EQ(bda_word(0x4A), 40);
    int10(0x0C02, 0, 5, 3);
    EXPECT_EQ(int10(0x0D00, 0, 5, 3).ax & 0xFF, 2);
    EXPECT_EQ(m.chipset.ega.mem_read(0xB8000 + 0x2000 + 80 + 1), 0x20);
    int10(0x0200, 0, 0, 0x0102);
    int10(0x0943, 0x0003, 1);
    EXPECT_EQ(int10(0x0800).ax & 0xFF, 'C');
}

TEST_F(EgaBiosTest, CgaTwoColorDotsAndCharacters) {
    int10(0x0006);
    int10(0x0C01, 0, 9, 1);
    EXPECT_EQ(m.chipset.ega.mem_read(0xB8000 + 0x2000 + 1), 0x40);
    int10(0x0200, 0, 0, 0x0001);
    int10(0x0A44, 0x0001, 1);
    EXPECT_EQ(int10(0x0800).ax & 0xFF, 'D');
}

TEST_F(EgaBiosTest, ColorPaletteInCgaModes) {
    int10(0x0004);
    int10(0x0B00, 0x0100);
    EXPECT_EQ(m.chipset.ega.attr_palette(1), 0x12);
    EXPECT_EQ(m.chipset.ega.attr_palette(2), 0x14);
    EXPECT_EQ(m.chipset.ega.attr_palette(3), 0x16);
    int10(0x0B00, 0x0009);  // background bright blue: intensity moves to bit 4
    EXPECT_EQ(m.chipset.ega.attr_palette(0), 0x11);
    EXPECT_EQ(m.chipset.ega.attr_palette(1), 0x02);  // BL bit 4 clear: the low-intensity set
}

TEST_F(EgaBiosTest, PaletteRegistersAndBlink) {
    int10(0x1000, 0x3F05);
    EXPECT_EQ(m.chipset.ega.attr_palette(5), 0x3F);
    int10(0x1003, 0x0000);
    EXPECT_FALSE(m.chipset.ega.attr_blink_enabled());
    int10(0x1003, 0x0001);
    EXPECT_TRUE(m.chipset.ega.attr_blink_enabled());
    for (int i = 0; i < 17; ++i) m.chipset.mem[0x900 + i] = uint8_t(i == 16 ? 0x01 : 0x3F - i);
    Regs r;
    r.ax = 0x1002;
    r.dx = 0x900;
    int10(r);
    EXPECT_EQ(m.chipset.ega.attr_palette(0), 0x3F);
    EXPECT_EQ(m.chipset.ega.attr_palette(15), 0x30);
}

TEST_F(EgaBiosTest, EightByEightFontGivesFortyThreeLines) {
    int10(0x1112, 0x0000);
    EXPECT_EQ(bda(0x85), 8);
    EXPECT_EQ(bda(0x84), 42);
    EXPECT_EQ(bda_word(0x4C), 43 * 160);
    EXPECT_EQ(m.chipset.ega.crtc_max_scan_line(), 7);
    EXPECT_EQ(m.chipset.ega.crtc_vertical_display_end(), 43 * 8 - 1);
    EXPECT_EQ(m.chipset.ega.cursor_start_scanline(), 6);
}

TEST_F(EgaBiosTest, UserFontLoadsIntoPlaneTwo) {
    for (int i = 0; i < 14; ++i) m.chipset.mem[0xA00 + i] = uint8_t(0x80 >> (i & 7));
    Regs r;
    r.ax = 0x1100;
    r.bx = 0x0E00;
    r.cx = 1;
    r.dx = 0x41;
    r.bp = 0xA00;
    int10(r);
    EXPECT_EQ(m.chipset.ega.vram[((0x41 * 32 + 3) << 2) + 2], 0x10);
    uint8_t glyph0 = m.chipset.ega.vram[2];
    m.chipset.ega.mem_write(0xB8000, 'Z');  // text mapping again: planes 0/1 only
    EXPECT_EQ(m.chipset.ega.vram[0], 'Z');
    EXPECT_EQ(m.chipset.ega.vram[2], glyph0);
}

TEST_F(EgaBiosTest, FontInformation) {
    Regs r = int10(0x1130, 0x0200);
    EXPECT_EQ(r.cx, 14);
    EXPECT_EQ(r.dx & 0xFF, 24);
    EXPECT_EQ(r.es, 0xC000);
    EXPECT_EQ(rom[r.bp + 'A' * 14 + 7], 0xFE);
    Regs r1 = int10(0x1130, 0x0300);
    EXPECT_EQ(r1.bp + 1024, int10(0x1130, 0x0400).bp);
}

TEST_F(EgaBiosTest, AlternateSelectReturnsEgaInformation) {
    Regs r = int10(0x1200, 0x0010);
    EXPECT_EQ(r.bx, 0x0003);
    EXPECT_EQ(r.cx, 0x0009);
}

TEST_F(EgaBiosTest, ModeSetWithBit7KeepsTheRegen) {
    int10(0x0200, 0, 0, 0x0000);
    int10(0x0951, 0x0007, 1);
    int10(0x0083);
    EXPECT_EQ(text_row(0).substr(0, 1), "Q");
    int10(0x0003);
    EXPECT_EQ(text_row(0).substr(0, 1), " ");
}

TEST_F(EgaBiosTest, ReservedModesAreIgnored) {
    int10(0x0009);
    EXPECT_EQ(bda(0x49), 3);
}

}  // namespace
