// GoogleTest suite for the shared EGA/VGA renderer: glyph decode from VRAM
// plane 2, palette decode via the live Attribute Controller registers, the
// block cursor's visibility/shape/scanline range, the CGA-compatibility and
// native 16-color planar graphics decodes, and (Milestone 3) the VGA
// 256-color path -- mode 13h's chain-4 byte-per-pixel frame buffer through
// the real DAC, and the card's SVGA linear modes. See PC486_REVIEW.md §7.

#include <gtest/gtest.h>

#include "ega.h"
#include "ega_render.h"

namespace {

using pc486::DetectScreenMode;
using pc486::Ega;
using pc486::kTextRenderHeight;
using pc486::kTextRenderWidth;
using pc486::RenderCgaGraphics4Screen;
using pc486::RenderEgaNative16Screen;
using pc486::RenderedFrame;
using pc486::RenderScreen;
using pc486::RenderTextScreen;
using pc486::RenderVga256Screen;
using pc486::ScreenMode;

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

// Programs one of the 256 DAC color registers through the real PEL Address
// Write / PEL Data ports, in raw 6-bit-per-channel values.
void SetDac(Ega &ega, int index, uint8_t r, uint8_t g, uint8_t b) {
    ega.out(0x3C8, uint8_t(index));
    ega.out(0x3C9, r); ega.out(0x3C9, g); ega.out(0x3C9, b);
}

// Exactly the register values this machine's own BIOS was observed to
// program for mode 13h, read back off the live card by vbe_mode13_check --
// see PC486_REVIEW.md §7. Nothing here is a guess at "what mode 13h ought
// to look like"; it is what the firmware actually wrote.
void SetupMode13h(Ega &ega) {
    ega.out(0x3C4, 0x02); ega.out(0x3C5, 0x0F);  // Sequencer Map Mask: all 4 planes
    ega.out(0x3C4, 0x04); ega.out(0x3C5, 0x0E);  // Memory Mode: Chain 4 on, odd/even off
    ega.out(0x3CE, 0x05); ega.out(0x3CF, 0x40);  // GC Mode: Shift Register field = 2 (256-color)
    ega.out(0x3CE, 0x06); ega.out(0x3CF, 0x05);  // GC Misc: graphics, 64K @ A0000
    ega.out(0x3CE, 0x08); ega.out(0x3CF, 0xFF);  // Bit Mask: all bits pass through
    ega.out(0x3D4, 0x01); ega.out(0x3D5, 79);    // CRTC H Display End: 640 dots of timing
    ega.out(0x3D4, 0x12); ega.out(0x3D5, 0x8F);  // CRTC V Display End low 8 bits = 143
    ega.out(0x3D4, 0x07); ega.out(0x3D5, 0x1F);  // Overflow bit 1 -> +256 = 399
    ega.out(0x3D4, 0x09); ega.out(0x3D5, 0x41);  // Max Scan Line = 1 -> 2 lines/row, no doubling
    ega.out(0x3D4, 0x13); ega.out(0x3D5, 40);    // Offset = 40 units
    ega.out(0x3D4, 0x14); ega.out(0x3D5, 0x40);  // Underline Location bit 6: Doubleword Mode
    ega.out(0x3D4, 0x17); ega.out(0x3D5, 0xA3);  // Mode Control: byte-mode bit clear
    ega.in(0x3DA);                                // reset the AC flip-flop to "address"
    ega.out(0x3C0, 0x10); ega.out(0x3C0, 0x41);  // AC Mode Control: graphics + 8-bit color
}

// Switches the card into an SVGA (VBE-programmed) linear mode through its
// extension registers, the way the card's own ROM does on a 4F02.
void SetSvgaMode(Ega &ega, uint16_t xres, uint16_t yres) {
    auto put = [&](uint16_t reg, uint16_t v) {
        ega.out16(Ega::kVbeIndexPort, reg);
        ega.out16(Ega::kVbeDataPort, v);
    };
    put(Ega::kVbeRegXres, xres);
    put(Ega::kVbeRegYres, yres);
    put(Ega::kVbeRegBpp, 8);
    put(Ega::kVbeRegEnable, Ega::kVbeEnabled);
}

// Asserts one rendered pixel's exact RGB -- no tolerance: a DAC channel
// that scales wrong by one step is a real bug, not a rounding preference.
void ExpectRgb(const std::vector<uint8_t> &rgba, int width, int x, int y,
               uint8_t r, uint8_t g, uint8_t b) {
    std::size_t i = (std::size_t(y) * std::size_t(width) + std::size_t(x)) * 4;
    ASSERT_LT(i + 3, rgba.size());
    EXPECT_EQ(rgba[i + 0], r) << "red at (" << x << "," << y << ")";
    EXPECT_EQ(rgba[i + 1], g) << "green at (" << x << "," << y << ")";
    EXPECT_EQ(rgba[i + 2], b) << "blue at (" << x << "," << y << ")";
    EXPECT_EQ(rgba[i + 3], 255) << "alpha at (" << x << "," << y << ")";
}

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

// Real hardware fact this covers: this machine's freely-licensed BIOS
// substitute is a full VGA BIOS and programs VGA's native 16-line-per-row
// text mode (Max Scan Line = 15) rather than genuine EGA's own 14-line
// convention (see ega_render.h's RenderTextScreen comment). The renderer
// must follow that real register, not a hardcoded row height -- otherwise
// every glyph's last two scanlines (exactly where the VGA 8x16 font draws
// descenders on g/y/p/q/j) get silently discarded.
TEST(EgaRenderTest, RowHeightAndFrameSizeFollowTheRealMaxScanLineRegister) {
    Ega ega;
    ega.reset();
    ega.out(0x3D4, 0x09); ega.out(0x3D5, 0x0F);  // Max Scan Line = 15 -> 16 lines/row
    // Character 'g' (0x67), scanline 14 -- part of a real descender, and
    // exactly the row the old hardcoded 14-line renderer never reached.
    uint32_t glyph_off = uint32_t('g') * 32 + 14;
    ega.vram[(glyph_off << 2) + 2] = 0xFF;  // every pixel in this row set
    ega.vram[(0 << 2) + 0] = 'g';
    ega.vram[(0 << 2) + 1] = 0x0F;  // fg=white, bg=black
    SetPalette(ega, 15, 0x3F);

    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    RenderTextScreen(ega, rgba, /*blink_on=*/false, w, h);
    EXPECT_EQ(w, kTextRenderWidth);
    EXPECT_EQ(h, 16 * 25);  // 400, not the old fixed 350
    ASSERT_EQ(rgba.size(), std::size_t(kTextRenderWidth * 16 * 25 * 4));

    std::size_t p = PixelIndex(0, 14);
    EXPECT_EQ(rgba[p + 0], 255); EXPECT_EQ(rgba[p + 1], 255); EXPECT_EQ(rgba[p + 2], 255);
}

// Real hardware fact this covers: 40-column text (BIOS mode 0/1) is a
// genuine CRTC configuration -- Horizontal Displayed (R01) = 39, not 79 --
// that period DOS software legitimately uses for a large-character screen
// (confirmed live against MECC's The Oregon Trail's "Look at map" screen,
// which renders exactly this way). VRAM stays laid out row*cols+col with
// cols=40 in this mode, so a renderer that hardcodes 80 columns starts
// every row after the first at the wrong offset -- reading half of row 1
// from the tail of row 0 and the other half from row 1's own first bytes
// -- which is exactly the scrambled-glyph-noise failure mode the file
// header warns about, just reached through a missed CRTC register instead
// of a missed graphics-mode bit. See PC486_REVIEW.md.
TEST(EgaRenderTest, ColumnCountAndFrameWidthFollowTheRealHorizontalDisplayedRegister) {
    Ega ega;
    ega.reset();
    ega.out(0x3D4, 0x01); ega.out(0x3D5, 39);  // Horizontal Displayed = 39 -> 40 cols
    SetPalette(ega, 15, 0x3F);  // white
    // Cell (row=1, col=0) sits at the 40-column offset 40 -- at the
    // hardcoded-80 offset that same VRAM slot would instead land mid-row 0.
    uint32_t cell = 40;
    ega.vram[(cell << 2) + 0] = 0x41;
    ega.vram[(cell << 2) + 1] = 0x0F;  // fg=white, bg=black
    uint32_t glyph_off = uint32_t(0x41) * 32 + 0;
    ega.vram[(glyph_off << 2) + 2] = 0x80;  // font row 0: leftmost pixel set

    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    RenderTextScreen(ega, rgba, /*blink_on=*/false, w, h);
    EXPECT_EQ(w, 40 * 8);  // 320, not the old fixed 640
    EXPECT_EQ(h, kTextRenderHeight);
    ASSERT_EQ(rgba.size(), std::size_t(w * h * 4));

    // Row 1's top-left pixel: y = 1 row * 14 (default scan lines/row) = 14, x = 0.
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
    EXPECT_EQ(DetectScreenMode(ega), ScreenMode::kVga256);  // VGA 256-color (mode 13h)

    // Shift Register field 3 is not a mode real VGA silicon defines -- an
    // honest black frame, not a guess at which decode was meant.
    SetGraphicsMode(ega, /*graphics=*/true, /*shift_register_mode=*/3);
    EXPECT_EQ(DetectScreenMode(ega), ScreenMode::kUnsupportedGraphics);

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

TEST(EgaRenderTest, EgaNative16ResolutionComesFromCrtcTimingNotATable) {
    // Verified against this machine's own real BIOS: directly invoking its
    // INT 10h AL=0x10 handler and reading back what it programs gives
    // exactly these register values for genuine mode 0x10 (640x350x16) --
    // see PC486_REVIEW.md §16.
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
    // Real EGA/VGA convention: plane 0 = bit 0 (LSB) of the 4-bit color
    // index, through plane 3 = bit 3 (MSB); within a byte, bit 7 is the
    // leftmost pixel (MSB-first, the same convention text mode's glyph
    // bytes and CGA-mode's pixel bytes already use).
    Ega ega;
    ega.reset();
    ega.out(0x3D4, 0x01); ega.out(0x3D5, 9);     // H Display End -> (9+1)*8 = 80 wide (small, for the test)
    ega.out(0x3D4, 0x12); ega.out(0x3D5, 0);     // V Display End -> 0+1 = 1 tall
    SetPalette(ega, 0x0, 0x00);  // black
    SetPalette(ega, 0x5, 0x02);  // index 5 = green (planes 0 and 2 set: bits 0+2 = 0b0101 = 5)
    // Plane 0 byte and plane 2 byte both have their MSB set (leftmost
    // pixel); planes 1 and 3 are 0 -- leftmost pixel's index = 0b0101 = 5.
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

    // Native 16-color EGA: resolution comes from the CRTC, not a table --
    // program real mode-0x10 Horizontal/Vertical Display End values.
    SetGraphicsMode(ega, true, 0);
    ega.out(0x3D4, 0x01); ega.out(0x3D5, 79);    // H Display End -> (79+1)*8 = 640
    ega.out(0x3D4, 0x12); ega.out(0x3D5, 0x5D);  // V Display End low 8 bits = 93
    ega.out(0x3D4, 0x07); ega.out(0x3D5, 0x02);  // overflow bit1 set -> +256 = 349 -> +1 = 350
    RenderedFrame native16_frame;
    RenderScreen(ega, native16_frame, false);
    EXPECT_EQ(native16_frame.width, 640);
    EXPECT_EQ(native16_frame.height, 350);

    // VGA 256-color: 320x200 out of the same 640-dot CRTC timing, because
    // the Attribute Controller's 8-bit-color bit halves the pixel clock.
    SetupMode13h(ega);
    RenderedFrame vga256_frame;
    RenderScreen(ega, vga256_frame, false);
    EXPECT_EQ(vga256_frame.width, 320);
    EXPECT_EQ(vga256_frame.height, 200);

    SetGraphicsMode(ega, true, 3);  // no such shift mode on real VGA -- placeholder frame
    RenderedFrame placeholder;
    RenderScreen(ega, placeholder, false);
    EXPECT_EQ(placeholder.width, kTextRenderWidth);
    EXPECT_EQ(placeholder.height, kTextRenderHeight);
    EXPECT_EQ(placeholder.rgba[0], 0);  // black, not a garbled misread of graphics VRAM as text
}

// --- VGA 256-color (mode 13h) --------------------------------------------

TEST(EgaRenderTest, Vga256ResolutionComesFromCrtcAndAttributeControllerNotATable) {
    // Every number here is what this machine's own BIOS was observed to
    // program for mode 13h, read back off the live card by
    // vbe_mode13_check (see PC486_REVIEW.md §7). The CRTC alone describes a
    // 640x400 raster; only the Attribute Controller's 8-bit-color bit and
    // the Maximum Scan Line register fold it to the real 320x200.
    Ega ega;
    ega.reset();
    SetupMode13h(ega);
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    RenderVga256Screen(ega, rgba, w, h);
    EXPECT_EQ(w, 320);
    EXPECT_EQ(h, 200);
    EXPECT_EQ(rgba.size(), std::size_t(320 * 200 * 4));

    // Drop the 8-bit-color bit and the same CRTC now genuinely describes a
    // 640-wide picture -- proof the 320 is derived, not hardcoded.
    ega.out(0x3C0, 0x10); ega.out(0x3C0, 0x01);
    RenderVga256Screen(ega, rgba, w, h);
    EXPECT_EQ(w, 640);
}

TEST(EgaRenderTest, Vga256DecodesOneBytePerPixelThroughTheLiveDac) {
    Ega ega;
    ega.reset();
    SetupMode13h(ega);
    SetDac(ega, 7, 63, 0, 21);
    SetDac(ega, 200, 0, 32, 63);
    // Chain-4 makes the flat frame-buffer offset the VRAM index -- write
    // through the real memory interface, not straight into the array.
    ega.mem_write(0xA0000 + 0, 7);          // (0,0)
    ega.mem_write(0xA0000 + 319, 200);      // (319,0)
    ega.mem_write(0xA0000 + 320, 200);      // (0,1) -- one scan line down

    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    RenderVga256Screen(ega, rgba, w, h);
    // 6-bit DAC channels scaled to full-scale 8-bit: 63 -> 255, 21 -> 85,
    // 32 -> 130, 0 -> 0.
    ExpectRgb(rgba, w, 0, 0, 255, 0, 85);
    ExpectRgb(rgba, w, 319, 0, 0, 130, 255);
    ExpectRgb(rgba, w, 0, 1, 0, 130, 255);
    ExpectRgb(rgba, w, 1, 0, 0, 0, 0);  // untouched -> DAC entry 0, black
}

TEST(EgaRenderTest, Vga256ScanLineStrideUsesTheDoublewordAddressUnit) {
    // The Offset Register reads 40 in both mode 10h and mode 13h, and means
    // 80 bytes/line in one and 320 in the other -- the difference is the
    // CRTC's Underline Location doubleword bit. Clearing it here must move
    // the second scan line's source data, or the stride was hardcoded.
    Ega ega;
    ega.reset();
    SetupMode13h(ega);
    EXPECT_EQ(ega.crtc_address_unit_bytes(), 4);
    EXPECT_EQ(ega.crtc_row_byte_stride(), 320);

    ega.out(0x3D4, 0x14); ega.out(0x3D5, 0x00);  // clear Doubleword Mode
    ega.out(0x3D4, 0x17); ega.out(0x3D5, 0xE3);  // Mode Control bit 6 = Byte Mode
    EXPECT_EQ(ega.crtc_address_unit_bytes(), 1);
    EXPECT_EQ(ega.crtc_row_byte_stride(), 80);

    SetDac(ega, 9, 63, 63, 63);
    ega.mem_write(0xA0000 + 80, 9);  // (0,1) at the now-80-byte stride
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    RenderVga256Screen(ega, rgba, w, h);
    ExpectRgb(rgba, w, 0, 1, 255, 255, 255);
}

TEST(EgaRenderTest, Vga256AppliesThePelMaskBetweenPixelAndDac) {
    // Real hardware ANDs the pixel value with the PEL Mask on its way to
    // the DAC's address lines -- it does not alter a single stored color,
    // which is exactly why period code uses it for palette tricks.
    Ega ega;
    ega.reset();
    SetupMode13h(ega);
    SetDac(ega, 0x0F, 63, 0, 0);
    SetDac(ega, 0xFF, 0, 63, 0);
    ega.mem_write(0xA0000, 0xFF);

    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    RenderVga256Screen(ega, rgba, w, h);
    ExpectRgb(rgba, w, 0, 0, 0, 255, 0);  // full mask: entry 0xFF

    ega.out(0x3C6, 0x0F);  // fold the top nibble away
    RenderVga256Screen(ega, rgba, w, h);
    ExpectRgb(rgba, w, 0, 0, 255, 0, 0);  // same pixel byte now reaches entry 0x0F
}

TEST(EgaRenderTest, Vga256StartAddressScrollsByWholeAddressUnits) {
    // The CRTC Start Address is in the same doubleword units as the Offset
    // Register here, so a start of 80 moves the picture forward by 320
    // bytes -- exactly one scan line, the real hardware scroll.
    Ega ega;
    ega.reset();
    SetupMode13h(ega);
    SetDac(ega, 5, 63, 63, 0);
    ega.mem_write(0xA0000 + 320, 5);  // (0,1) with no scroll

    ega.out(0x3D4, 0x0C); ega.out(0x3D5, 0x00);
    ega.out(0x3D4, 0x0D); ega.out(0x3D5, 80);  // start = 80 units = 320 bytes
    EXPECT_EQ(ega.start_byte_offset(), 320u);

    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    RenderVga256Screen(ega, rgba, w, h);
    ExpectRgb(rgba, w, 0, 0, 255, 255, 0);  // that pixel is now the top-left one
}

// Real DOS software commonly disables chain-4 while keeping 256-color
// shift-out selected ("unchained mode 13h") to write one plane at a time
// through Map Mask -- id's DOOM engine does this for its column renderer
// and for page-flipping among up to four 64KB-aligned buffers in the
// card's 256KB of VRAM. Once chain-4 is off, the CPU's write address no
// longer equals the interleaved vram[] index the way it does under
// chain-4, so the renderer must walk plane_off/plane directly instead of
// a flat byte offset -- this reproduces the corrupted, tiled/banded
// screen a real DOOM install produced on this emulator before the fix.
// See PC486_REVIEW.md.
TEST(EgaRenderTest, Vga256UnchainedWalksPlaneOffDirectlyNotAFlatOffset) {
    Ega ega;
    ega.reset();
    SetupMode13h(ega);
    ega.out(0x3C4, 0x04); ega.out(0x3C5, 0x06);  // Memory Mode: Chain 4 OFF, odd/even disabled
    ega.out(0x3D4, 0x14); ega.out(0x3D5, 0x00);  // clear Doubleword Mode
    ega.out(0x3D4, 0x17); ega.out(0x3D5, 0xE3);  // Mode Control: Byte Mode
    EXPECT_EQ(ega.chain4_enabled(), false);

    SetDac(ega, 11, 63, 0, 0);
    SetDac(ega, 22, 0, 63, 0);
    SetDac(ega, 33, 0, 0, 63);
    SetDac(ega, 44, 63, 63, 0);

    // Four consecutive displayed pixels (x=0..3) are one byte from each of
    // the four planes at the same plane_off=0 -- select each plane through
    // Map Mask and write it individually, exactly like the real unchained
    // write path (and Doom's own per-plane column blit) does.
    auto write_plane = [&](int plane, uint8_t value) {
        ega.out(0x3C4, 0x02); ega.out(0x3C5, uint8_t(1 << plane));
        ega.mem_write(0xA0000 + 0, value);
    };
    write_plane(0, 11);
    write_plane(1, 22);
    write_plane(2, 33);
    write_plane(3, 44);

    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    RenderVga256Screen(ega, rgba, w, h);
    ExpectRgb(rgba, w, 0, 0, 255, 0, 0);
    ExpectRgb(rgba, w, 1, 0, 0, 255, 0);
    ExpectRgb(rgba, w, 2, 0, 0, 0, 255);
    ExpectRgb(rgba, w, 3, 0, 255, 255, 0);
}

// The CRTC Start Address register still counts in the same per-plane-group
// units mem_write()'s plane_off does, regardless of chain-4 -- unaffected
// by the byte/word/dword bits, which only ever scaled the flat address
// chain-4 exposes to the CPU. This is exactly Doom's page-flip mechanism:
// up to four 64KB-aligned buffers selected by Start Address alone.
TEST(EgaRenderTest, Vga256UnchainedStartAddressSelectsA64KAlignedBuffer) {
    Ega ega;
    ega.reset();
    SetupMode13h(ega);
    ega.out(0x3C4, 0x04); ega.out(0x3C5, 0x06);  // Chain 4 OFF, odd/even disabled
    ega.out(0x3D4, 0x14); ega.out(0x3D5, 0x00);
    ega.out(0x3D4, 0x17); ega.out(0x3D5, 0xE3);

    SetDac(ega, 55, 10, 20, 30);
    // Start Address = 0x4000 (16384) plane_off units -> the second of the
    // four 64KB-aligned unchained buffers.
    ega.out(0x3D4, 0x0C); ega.out(0x3D5, 0x40);
    ega.out(0x3D4, 0x0D); ega.out(0x3D5, 0x00);
    ega.out(0x3C4, 0x02); ega.out(0x3C5, 0x01);  // Map Mask: plane 0
    // With chain-4 and odd/even both off, plane_off == the raw CPU offset.
    ega.mem_write(0xA0000 + 0x4000, 55);         // plane_off 0x4000, plane 0

    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    RenderVga256Screen(ega, rgba, w, h);
    ExpectRgb(rgba, w, 0, 0, 40, 81, 121);
}

// --- SVGA (VBE-programmed) linear modes ----------------------------------

TEST(EgaRenderTest, SvgaLinearGeometryComesFromTheExtensionRegistersNotTheCrtc) {
    // In an SVGA mode the card's own extension registers describe the
    // picture and the legacy CRTC keeps whatever the previous mode left --
    // so this test deliberately leaves mode 13h's CRTC values in place and
    // still expects 640x400.
    Ega ega;
    ega.reset();
    SetupMode13h(ega);
    SetSvgaMode(ega, 640, 400);
    EXPECT_EQ(DetectScreenMode(ega), ScreenMode::kVga256);

    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    RenderVga256Screen(ega, rgba, w, h);
    EXPECT_EQ(w, 640);
    EXPECT_EQ(h, 400);

    SetDac(ega, 11, 63, 0, 63);
    ega.mem_write(0xA0000 + 640, 11);  // (0,1) at the 640-byte linear stride
    RenderVga256Screen(ega, rgba, w, h);
    ExpectRgb(rgba, w, 0, 1, 255, 0, 255);
}

TEST(EgaRenderTest, SvgaVirtualWidthAndOffsetsPanTheVisibleWindow) {
    // A logical line wider than the displayed one, panned by the X/Y
    // offset registers -- the SVGA equivalent of the CRTC Start Address,
    // and how period software double-buffers in a VBE mode.
    Ega ega;
    ega.reset();
    SetSvgaMode(ega, 320, 200);
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegVirtWidth);
    ega.out16(Ega::kVbeDataPort, 512);  // wider logical line than the 320 shown

    SetDac(ega, 21, 0, 63, 63);
    // With Y offset 2 and X offset 3, the top-left pixel comes from linear
    // offset 2*512 + 3.
    ega.mem_write(0xA0000 + 2 * 512 + 3, 21);
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegXOffset); ega.out16(Ega::kVbeDataPort, 3);
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegYOffset); ega.out16(Ega::kVbeDataPort, 2);

    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    RenderVga256Screen(ega, rgba, w, h);
    EXPECT_EQ(w, 320);
    EXPECT_EQ(h, 200);
    ExpectRgb(rgba, w, 0, 0, 0, 255, 255);
}

}  // namespace
