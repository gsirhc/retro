// GoogleTest suite for the shared EGA text-mode renderer: glyph decode
// from VRAM plane 2, palette decode via the live Attribute Controller
// registers, and the block cursor's visibility/shape/scanline range.

#include <gtest/gtest.h>

#include "ega.h"
#include "ega_render.h"

namespace {

using ibmpcat::Ega;
using ibmpcat::kTextRenderHeight;
using ibmpcat::kTextRenderWidth;
using ibmpcat::RenderTextScreen;

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

}  // namespace
