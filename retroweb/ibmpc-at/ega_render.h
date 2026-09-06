// Renders the EGA's current screen to a packed RGBA8888 buffer -- one
// canonical implementation shared between render_screen.cpp (the native
// BMP diagnostic) and the WASM front end's canvas renderer, so there's a
// single, tested source of truth for "what does this screen actually look
// like" rather than the same decode logic duplicated in C++ and JS.
//
// Two real hardware layouts are supported so far:
//   - Text mode (80x25, 8x14 cells -> 640x350): the mode a real BIOS's own
//     POST/boot messages and a plain DOS prompt use. See RenderTextScreen.
//   - The EGA/VGA "CGA-compatibility" 4-color 320x200 graphics mode (GR05
//     Shift Register field = 1): what INT 10h mode 4/5 programs, and
//     genuinely common -- any DOS program written for plain CGA graphics
//     (no EGA/VGA-specific code) runs in exactly this mode on real EGA
//     hardware unmodified. See RenderScreen/DetectScreenMode.
//
// Native 16-color EGA graphics (320x200x16, 640x350x16 -- Shift Register
// field = 0) is NOT yet implemented: unlike the CGA-compatible mode above,
// deriving its actual resolution needs the CRTC's own Horizontal/Vertical
// Display End timing registers, not just a register field to branch on.
// An unrecognized graphics mode renders as a plain black frame rather than
// misinterpreting graphics VRAM as text glyphs (the bug this replaced --
// see IBM_PCAT_REVIEW.md's account of the actual garbled-block symptom
// that led here). Genuine EGA hardware never uses Shift Register value 2
// (a VGA-only variant), so it's grouped with "unrecognized" too.
#ifndef IBMPCAT_EGA_RENDER_H
#define IBMPCAT_EGA_RENDER_H

#include <cstdint>
#include <vector>

#include "ega.h"

namespace ibmpcat {

constexpr int kTextRenderWidth = 640;
constexpr int kTextRenderHeight = 350;

// Fills `rgba` (resized as needed) with kTextRenderWidth*kTextRenderHeight
// RGBA8888 pixels (row-major, top to bottom) reflecting the Ega's current
// text-mode screen: glyph bitmaps read straight from VRAM plane 2 (the
// character generator RAM, exactly where a real vgabios's mode-set writes
// it, at the standard EGA/VGA convention of 32 bytes reserved per
// character) and colors from the live Attribute Controller palette
// registers, decoded via the genuine EGA 6-bit color format -- no
// hardcoded font or color table. `blink_on` selects whether a
// non-disabled text cursor is currently drawn as a solid block at its
// real CRTC-programmed scanlines; the caller paces the actual blink rate
// (this function just draws the requested phase, matching how a real CRT
// controller has no opinion of its own about blink timing -- that's a
// separate counter in the CRTC feeding this same enable bit).
void RenderTextScreen(const Ega &ega, std::vector<uint8_t> &rgba, bool blink_on);

// Fills `rgba` with 320*200 RGBA8888 pixels reflecting the EGA/VGA CGA-
// compatibility 4-color graphics mode's current screen. Real hardware
// fact this decodes: a CGA-unaware program writes what it thinks is one
// flat 8000-byte CGA bank (even scanlines in the first half, odd
// scanlines in the second, 80 bytes/scanline, 4 pixels/byte) -- odd/even
// plane chaining (already implemented for Phase 5's memory engine) splits
// those writes across planes 0 and 1 by address parity, so each pair of
// consecutive CGA bytes lands at the SAME plane offset, one in each
// plane. The real CRT controller's shift registers then read plane 0's
// byte as pixels 0-3 of that pair and plane 1's byte as pixels 4-7, each
// 2-bit value indexing the live Attribute Controller palette (registers
// 0-3) exactly like text mode's colors -- not a fixed CGA palette table.
void RenderCgaGraphics4Screen(const Ega &ega, std::vector<uint8_t> &rgba);

enum class ScreenMode { kText, kCgaGraphics4, kUnsupportedGraphics };

// Examines the Graphics Controller's Miscellaneous register (graphics vs.
// alphanumeric) and Mode register (Shift Register field) -- the same
// registers a real EGA's own CRT controller consults -- to decide which
// screen layout is currently active. Not a BIOS video-mode-number guess.
ScreenMode DetectScreenMode(const Ega &ega);

// One packed frame plus the resolution it's actually at -- text and the
// CGA-compatible graphics mode render at different real resolutions, so a
// caller (a canvas, a BMP writer) needs both together, not an assumed
// fixed size.
struct RenderedFrame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
};

// Renders whatever screen is currently active into `out`, resizing it as
// needed -- the one entry point render_screen.cpp and the WASM front end
// both call, so a real, tested single implementation decides what's on
// screen rather than each caller guessing.
void RenderScreen(const Ega &ega, RenderedFrame &out, bool blink_on);

}  // namespace ibmpcat

#endif  // IBMPCAT_EGA_RENDER_H
