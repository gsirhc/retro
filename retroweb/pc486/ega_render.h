// Renders the current screen to packed RGBA8888. One implementation shared by
// render_screen.cpp (native BMP) and the WASM canvas renderer.
// Modes: text, CGA-compatible 4-color (GR05 shift = 1), native 16-color EGA
// (shift = 0), VGA 256-color (shift = 2). Anything else renders black.
#ifndef PC486_EGA_RENDER_H
#define PC486_EGA_RENDER_H

#include <cstdint>
#include <vector>

#include "ega.h"

namespace pc486 {

constexpr int kTextRenderWidth = 640;
constexpr int kTextRenderHeight = 350;  // the classic 14-line/row default -- see RenderTextScreen

// Renders the text-mode screen. Glyphs come from VRAM plane 2 (32 bytes per
// character), colors from the Attribute Controller palette via the DAC.
// Rows come from Vertical Display End over Maximum Scan Line (R09), columns
// from Horizontal Display End (40 and 80 both occur). A freshly reset Ega reads
// Max Scan Line 0, which falls back to 14 lines. Cells are 9 dots unless SR01 bit 0 selects 8, with the
// line-graphics column for C0h-DFh. Attribute bit 7 blinks or brightens per
// AR10 bit 3, bit 3 picks font map A or B (SR03). Cursor and blink follow
// Ega::frame_count. Start address, panning, Preset Row Scan and Line Compare
// apply as in the planar modes.
void RenderTextScreen(const Ega &ega, std::vector<uint8_t> &rgba, int &width, int &height);

// Renders the CGA-compatible 4-color mode (320x200). Odd/even chaining splits
// the CGA byte stream across planes 0 and 1; each pair of bytes holds 8
// pixels, 2 bits each, indexing attribute palette registers 0-3.
void RenderCgaGraphics4Screen(const Ega &ega, std::vector<uint8_t> &rgba);

// Renders native 16-color EGA graphics. Resolution comes from the CRTC Display
// End registers, except in 4bpp DISPI modes where the extension registers give
// it (the 9-bit VDE cannot express 600/768 lines, PC486_REVIEW.md §7.5.1).
// Each pixel combines one bit from each plane (plane 0 = LSB) into an index
// into the attribute palette. Scanline stride comes from the Offset Register
// (R13), not the displayed width.
void RenderEgaNative16Screen(const Ega &ega, std::vector<uint8_t> &rgba, int &width, int &height);

// Renders VGA 256-color graphics (mode 13h).
//  - Chain-4 makes the flat byte-per-pixel buffer the planar VRAM read through.
//  - Width: CRTC timing is 640 dots; AR10 bit 6 halves the pixel clock to 320.
//  - Height: Maximum Scan Line = 1 doubles each row.
//  - Stride: Offset 40 is in doublewords here, 320 bytes per line.
//  - Color: pixel ANDed with the PEL Mask, then the DAC (6 bits scaled to 8).
//    The attribute palette is bypassed.
void RenderVga256Screen(const Ega &ega, std::vector<uint8_t> &rgba, int &width, int &height);

enum class ScreenMode { kText, kCgaGraphics4, kEgaGraphics16, kVga256, kUnsupportedGraphics };

// Decides the active layout from GR06 (graphics vs alphanumeric) and GR05
// (Shift Register field).
ScreenMode DetectScreenMode(const Ega &ega);

// A frame plus its resolution, which varies by mode.
struct RenderedFrame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
};

// Renders whatever screen is active into `out`.
void RenderScreen(const Ega &ega, RenderedFrame &out);

}  // namespace pc486

#endif  // PC486_EGA_RENDER_H
