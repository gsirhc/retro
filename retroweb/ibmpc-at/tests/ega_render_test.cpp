#include <gtest/gtest.h>

#include "ega.h"
#include "ega_render.h"

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

// Programs GR06 bit 0 (graphics vs alphanumeric) and GR05 bits 5-6 (Shift Register).
void SetGraphicsMode(Ega &ega, bool graphics, uint8_t shift_register_mode) {
    ega.out(0x3CE, 0x06); ega.out(0x3CF, graphics ? 0x01 : 0x00);
    ega.out(0x3CE, 0x05); ega.out(0x3CF, uint8_t((shift_register_mode & 0x03) << 5));
}

// Programs palette register `index` through the 0x3C0 address/data flip-flop.
void SetPalette(Ega &ega, int index, uint8_t value) {
    ega.out(0x3C0, uint8_t(index));
    ega.out(0x3C0, value);
}

std::size_t PixelIndex(int x, int y) { return (std::size_t(y) * kTextRenderWidth + std::size_t(x)) * 4; }

TEST(EgaRenderTest, ProducesTheDocumentedBufferSize) {
    Ega ega;
    ega.reset();
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    RenderTextScreen(ega, rgba, false, w, h);
    EXPECT_EQ(w, kTextRenderWidth);
    EXPECT_EQ(h, kTextRenderHeight);
    EXPECT_EQ(rgba.size(), std::size_t(kTextRenderWidth * kTextRenderHeight * 4));
}

// The VGA BIOS programs 16-line rows (Max Scan Line = 15). The renderer must follow
// the register, or the last two scanlines (descenders) are discarded.
TEST(EgaRenderTest, RowHeightAndFrameSizeFollowTheRealMaxScanLineRegister) {
    Ega ega;
    ega.reset();
    ega.out(0x3D4, 0x09); ega.out(0x3D5, 0x0F);  // Max Scan Line = 15 -> 16 lines/row
    // 'g' (0x67) scanline 14 is part of a descender.
    uint32_t glyph_off = uint32_t('g') * 32 + 14;
    ega.vram[(glyph_off << 2) + 2] = 0xFF;  // every pixel in this row set
    ega.vram[(0 << 2) + 0] = 'g';
    ega.vram[(0 << 2) + 1] = 0x0F;  // fg=white, bg=black
    SetPalette(ega, 15, 0x3F);

    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    RenderTextScreen(ega, rgba, /*blink_on=*/false, w, h);
    EXPECT_EQ(w, kTextRenderWidth);
    EXPECT_EQ(h, 16 * 25);  // not a fixed 350
    ASSERT_EQ(rgba.size(), std::size_t(kTextRenderWidth * 16 * 25 * 4));

    std::size_t p = PixelIndex(0, 14);
    EXPECT_EQ(rgba[p + 0], 255); EXPECT_EQ(rgba[p + 1], 255); EXPECT_EQ(rgba[p + 2], 255);
}

// 40-column text has Horizontal Displayed (R01) = 39. A renderer that
// hardcodes 80 starts rows after the first at the wrong offset (IBM_PCAT_REVIEW.md).
TEST(EgaRenderTest, ColumnCountAndFrameWidthFollowTheRealHorizontalDisplayedRegister) {
    Ega ega;
    ega.reset();
    ega.out(0x3D4, 0x01); ega.out(0x3D5, 39);  // Horizontal Displayed = 39 -> 40 cols
    SetPalette(ega, 15, 0x3F);  // white
    // Cell (row=1, col=0) sits at offset 40 in 40-column mode.
    uint32_t cell = 40;
    ega.vram[(cell << 2) + 0] = 0x41;
    ega.vram[(cell << 2) + 1] = 0x0F;  // fg=white, bg=black
    uint32_t glyph_off = uint32_t(0x41) * 32 + 0;
    ega.vram[(glyph_off << 2) + 2] = 0x80;  // font row 0: leftmost pixel set

    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    RenderTextScreen(ega, rgba, /*blink_on=*/false, w, h);
    EXPECT_EQ(w, 40 * 8);  // not a fixed 640
    EXPECT_EQ(h, kTextRenderHeight);
    ASSERT_EQ(rgba.size(), std::size_t(w * h * 4));

    // Row 1's top-left pixel: y = 14 (default rows), x = 0.
    std::size_t p = (std::size_t(14) * std::size_t(w) + 0) * 4;
    EXPECT_EQ(rgba[p + 0], 255); EXPECT_EQ(rgba[p + 1], 255); EXPECT_EQ(rgba[p + 2], 255);
}

