#include <gtest/gtest.h>

#include "ega.h"
#include "ega_render.h"

#include <initializer_list>
#include <utility>

namespace {

using ibmpcat::DetectScreenMode;
using ibmpcat::Ega;
using ibmpcat::kTextRenderHeight;
using ibmpcat::kTextRenderWidth;
using ibmpcat::RenderCgaGraphics4Screen;
using ibmpcat::RenderEgaNative16Screen;
using ibmpcat::RenderedFrame;
using ibmpcat::RenderScreen;
using ibmpcat::RenderTextScreen;
using ibmpcat::ScreenMode;

void Crtc(Ega &ega, std::initializer_list<std::pair<uint8_t, uint8_t>> regs) {
    for (auto [i, v] : regs) { ega.out(0x3D4, i); ega.out(0x3D5, v); }
}
void Seq(Ega &ega, uint8_t i, uint8_t v) { ega.out(0x3C4, i); ega.out(0x3C5, v); }
void Gfx(Ega &ega, uint8_t i, uint8_t v) { ega.out(0x3CE, i); ega.out(0x3CF, v); }
void Attr(Ega &ega, uint8_t i, uint8_t v) { ega.in(0x3DA); ega.out(0x3C0, i); ega.out(0x3C0, v); }

// IBM's mode 3 with the Enhanced Color Display (IBM EGA Technical Reference, BIOS register tables).
void ProgramMode3(Ega &ega) {
    ega.out(0x3C2, 0xA7);
    Seq(ega, 0x01, 0x01); Seq(ega, 0x02, 0x03); Seq(ega, 0x03, 0x00); Seq(ega, 0x04, 0x03);
    Crtc(ega, {{0x00, 0x5B}, {0x01, 0x4F}, {0x06, 0x6C}, {0x07, 0x1F}, {0x08, 0x00}, {0x09, 0x0D},
               {0x0A, 0x0B}, {0x0B, 0x0C}, {0x0C, 0x00}, {0x0D, 0x00}, {0x10, 0x5E}, {0x11, 0x2B},
               {0x12, 0x5D}, {0x13, 0x28}, {0x14, 0x0F}, {0x17, 0xA3}, {0x18, 0xFF}});
    Gfx(ega, 0x05, 0x10); Gfx(ega, 0x06, 0x0E);
    Attr(ega, 0x10, 0x08); Attr(ega, 0x12, 0x0F); Attr(ega, 0x13, 0x00);
    for (int i = 0; i < 16; ++i) Attr(ega, uint8_t(i), uint8_t(i));
    Attr(ega, 0x07, 0x07); Attr(ega, 0x0F, 0x3F);
}

// IBM's mode 10h with 128KB or more (same tables).
void ProgramMode10(Ega &ega) {
    ega.out(0x3C2, 0xA7);
    Seq(ega, 0x01, 0x01); Seq(ega, 0x02, 0x0F); Seq(ega, 0x04, 0x06);
    Crtc(ega, {{0x00, 0x5B}, {0x01, 0x4F}, {0x06, 0x6C}, {0x07, 0x1F}, {0x08, 0x00}, {0x09, 0x00},
               {0x0C, 0x00}, {0x0D, 0x00}, {0x10, 0x5E}, {0x11, 0x2B}, {0x12, 0x5D}, {0x13, 0x28},
               {0x14, 0x0F}, {0x17, 0xE3}, {0x18, 0xFF}});
    Gfx(ega, 0x05, 0x00); Gfx(ega, 0x06, 0x05);
    Attr(ega, 0x10, 0x01); Attr(ega, 0x12, 0x0F); Attr(ega, 0x13, 0x00);
    for (int i = 0; i < 16; ++i) Attr(ega, uint8_t(i), uint8_t(i));
}

void PutCell(Ega &ega, uint32_t cell, uint8_t ch, uint8_t attr) {
    const uint32_t a = ega.display_address(cell, 0);
    ega.vram[(a << 2) + 0] = ch;
    ega.vram[(a << 2) + 1] = attr;
}
void PutGlyphRow(Ega &ega, uint32_t map_offset, uint8_t ch, int row, uint8_t bits) {
    ega.vram[((map_offset + uint32_t(ch) * 32 + uint32_t(row)) << 2) + 2] = bits;
}

struct Frame {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    uint8_t r(int x, int y) const { return rgba[(std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4 + 0]; }
    uint8_t g(int x, int y) const { return rgba[(std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4 + 1]; }
    uint8_t b(int x, int y) const { return rgba[(std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4 + 2]; }
    bool white(int x, int y) const { return r(x, y) == 0xFF && g(x, y) == 0xFF && b(x, y) == 0xFF; }
    bool black(int x, int y) const { return r(x, y) == 0 && g(x, y) == 0 && b(x, y) == 0; }
};
Frame Text(const Ega &ega) { Frame f; RenderTextScreen(ega, f.rgba, f.w, f.h); return f; }
Frame Native(const Ega &ega) { Frame f; RenderEgaNative16Screen(ega, f.rgba, f.w, f.h); return f; }

// Runs the raster until `n` more vertical frames have passed.
void AdvanceFrames(Ega &ega, uint64_t &now, uint32_t n) {
    uint32_t target = ega.frame_count() + n;
    while (ega.frame_count() < target) ega.tick(now += 1000);
}

TEST(EgaRenderTest, Mode3IsEightyByTwentyFiveOnA350LineRaster) {
    Ega ega;
    ega.reset();
    ProgramMode3(ega);
    Frame f = Text(ega);
    EXPECT_EQ(f.w, 640);
    EXPECT_EQ(f.h, 350);
}

TEST(EgaRenderTest, RenderScreenIsBlackWhileTheCrtcIsHeldInReset) {
    Ega ega;
    ega.reset();
    ProgramMode3(ega);
    PutCell(ega, 0, 'A', 0x0F);
    PutGlyphRow(ega, 0, 'A', 0, 0xFF);
    Crtc(ega, {{0x17, 0x23}});
    RenderedFrame out;
    RenderScreen(ega, out);
    EXPECT_EQ(out.width, kTextRenderWidth);
    EXPECT_EQ(out.height, kTextRenderHeight);
    EXPECT_EQ(out.rgba[0], 0);
}

TEST(EgaRenderTest, RenderScreenFollowsTheModeRegisters) {
    Ega ega;
    ega.reset();
    ProgramMode3(ega);
    EXPECT_EQ(DetectScreenMode(ega), ScreenMode::kText);
    Gfx(ega, 0x06, 0x01); Gfx(ega, 0x05, 0x20);
    EXPECT_EQ(DetectScreenMode(ega), ScreenMode::kCgaGraphics4);
    Gfx(ega, 0x05, 0x00);
    EXPECT_EQ(DetectScreenMode(ega), ScreenMode::kEgaGraphics16);
    Gfx(ega, 0x05, 0x40);  // bit 6 is VGA's 256-color shift; the EGA has no such mode
    EXPECT_EQ(DetectScreenMode(ega), ScreenMode::kEgaGraphics16);
    Gfx(ega, 0x06, 0x00);
    EXPECT_EQ(DetectScreenMode(ega), ScreenMode::kText);

    ProgramMode10(ega);
    RenderedFrame out;
    RenderScreen(ega, out);
    EXPECT_EQ(out.width, 640);
    EXPECT_EQ(out.height, 350);
}

TEST(EgaRenderTest, GlyphUsesTheLivePaletteColors) {
    Ega ega;
    ega.reset();
    ProgramMode3(ega);
    Attr(ega, 0x01, 0x01);
    PutCell(ega, 0, 'A', 0x1F);
    PutGlyphRow(ega, 0, 'A', 0, 0x80);
    Frame f = Text(ega);
    EXPECT_TRUE(f.white(0, 0));
    EXPECT_EQ(f.b(1, 0), 0xAA);
    EXPECT_EQ(f.r(1, 0), 0);
}

TEST(EgaRenderTest, RowHeightFollowsMaximumScanLine) {
    Ega ega;
    ega.reset();
    ProgramMode3(ega);
    Crtc(ega, {{0x09, 0x0F}, {0x12, 0x8F}, {0x07, 0x1F}});  // 16-line rows, 400 lines
    PutCell(ega, 0, 'g', 0x0F);
    PutGlyphRow(ega, 0, 'g', 14, 0xFF);
    Frame f = Text(ega);
    EXPECT_EQ(f.h, 400);
    EXPECT_TRUE(f.white(0, 14));
}

TEST(EgaRenderTest, FortyColumnTextWrapsAtTheOffsetRegister) {
    Ega ega;
    ega.reset();
    ProgramMode3(ega);
    Crtc(ega, {{0x01, 0x27}, {0x13, 0x14}});
    PutCell(ega, 40, 'A', 0x0F);
    PutGlyphRow(ega, 0, 'A', 0, 0x80);
    Frame f = Text(ega);
    EXPECT_EQ(f.w, 320);
    EXPECT_TRUE(f.white(0, 14));
}

// E5: MODE CON LINES=43 is an 8x8 font in 350 lines.
TEST(EgaRenderTest, FortyThreeLineModeShowsEveryRow) {
    Ega ega;
    ega.reset();
    ProgramMode3(ega);
    Crtc(ega, {{0x09, 0x07}});
    PutCell(ega, 42 * 80, 'A', 0x0F);
    PutGlyphRow(ega, 0, 'A', 0, 0x80);
    Frame f = Text(ega);
    EXPECT_EQ(f.h, 350);
    EXPECT_TRUE(f.white(0, 42 * 8));
}

TEST(EgaRenderTest, NineDotCellsRepeatColumnEightOnlyForLineGraphics) {
    Ega ega;
    ega.reset();
    ProgramMode3(ega);
    Seq(ega, 0x01, 0x00);
    Attr(ega, 0x10, 0x0C);
    Attr(ega, 0x13, 0x08);
    PutCell(ega, 0, 0xC4, 0x0F);
    PutCell(ega, 1, 'A', 0x0F);
    PutGlyphRow(ega, 0, 0xC4, 0, 0xFF);
    PutGlyphRow(ega, 0, 'A', 0, 0xFF);
    Frame f = Text(ega);
    EXPECT_EQ(f.w, 720);
    EXPECT_TRUE(f.white(8, 0));
    EXPECT_TRUE(f.black(17, 0));
    Attr(ega, 0x10, 0x08);
    EXPECT_TRUE(Text(ega).black(8, 0));
}

TEST(EgaRenderTest, CpuTextWritesLandWhereTheCrtcFetches) {
    Ega ega;
    ega.reset();
    ProgramMode3(ega);
    Seq(ega, 0x04, 0x02);
    Gfx(ega, 0x08, 0xFF);
    PutGlyphRow(ega, 0, 'A', 0, 0x80);
    ega.mem_write(0xB8000 + 2 * 81, 'A');
    ega.mem_write(0xB8000 + 2 * 81 + 1, 0x0F);
    Frame f = Text(ega);
    EXPECT_TRUE(f.white(8, 14));
    EXPECT_TRUE(f.black(0, 0));
}

TEST(EgaRenderTest, MonochromeDisplayShowsVideoAndIntensityInGreen) {
    Ega ega;
    ega.reset();
    ProgramMode3(ega);
    ega.set_switches(0x0B);
    Attr(ega, 0x07, 0x08); Attr(ega, 0x0F, 0x18);
    PutCell(ega, 0, 0xDB, 0x07);
    PutCell(ega, 1, 0xDB, 0x0F);
    PutGlyphRow(ega, 0, 0xDB, 0, 0xFF);
    Frame f = Text(ega);
    EXPECT_EQ(f.g(0, 0), 0xAA);
    EXPECT_EQ(f.r(0, 0), 0x00);
    EXPECT_EQ(f.g(9, 0), 0xFF);
    EXPECT_EQ(f.b(9, 0), 0x00);
}

// E2: the start address picks the first cell, in text and in graphics.
TEST(EgaRenderTest, StartAddressScrollsTextAndGraphics) {
    Ega ega;
    ega.reset();
    ProgramMode3(ega);
    PutCell(ega, 160, 'A', 0x0F);
    PutGlyphRow(ega, 0, 'A', 0, 0x80);
    Crtc(ega, {{0x0C, 0x00}, {0x0D, 0xA0}});
    EXPECT_TRUE(Text(ega).white(0, 0));

    ProgramMode10(ega);
    Attr(ega, 0x0F, 0x3F);
    for (int p = 0; p < 4; ++p) ega.vram[(0x4000u << 2) + uint32_t(p)] = 0x80;
    EXPECT_TRUE(Native(ega).black(0, 0));
    Crtc(ega, {{0x0C, 0x40}, {0x0D, 0x00}});
    EXPECT_TRUE(Native(ega).white(0, 0));
}

// E3: AR13 shifts left 0-7 dots; 9-dot text runs 8, 0-7 for 0-8.
TEST(EgaRenderTest, PelPanningShiftsLeft) {
    Ega ega;
    ega.reset();
    ProgramMode10(ega);
    Attr(ega, 0x0F, 0x3F);
    for (int p = 0; p < 4; ++p) ega.vram[uint32_t(p)] = 0x08;  // dot 4
    EXPECT_TRUE(Native(ega).white(4, 0));
    Attr(ega, 0x13, 0x03);
    EXPECT_TRUE(Native(ega).white(1, 0));

    ProgramMode3(ega);
    Seq(ega, 0x01, 0x00);
    PutCell(ega, 0, 'A', 0x0F);
    PutGlyphRow(ega, 0, 'A', 0, 0x08);
    Attr(ega, 0x13, 0x08);
    EXPECT_TRUE(Text(ega).white(4, 0));
    Attr(ega, 0x13, 0x00);
    EXPECT_TRUE(Text(ega).white(3, 0));
}

TEST(EgaRenderTest, PresetRowScanStartsTheTopRowPartWayDown) {
    Ega ega;
    ega.reset();
    ProgramMode3(ega);
    PutCell(ega, 0, 'A', 0x0F);
    PutGlyphRow(ega, 0, 'A', 5, 0x80);
    Crtc(ega, {{0x08, 0x05}});
    EXPECT_TRUE(Text(ega).white(0, 0));
}

// E4: past Line Compare (9 bits, bit 8 in Overflow bit 4) the address restarts at 0.
TEST(EgaRenderTest, LineCompareSplitsTheScreen) {
    Ega ega;
    ega.reset();
    ProgramMode10(ega);
    Attr(ega, 0x0F, 0x3F);
    for (int p = 0; p < 4; ++p) ega.vram[uint32_t(p)] = 0x80;
    Crtc(ega, {{0x0C, 0x40}, {0x0D, 0x00}, {0x18, 0x2B}, {0x07, 0x1F}});  // split after line 299
    Frame f = Native(ega);
    EXPECT_TRUE(f.black(0, 0));
    EXPECT_TRUE(f.black(0, 299));
    EXPECT_TRUE(f.white(0, 300));
    Crtc(ega, {{0x07, 0x0F}});  // bit 8 clear: compare 43
    EXPECT_TRUE(Native(ega).white(0, 44));
}

// E6: AR10 bit 3 picks blinking foreground or a bright background for attribute bit 7.
TEST(EgaRenderTest, AttributeBitSevenBlinksOrBrightens) {
    Ega ega;
    ega.reset();
    ProgramMode3(ega);
    Attr(ega, 0x09, 0x39);
    PutCell(ega, 0, 'A', 0x9F);
    PutGlyphRow(ega, 0, 'A', 0, 0x80);
    uint64_t now = 0;
    EXPECT_TRUE(Text(ega).white(0, 0));
    EXPECT_EQ(Text(ega).b(1, 0), 0xAA);
    AdvanceFrames(ega, now, 16);
    EXPECT_EQ(Text(ega).b(0, 0), 0xAA);

    Attr(ega, 0x10, 0x00);
    Frame f = Text(ega);
    EXPECT_TRUE(f.white(0, 0));
    EXPECT_EQ(f.r(1, 0), 0x55);
}

// E7: map A (SR03 bits 2-3) for attribute bit 3 set, map B for clear, only with Memory Mode bit 1.
TEST(EgaRenderTest, CharacterMapSelectPicksAFontByAttributeBitThree) {
    Ega ega;
    ega.reset();
    ProgramMode3(ega);
    PutCell(ega, 0, 'A', 0x0F);
    PutCell(ega, 1, 'A', 0x07);
    PutGlyphRow(ega, 0x0000, 'A', 0, 0x80);
    PutGlyphRow(ega, 0x4000, 'A', 0, 0x40);
    Seq(ega, 0x03, 0x04);  // A = map 1, B = map 0
    Frame f = Text(ega);
    EXPECT_TRUE(f.white(1, 0));
    EXPECT_TRUE(f.black(0, 0));
    EXPECT_FALSE(f.black(8, 0));
    Seq(ega, 0x04, 0x01);
    EXPECT_TRUE(Text(ega).white(0, 0));
}

TEST(EgaRenderTest, CursorRowsEndBeforeTheEndRegister) {
    Ega ega;
    ega.reset();
    ProgramMode3(ega);
    PutCell(ega, 0, 0x00, 0x0F);
    Frame f = Text(ega);  // IBM's 0Bh/0Ch: row 11 only
    EXPECT_TRUE(f.black(0, 10));
    EXPECT_TRUE(f.white(0, 11));
    EXPECT_TRUE(f.black(0, 12));
    Crtc(ega, {{0x0B, 0x00}});  // end at or below start runs to the bottom
    f = Text(ega);
    EXPECT_TRUE(f.white(0, 13));
    EXPECT_TRUE(f.black(0, 10));
    Crtc(ega, {{0x0A, 0x1E}});  // IBM's cursor off
    EXPECT_TRUE(Text(ega).black(0, 13));
}

TEST(EgaRenderTest, CursorBlinksEverySixteenFrames) {
    Ega ega;
    ega.reset();
    ProgramMode3(ega);
    PutCell(ega, 0, 0x00, 0x0F);
    uint64_t now = 0;
    EXPECT_TRUE(Text(ega).white(0, 11));
    AdvanceFrames(ega, now, 15);
    EXPECT_TRUE(Text(ega).white(0, 11));
    AdvanceFrames(ega, now, 1);
    EXPECT_TRUE(Text(ega).black(0, 11));
}

TEST(EgaRenderTest, CursorSkewMovesTheCursorRight) {
    Ega ega;
    ega.reset();
    ProgramMode3(ega);
    PutCell(ega, 0, 0x00, 0x0F);
    PutCell(ega, 2, 0x00, 0x0F);
    Crtc(ega, {{0x0B, 0x4C}});
    Frame f = Text(ega);
    EXPECT_TRUE(f.black(0, 11));
    EXPECT_TRUE(f.white(16, 11));
}

TEST(EgaRenderTest, MonochromeAttributesUnderlineForegroundOne) {
    Ega ega;
    ega.reset();
    ProgramMode3(ega);
    Attr(ega, 0x10, 0x0A);
    Attr(ega, 0x01, 0x3F);
    Crtc(ega, {{0x14, 0x0C}, {0x0A, 0x1E}});
    PutCell(ega, 0, 0x00, 0x01);
    PutCell(ega, 1, 0x00, 0x07);
    Frame f = Text(ega);
    EXPECT_TRUE(f.white(0, 12));
    EXPECT_TRUE(f.black(0, 11));
    EXPECT_TRUE(f.black(8, 12));
    Attr(ega, 0x10, 0x08);
    EXPECT_TRUE(Text(ega).black(0, 12)) << "colour attributes have no underline";
}

TEST(EgaRenderTest, ColorPlaneEnableMasksTextColours) {
    Ega ega;
    ega.reset();
    ProgramMode3(ega);
    Attr(ega, 0x07, 0x07);
    PutCell(ega, 0, 'A', 0x0F);
    PutGlyphRow(ega, 0, 'A', 0, 0x80);
    Attr(ega, 0x12, 0x07);
    Frame f = Text(ega);
    EXPECT_EQ(f.r(0, 0), 0xAA);
}

TEST(EgaRenderTest, CgaGraphics4DecodesPlane0ThenPlane1AsFourPixelsEach) {
    Ega ega;
    ega.reset();
    ProgramMode3(ega);
    Crtc(ega, {{0x01, 0x27}, {0x09, 0x01}, {0x12, 0xC7}, {0x07, 0x11}, {0x13, 0x14}, {0x17, 0xA2}});
    Attr(ega, 0x10, 0x01);
    Attr(ega, 0x01, 0x01); Attr(ega, 0x02, 0x02); Attr(ega, 0x03, 0x3F);
    ega.vram[0] = 0x6C;  // blue, green, white, black
    ega.vram[1] = 0xC6;  // white, black, blue, green
    ega.vram[(0x2000u << 2) + 0] = 0xFF;  // B800:2000h, scanline 1
    std::vector<uint8_t> rgba;
    RenderCgaGraphics4Screen(ega, rgba);
    ASSERT_EQ(rgba.size(), std::size_t(320 * 200 * 4));
    auto px = [&](int x, int y) { return (std::size_t(y) * 320 + std::size_t(x)) * 4; };
    EXPECT_EQ(rgba[px(0, 0) + 2], 0xAA);
    EXPECT_EQ(rgba[px(1, 0) + 1], 0xAA);
    EXPECT_EQ(rgba[px(2, 0) + 0], 0xFF);
    EXPECT_EQ(rgba[px(3, 0) + 0], 0x00);
    EXPECT_EQ(rgba[px(4, 0) + 0], 0xFF);
    EXPECT_EQ(rgba[px(6, 0) + 2], 0xAA);
    EXPECT_EQ(rgba[px(0, 1) + 0], 0xFF);
}

// E12: mode 6 is planar plane 0 in byte mode, with 17h bit 0 putting odd lines 8KB up.
TEST(EgaRenderTest, Mode6UsesTheCgaBanks) {
    Ega ega;
    ega.reset();
    ProgramMode10(ega);
    Crtc(ega, {{0x09, 0x01}, {0x12, 0xC7}, {0x07, 0x11}, {0x17, 0xC2}});
    Attr(ega, 0x01, 0x3F);
    ega.vram[(0x2000u << 2) + 0] = 0x80;
    Frame f = Native(ega);
    EXPECT_EQ(f.h, 200);
    EXPECT_TRUE(f.black(0, 0));
    EXPECT_TRUE(f.white(0, 1));
}

TEST(EgaRenderTest, NativeSixteenColorDecodesOneBitPerPlane) {
    Ega ega;
    ega.reset();
    ProgramMode10(ega);
    Attr(ega, 0x05, 0x02);
    ega.vram[0] = 0x80;
    ega.vram[2] = 0x80;
    Frame f = Native(ega);
    EXPECT_EQ(f.g(0, 0), 0xAA);
    EXPECT_TRUE(f.black(1, 0));
}

TEST(EgaRenderTest, GraphicsBlinkDropsBitThreeInTheOffPhase) {
    Ega ega;
    ega.reset();
    ProgramMode10(ega);
    Attr(ega, 0x10, 0x09);
    Attr(ega, 0x0F, 0x3F);
    Attr(ega, 0x07, 0x07);
    for (int p = 0; p < 4; ++p) ega.vram[uint32_t(p)] = 0x80;
    uint64_t now = 0;
    EXPECT_TRUE(Native(ega).white(0, 0));
    AdvanceFrames(ega, now, 16);
    EXPECT_EQ(Native(ega).r(0, 0), 0xAA);
}

}  // namespace
