// Renders the EGA's current screen to packed RGBA8888. Shared by
// render_screen.cpp and the WASM canvas renderer.
// Modes: text, CGA-compatible 4-color (GR05 shift = 1), native planar
// (shift = 0). Shift 2 and a CRTC held in reset render black.
#ifndef IBMPCAT_EGA_RENDER_H
#define IBMPCAT_EGA_RENDER_H

#include <cstdint>
#include <vector>

#include "ega.h"

namespace ibmpcat {

constexpr int kTextRenderWidth = 640;
constexpr int kTextRenderHeight = 350;  // classic 14-line/row default

// Renders the text-mode screen. Glyphs come from VRAM plane 2 (32 bytes per
// character), colors from the Attribute Controller palette. Rows come from
// Vertical Display End over Maximum Scan Line, columns from Horizontal Display
// End. Cells are 9 dots unless SR01 bit 0 selects 8. Start address, pel
// panning, Preset Row Scan and Line Compare apply as in the planar modes.
void RenderTextScreen(const Ega &ega, std::vector<uint8_t> &rgba, int &width, int &height);

// Fills `rgba` with 320*200 pixels of the CGA-compatible 4-color mode. A CGA
// program writes one flat bank (even scanlines first half, odd second, 80
// bytes/scanline). Odd/even chaining splits it across planes 0 and 1 so each
// consecutive byte pair shares a plane offset. Plane 0 supplies pixels 0-3 and
// plane 1 pixels 4-7, 2 bits each, indexing attribute palette registers 0-3.
void RenderCgaGraphics4Screen(const Ega &ega, std::vector<uint8_t> &rgba);

// Fills `rgba`/`width`/`height` with the native 16-color screen. Resolution
// comes from the CRTC Display End registers. With odd/even chaining off, each
// byte holds one bit of 8 pixels; plane 0 is the LSB of the color index and
// plane 3 the MSB, indexing attribute palette registers 0-15.
//
// The scanline stride comes from the CRTC Offset Register (R13), not the
// displayed width, since software can program a wider logical scanline.
void RenderEgaNative16Screen(const Ega &ega, std::vector<uint8_t> &rgba, int &width, int &height);

enum class ScreenMode { kText, kCgaGraphics4, kEgaGraphics16, kBlank };

// Picks the layout from GR06 (graphics vs alphanumeric) and GR05 bit 5.
ScreenMode DetectScreenMode(const Ega &ega);

// A frame and its resolution, which differs per mode.
struct RenderedFrame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
};

// Renders the active screen into `out`, resizing as needed.
void RenderScreen(const Ega &ega, RenderedFrame &out);

}  // namespace ibmpcat

#endif  // IBMPCAT_EGA_RENDER_H