TEST(EgaRenderTest, RendersAGlyphInTheLivePaletteColors) {
    Ega ega;
    ega.reset();
    SetPalette(ega, 15, 0x3F);  // white
    SetPalette(ega, 1, 0x01);   // blue
    // Cell (0,0): character code 0x41, attribute fg=15 (white) / bg=1 (blue).
    ega.vram[(0 << 2) + 0] = 0x41;
    ega.vram[(0 << 2) + 1] = 0x1F;
    // Font row 0 of character 0x41: only the leftmost pixel set.
    uint32_t glyph_off = uint32_t(0x41) * 32 + 0;
    ega.vram[(glyph_off << 2) + 2] = 0x80;

    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    RenderTextScreen(ega, rgba, /*blink_on=*/false, w, h);

    std::size_t i0 = PixelIndex(0, 0);  // set bit -> foreground (white)
    EXPECT_EQ(rgba[i0 + 0], 255); EXPECT_EQ(rgba[i0 + 1], 255); EXPECT_EQ(rgba[i0 + 2], 255);
    EXPECT_EQ(rgba[i0 + 3], 255);  // alpha always opaque

    std::size_t i1 = PixelIndex(1, 0);  // clear bit -> background (blue)
    EXPECT_EQ(rgba[i1 + 0], 0); EXPECT_EQ(rgba[i1 + 1], 0); EXPECT_EQ(rgba[i1 + 2], 0xAA);
}

TEST(EgaRenderTest, CursorDrawsAsASolidBlockAtItsProgrammedScanlines) {
    Ega ega;
    ega.reset();
    SetPalette(ega, 15, 0x3F);
    // Cell (0,0) holds a blank character (font row all zero, so without the
    // cursor override every pixel would read as background).
    ega.vram[(0 << 2) + 0] = 0x00;
    ega.vram[(0 << 2) + 1] = 0x0F;  // fg=white, bg=black(0)
    ega.out(0x3D4, 0x0E); ega.out(0x3D5, 0x00);  // cursor location high
    ega.out(0x3D4, 0x0F); ega.out(0x3D5, 0x00);  // cursor location low -> cell (0,0)
    ega.out(0x3D4, 0x0A); ega.out(0x3D5, 0x05);  // cursor start scanline 5, not disabled
    ega.out(0x3D4, 0x0B); ega.out(0x3D5, 0x06);  // cursor end scanline 6

    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    RenderTextScreen(ega, rgba, /*blink_on=*/true, w, h);
    // Scanline 5 (within the cursor's range): forced to foreground (white).
    std::size_t in_range = PixelIndex(0, 5);
    EXPECT_EQ(rgba[in_range + 0], 255); EXPECT_EQ(rgba[in_range + 1], 255); EXPECT_EQ(rgba[in_range + 2], 255);
    // Scanline 4 (outside the range): the blank glyph's background (black).
    std::size_t out_of_range = PixelIndex(0, 4);
    EXPECT_EQ(rgba[out_of_range + 0], 0); EXPECT_EQ(rgba[out_of_range + 1], 0); EXPECT_EQ(rgba[out_of_range + 2], 0);
}

TEST(EgaRenderTest, CursorIsHiddenWhenBlinkPhaseIsOffOrTheDisableBitIsSet) {
    Ega ega;
    ega.reset();
    SetPalette(ega, 15, 0x3F);
    ega.vram[(0 << 2) + 0] = 0x00;
    ega.vram[(0 << 2) + 1] = 0x0F;
    ega.out(0x3D4, 0x0A); ega.out(0x3D5, 0x00);
    ega.out(0x3D4, 0x0B); ega.out(0x3D5, 0x0D);  // covers the whole cell (0-13)

    std::vector<uint8_t> rgba_blink_off;
    int w = 0, h = 0;
    RenderTextScreen(ega, rgba_blink_off, /*blink_on=*/false, w, h);
    std::size_t p = PixelIndex(0, 0);
    EXPECT_EQ(rgba_blink_off[p + 0], 0);  // background -- no cursor this phase

    ega.out(0x3D4, 0x0A); ega.out(0x3D5, 0x20);  // bit 5 -- cursor disabled outright
    std::vector<uint8_t> rgba_disabled;
    RenderTextScreen(ega, rgba_disabled, /*blink_on=*/true, w, h);
    EXPECT_EQ(rgba_disabled[p + 0], 0);
}

