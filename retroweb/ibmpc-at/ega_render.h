// Renders the EGA's current text-mode screen to a packed RGBA8888 buffer
// -- one canonical implementation shared between render_screen.cpp (the
// native BMP diagnostic) and the WASM front end's canvas renderer, so
// there's a single, tested source of truth for "what does this screen
// actually look like" rather than the same decode logic duplicated in
// C++ and JS.
//
// Text mode only (80x25, 8x14 cells -> 640x350) -- matches the real EGA
// 80x25 16-color text mode this machine actually boots to, and the only
// mode a real BIOS's own POST/boot messages and a plain DOS prompt ever
// use. CRTC-timing-driven graphics-mode scanout (deriving the active
// resolution from the Horizontal/Vertical Display End registers, for the
// 320x200x16/640x350x16/etc. graphics modes Phase 5's memory engine
// already fully supports) is deliberately out of scope for this pass --
// see IBM_PCAT_REVIEW.md §14.
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

}  // namespace ibmpcat

#endif  // IBMPCAT_EGA_RENDER_H
