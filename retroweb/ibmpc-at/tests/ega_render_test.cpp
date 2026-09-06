// GoogleTest suite for the shared EGA text-mode renderer: glyph decode
// from VRAM plane 2, palette decode via the live Attribute Controller
// registers, and the block cursor's visibility/shape/scanline range.

#include <gtest/gtest.h>

#include "ega.h"
#include "ega_render.h"

namespace {

using ibmpcat::DetectScreenMode;
using ibmpcat::Ega;
using ibmpcat::kTextRenderHeight;
using ibmpcat::kTextRenderWidth;
using ibmpcat::RenderCgaGraphics4Screen;
using ibmpcat::RenderedFrame;
using ibmpcat::RenderScreen;
using ibmpcat::RenderTextScreen;
using ibmpcat::ScreenMode;

// Programs the two real Graphics Controller registers that select mode:
// GR06 bit 0 (graphics vs. alphanumeric) and GR05 bits 5-6 (Shift
// Register field).
void SetGraphicsMode(Ega &ega, bool graphics, uint8_t shift_register_mode) {
    ega.out(0x3CE, 0x06); ega.out(0x3CF, graphics ? 0x01 : 0x00);
    ega.out(0x3CE, 0x05); ega.out(0x3CF, uint8_t((shift_register_mode & 0x03) << 5));
}

// Programs Attribute Controller palette register `index` to raw EGA color
// `value` (the same address/data flip-flop port real software uses).
void SetPalette(Ega &ega, int index, uint8_t value) {
    ega.out(0x3C0, uint8_t(index));
    ega.out(0x3C0, value);
}

std::size_t PixelIndex(int x, int y) { return (std::size_t(y) * kTextRenderWidth + std::size_t(x)) * 4; }

TEST(EgaRenderTest, ProducesTheDocumentedBufferSize) {
    Ega ega;
    ega.reset();
    std::vector<uint8_t> rgba;
    RenderTextScreen(ega, rgba, false);
    EXPECT_EQ(rgba.size(), std::size_t(kTextRenderWidth * kTextRenderHeight * 4));
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
    RenderTextScreen(ega, rgba, /*blink_on=*/false);

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
    RenderTextScreen(ega, rgba, /*blink_on=*/true);
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
    RenderTextScreen(ega, rgba_blink_off, /*blink_on=*/false);
    std::size_t p = PixelIndex(0, 0);
    EXPECT_EQ(rgba_blink_off[p + 0], 0);  // background -- no cursor this phase

    ega.out(0x3D4, 0x0A); ega.out(0x3D5, 0x20);  // bit 5 -- cursor disabled outright
    std::vector<uint8_t> rgba_disabled;
    RenderTextScreen(ega, rgba_disabled, /*blink_on=*/true);
    EXPECT_EQ(rgba_disabled[p + 0], 0);
}

TEST(EgaRenderTest, DetectScreenModeReadsTheRealModeRegisters) {
    Ega ega;
    ega.reset();
    EXPECT_EQ(DetectScreenMode(ega), ScreenMode::kText);  // reset default: alphanumeric

    SetGraphicsMode(ega, /*graphics=*/true, /*shift_register_mode=*/1);
    EXPECT_EQ(DetectScreenMode(ega), ScreenMode::kCgaGraphics4);

    SetGraphicsMode(ega, /*graphics=*/true, /*shift_register_mode=*/0);
    EXPECT_EQ(DetectScreenMode(ega), ScreenMode::kUnsupportedGraphics);  // native 16-color, not yet rendered

    SetGraphicsMode(ega, /*graphics=*/false, /*shift_register_mode=*/1);
    EXPECT_EQ(DetectScreenMode(ega), ScreenMode::kText);  // alphanumeric bit wins regardless of shift mode
}

TEST(EgaRenderTest, CgaGraphics4DecodesPlane0ThenPlane1AsFourPixelsEach) {
    // Real hardware: a CGA-unaware program's two consecutive flat bytes for
    // scanline 0 (pixels 0-3, then 4-7) land at the same plane offset (0),
    // split across planes 0 and 1 by odd/even chaining -- see ega_render.h.
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
    // Scanline 1 (odd) starts at flat offset 0x2000, not 1 -- real CGA's
    // even/odd-scanline bank split, distinct from the plane odd/even split.
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

    SetGraphicsMode(ega, true, 0);  // native 16-color -- unsupported, placeholder frame
    RenderedFrame placeholder;
    RenderScreen(ega, placeholder, false);
    EXPECT_EQ(placeholder.width, kTextRenderWidth);
    EXPECT_EQ(placeholder.height, kTextRenderHeight);
    EXPECT_EQ(placeholder.rgba[0], 0);  // black, not a garbled misread of graphics VRAM as text
}

}  // namespace