TEST(EgaRenderTest, DetectScreenModeReadsTheRealModeRegisters) {
    Ega ega;
    ega.reset();
    EXPECT_EQ(DetectScreenMode(ega), ScreenMode::kText);  // reset default: alphanumeric

    SetGraphicsMode(ega, /*graphics=*/true, /*shift_register_mode=*/1);
    EXPECT_EQ(DetectScreenMode(ega), ScreenMode::kCgaGraphics4);

    SetGraphicsMode(ega, /*graphics=*/true, /*shift_register_mode=*/0);
    EXPECT_EQ(DetectScreenMode(ega), ScreenMode::kEgaGraphics16);  // real, native EGA planar graphics

    SetGraphicsMode(ega, /*graphics=*/true, /*shift_register_mode=*/2);
    EXPECT_EQ(DetectScreenMode(ega), ScreenMode::kUnsupportedGraphics);  // VGA-only 256-color Chain-4

    SetGraphicsMode(ega, /*graphics=*/false, /*shift_register_mode=*/1);
    EXPECT_EQ(DetectScreenMode(ega), ScreenMode::kText);  // alphanumeric bit wins regardless of shift mode
}

TEST(EgaRenderTest, CgaGraphics4DecodesPlane0ThenPlane1AsFourPixelsEach) {
    // A CGA program's two flat bytes for scanline 0 land at plane offset 0, split across planes 0 and 1.
    Ega ega;
    ega.reset();
    SetPalette(ega, 0, 0x00);  // black
    SetPalette(ega, 1, 0x01);  // blue
    SetPalette(ega, 2, 0x02);  // green
    SetPalette(ega, 3, 0x3F);  // white
    ega.vram[(0 << 2) + 0] = 0x6C;  // 01 10 11 00 -> pixels 0-3: blue,green,white,black
    ega.vram[(0 << 2) + 1] = 0xC6;  // 11 00 01 10 -> pixels 4-7: white,black,blue,green

    std::vector<uint8_t> rgba;
    RenderCgaGraphics4Screen(ega, rgba);
    ASSERT_EQ(rgba.size(), std::size_t(320 * 200 * 4));

    auto px = [&](int x) { return (std::size_t(x)) * 4; };
    EXPECT_EQ(rgba[px(0) + 2], 0xAA);  // blue -> b channel set
    EXPECT_EQ(rgba[px(1) + 1], 0xAA);  // green -> g channel set
    EXPECT_EQ(rgba[px(2) + 0], 0xFF);  // white -> all channels set
    EXPECT_EQ(rgba[px(2) + 1], 0xFF);
    EXPECT_EQ(rgba[px(2) + 2], 0xFF);
    EXPECT_EQ(rgba[px(3) + 0], 0x00);  // black -> all channels clear
    EXPECT_EQ(rgba[px(4) + 0], 0xFF);  // white again -- plane 1's byte, first pixel
    EXPECT_EQ(rgba[px(5) + 0], 0x00);  // black
    EXPECT_EQ(rgba[px(6) + 2], 0xAA);  // blue
    EXPECT_EQ(rgba[px(7) + 1], 0xAA);  // green
}

TEST(EgaRenderTest, CgaGraphics4OddScanlinesUseTheSecondEightKilobyteBank) {
    // Odd scanlines start at flat offset 0x2000 (CGA bank split, separate from plane odd/even).
    Ega ega;
    ega.reset();
    SetPalette(ega, 3, 0x3F);  // white
    uint32_t linear_offset = 0x2000;  // scanline 1, byte column 0
    uint32_t plane = linear_offset & 1, plane_offset = linear_offset >> 1;
    ega.vram[(plane_offset << 2) + plane] = 0xFF;  // all four pixels = value 3 (white)

    std::vector<uint8_t> rgba;
    RenderCgaGraphics4Screen(ega, rgba);
    std::size_t i = (std::size_t(1) * 320 + 0) * 4;  // (x=0, y=1)
    EXPECT_EQ(rgba[i + 0], 0xFF);
    EXPECT_EQ(rgba[i + 1], 0xFF);
    EXPECT_EQ(rgba[i + 2], 0xFF);
}

