// Renders the EGA's current screen to a packed RGBA8888 buffer. Shared by
// render_screen.cpp and the WASM canvas renderer.
//
// Supported layouts:
//   - Text mode (80x25 or 40x25, 8-pixel cells, row height from the CRTC).
//   - CGA-compatible 4-color 320x200 (GR05 Shift Register = 1), INT 10h modes 4/5.
//   - Native 16-color EGA (Shift Register = 0; modes 0x0D/0x0E/0x10). Resolution
//     comes from the CRTC Display End registers. This BIOS's INT 10h AL=0x10
//     gives Horizontal Display End 79 and Vertical Display End 349, i.e. 640x350
//     (IBM_PCAT_REVIEW.md §16).
//
// Shift Register 2 (VGA 256-color Chain-4) and unrecognized combinations
// render a black frame.
#ifndef IBMPCAT_EGA_RENDER_H
#define IBMPCAT_EGA_RENDER_H

#include <cstdint>
#include <vector>

#include "ega.h"

namespace ibmpcat {

constexpr int kTextRenderWidth = 640;
constexpr int kTextRenderHeight = 350;  // classic 14-line/row default

// Renders the text-mode screen. Glyphs come from VRAM plane 2 (character
// generator RAM, 32 bytes per character), colors from the Attribute
// Controller palette.
//
// Scan lines per row come from the CRTC Maximum Scan Line register. The VGA
// BIOS programs 16-line rows (640x400), and a hardcoded 14 clipped descenders.
// A freshly reset Ega reads 0 and falls back to 14.
//
// Columns per row come from R01 (crtc_horizontal_display_end()), since 40-column
// text (mode 0/1) is real. Same fallback to 80 when unprogrammed.
//
// `blink_on` is the cursor phase. The caller paces the blink.
void RenderTextScreen(const Ega &ega, std::vector<uint8_t> &rgba, bool blink_on, int &width, int &height);

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

enum class ScreenMode { kText, kCgaGraphics4, kEgaGraphics16, kUnsupportedGraphics };

// Picks the layout from GR06 (graphics vs alphanumeric) and the GR05 Shift Register field.
ScreenMode DetectScreenMode(const Ega &ega);

// A frame and its resolution, which differs per mode.
struct RenderedFrame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
};

// Renders the active screen into `out`, resizing as needed.
void RenderScreen(const Ega &ega, RenderedFrame &out, bool blink_on);

}  // namespace ibmpcat

#endif  // IBMPCAT_EGA_RENDER_H