TEST(EgaRenderTest, EgaNative16ResolutionComesFromCrtcTimingNotATable) {
    // Register values from this BIOS's INT 10h AL=0x10 (mode 0x10, 640x350x16), IBM_PCAT_REVIEW.md §16.
    Ega ega;
    ega.reset();
    ega.out(0x3D4, 0x01); ega.out(0x3D5, 79);    // H Display End
    ega.out(0x3D4, 0x12); ega.out(0x3D5, 0x5D);  // V Display End low 8 bits
    ega.out(0x3D4, 0x07); ega.out(0x3D5, 0x02);  // Overflow: bit 1 set

    std::vector<uint8_t> rgba;
    int width = 0, height = 0;
    RenderEgaNative16Screen(ega, rgba, width, height);
    EXPECT_EQ(width, 640);
    EXPECT_EQ(height, 350);
    EXPECT_EQ(rgba.size(), std::size_t(640 * 350 * 4));
}

TEST(EgaRenderTest, EgaNative16DecodesOneBitPerPlanePerPixelMsbFirst) {
    // Plane 0 = bit 0 of the color index through plane 3 = bit 3; bit 7 of a byte is the leftmost pixel.
    Ega ega;
    ega.reset();
    ega.out(0x3D4, 0x01); ega.out(0x3D5, 9);     // H Display End -> (9+1)*8 = 80 wide (small, for the test)
    ega.out(0x3D4, 0x12); ega.out(0x3D5, 0);     // V Display End -> 0+1 = 1 tall
    SetPalette(ega, 0x0, 0x00);  // black
    SetPalette(ega, 0x5, 0x02);  // index 5 = green (planes 0 and 2 set: bits 0+2 = 0b0101 = 5)
    // Planes 0 and 2 have their MSB set: leftmost pixel index = 0b0101 = 5.
    ega.vram[(0 << 2) + 0] = 0x80;
    ega.vram[(0 << 2) + 2] = 0x80;

    std::vector<uint8_t> rgba;
    int width = 0, height = 0;
    RenderEgaNative16Screen(ega, rgba, width, height);
    ASSERT_EQ(width, 80);
    EXPECT_EQ(rgba[0], 0x00); EXPECT_EQ(rgba[1], 0xAA); EXPECT_EQ(rgba[2], 0x00);  // green
    EXPECT_EQ(rgba[4], 0x00); EXPECT_EQ(rgba[5], 0x00); EXPECT_EQ(rgba[6], 0x00);  // next pixel: black
}

TEST(EgaRenderTest, RenderScreenDispatchesToTheRightModeAtTheRightResolution) {
    Ega ega;
    ega.reset();
    RenderedFrame text_frame;
    RenderScreen(ega, text_frame, /*blink_on=*/false);
    EXPECT_EQ(text_frame.width, kTextRenderWidth);
    EXPECT_EQ(text_frame.height, kTextRenderHeight);

    SetGraphicsMode(ega, true, 1);
    RenderedFrame cga_frame;
    RenderScreen(ega, cga_frame, false);
    EXPECT_EQ(cga_frame.width, 320);
    EXPECT_EQ(cga_frame.height, 200);

    // Native 16-color: resolution comes from the CRTC (mode 0x10 values).
    SetGraphicsMode(ega, true, 0);
    ega.out(0x3D4, 0x01); ega.out(0x3D5, 79);    // H Display End -> (79+1)*8 = 640
    ega.out(0x3D4, 0x12); ega.out(0x3D5, 0x5D);  // V Display End low 8 bits = 93
    ega.out(0x3D4, 0x07); ega.out(0x3D5, 0x02);  // overflow bit1 set -> +256 = 349 -> +1 = 350
    RenderedFrame native16_frame;
    RenderScreen(ega, native16_frame, false);
    EXPECT_EQ(native16_frame.width, 640);
    EXPECT_EQ(native16_frame.height, 350);

    SetGraphicsMode(ega, true, 2);  // VGA-only Chain-4 -- unsupported, placeholder frame
    RenderedFrame placeholder;
    RenderScreen(ega, placeholder, false);
    EXPECT_EQ(placeholder.width, kTextRenderWidth);
    EXPECT_EQ(placeholder.height, kTextRenderHeight);
    EXPECT_EQ(placeholder.rgba[0], 0);  // black, not a garbled misread of graphics VRAM as text
}

}  // namespace
