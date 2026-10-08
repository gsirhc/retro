#include "ega_render.h"

namespace ibmpcat {

namespace {

// EGA palette decode: 6 bits, 2 per channel; channel = primary*0xAA + secondary*0x55.
void DecodeEgaColor(uint8_t v, uint8_t &r, uint8_t &g, uint8_t &b) {
    auto chan = [](bool lo, bool hi) -> uint8_t { return uint8_t(lo * 0xAA + hi * 0x55); };
    b = chan(v & 0x01, v & 0x08);
    g = chan(v & 0x02, v & 0x10);
    r = chan(v & 0x04, v & 0x20);
}

// A monochrome display takes video on pin 7 and intensity on pin 6, palette bits 3 and 4
// (IBM EGA TR, Direct Drive Connector), on the 5151's green phosphor.
void DecodeMonoColor(uint8_t v, uint8_t &r, uint8_t &g, uint8_t &b) {
    r = b = 0;
    g = (v & 0x08) ? ((v & 0x10) ? 0xFF : 0xAA) : 0x00;
}

void DecodeAttrColor(const Ega &ega, uint8_t pixel, uint8_t &r, uint8_t &g, uint8_t &b) {
    if (ega.mono_display()) DecodeMonoColor(ega.attr_palette(pixel), r, g, b);
    else DecodeEgaColor(ega.attr_palette(pixel), r, g, b);
}

// CRTC address position on one raster line. Past Line Compare the counter and
// row scan restart at 0 (split screen); above it the start address and Preset
// Row Scan apply.
struct ScanPos {
    uint32_t base;
    int row;
    int row_scan;
};

ScanPos LocateRaster(const Ega &ega, int raster, int lines_per_row, uint32_t top_base) {
    int lc = ega.crtc_line_compare();
    ScanPos p;
    int line;
    if (raster > lc) {
        line = raster - lc - 1;
        p.base = 0;
    } else {
        line = raster + ega.crtc_preset_row_scan();
        p.base = top_base;
    }
    p.row = line / lines_per_row;
    p.row_scan = line % lines_per_row;
    return p;
}

// AR13: 0-7 pixels; 9-dot text runs 8, 0-7 for shifts of 0-8 (IBM EGA Technical Reference).
int PelPan(const Ega &ega, int dots_per_char) {
    int v = ega.attr_pel_pan();
    if (dots_per_char == 9) return v < 8 ? v + 1 : 0;
    return v < 8 ? v : 0;
}

// Color Plane Enable plus graphics blink (AR10 bit 3). IBM leaves the rule
// undefined; this is 86Box's (vid_svga_render.c).
uint8_t GraphicsAttr(const Ega &ega, uint8_t pixel) {
    const uint8_t pm = ega.attr_plane_enable();
    if (!ega.attr_blink_enabled()) return uint8_t(pixel & pm);
    const bool drops = !ega.char_blink_phase_on() && ((pixel & 8) || !(pm & 8));
    return uint8_t((pixel & pm & 7) | (drops ? 0 : 8));
}

void PutPixel(std::vector<uint8_t> &rgba, int width, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    std::size_t i = (std::size_t(y) * std::size_t(width) + std::size_t(x)) * 4;
    rgba[i + 0] = r;
    rgba[i + 1] = g;
    rgba[i + 2] = b;
    rgba[i + 3] = 255;
}

}  // namespace

void RenderTextScreen(const Ega &ega, std::vector<uint8_t> &rgba, int &width, int &height) {
    const int cw = ega.seq_8dot_chars() ? 8 : 9;
    const int ch_h = int(ega.crtc_max_scan_line()) + 1;
    const int cols = int(ega.crtc_horizontal_display_end()) + 1;
    const int H = int(ega.crtc_vertical_display_end()) + 1;
    const int W = cw * cols;
    width = W; height = H;
    rgba.assign(std::size_t(W) * std::size_t(H) * 4, 0);

    const int stride = ega.crtc_scanline_stride();
    const uint32_t top_base = ega.start_offset();
    const int pan = PelPan(ega, cw);
    const bool line_graphics = ega.attr_line_graphics();
    const bool blink_enabled = ega.attr_blink_enabled();
    const bool chars_on = ega.char_blink_phase_on();
    const bool mono = ega.attr_mono();
    const uint8_t plane_enable = ega.attr_plane_enable();
    // Font map n is the first 8KB of plane 2 bank n, 16KB apart (IBM EGA Technical Reference, Character Map Select).
    const uint32_t map_a = uint32_t(ega.seq_char_map_a()) * 0x4000;
    const uint32_t map_b = uint32_t(ega.seq_char_map_b()) * 0x4000;

    const uint16_t cursor_off = uint16_t(ega.cursor_offset() + ega.cursor_skew());
    const int underline_row = ega.crtc_underline_row();
    const bool cursor_visible = ega.cursor_blink_phase_on();

    for (int y = 0; y < H; ++y) {
        ScanPos pos = LocateRaster(ega, y, ch_h, top_base);
        const uint32_t row_base = pos.base + uint32_t(pos.row * stride);
        const bool cursor_row = cursor_visible && ega.cursor_on_row(pos.row_scan);
        int cached_col = -1;
        uint8_t ch = 0, bits = 0;
        bool is_cursor = false, hidden = false;
        uint8_t fr = 0, fg = 0, fb = 0, br = 0, bg = 0, bb = 0;
        for (int x = 0; x < W; ++x) {
            const int sx = x + pan;
            const int col = sx / cw, c = sx % cw;
            if (col != cached_col) {
                cached_col = col;
                const uint32_t ma = (row_base + uint32_t(col)) & 0xFFFF;
                const uint32_t cell = ega.display_address(ma, pos.row_scan);
                ch = ega.vram[(cell << 2) + 0];
                const uint8_t attr = ega.vram[(cell << 2) + 1];
                const uint32_t map = (attr & 0x08) ? map_a : map_b;
                bits = ega.vram[((map + uint32_t(ch) * 32 + uint32_t(pos.row_scan)) << 2) + 2];
                // Monochrome attributes underline foreground 1 on the Underline Location row (86Box vid_ega_render.c).
                if (mono && (attr & 0x07) == 0x01 && pos.row_scan == underline_row) bits = 0xFF;
                uint8_t bg_idx = blink_enabled ? uint8_t((attr >> 4) & 0x07) : uint8_t(attr >> 4);
                hidden = blink_enabled && (attr & 0x80) && !chars_on;
                is_cursor = cursor_row && ma == cursor_off;
                DecodeAttrColor(ega, uint8_t(attr & 0x0F & plane_enable), fr, fg, fb);
                DecodeAttrColor(ega, uint8_t(bg_idx & plane_enable), br, bg, bb);
            }
            bool set;
            if (c < 8) set = (bits & (0x80 >> c)) != 0;
            else set = line_graphics && ch >= 0xC0 && ch <= 0xDF && (bits & 0x01);
            if (hidden) set = false;
            if (is_cursor) set = true;
            if (set) PutPixel(rgba, W, x, y, fr, fg, fb);
            else PutPixel(rgba, W, x, y, br, bg, bb);
        }
    }
}

void RenderCgaGraphics4Screen(const Ega &ega, std::vector<uint8_t> &rgba) {
    constexpr int W = 320, H = 200;
    rgba.assign(std::size_t(W) * std::size_t(H) * 4, 0);

    const int lines_per_row = int(ega.crtc_max_scan_line()) + 1;
    const uint32_t stride = uint32_t(ega.crtc_scanline_stride());
    const uint32_t top_base = ega.start_offset();
    const int pan = PelPan(ega, 8);

    for (int y = 0; y < H; ++y) {
        ScanPos pos = LocateRaster(ega, y, lines_per_row, top_base);
        const uint32_t row_base = pos.base + uint32_t(pos.row) * stride;
        for (int x = 0; x < W; ++x) {
            const int sx = x + pan;
            // One character clock is 8 pels: 4 from plane 0 then 4 from plane 1, planes 2/3 the high bits (86Box vid_ega_render.c).
            const uint32_t ma = (row_base + uint32_t(sx >> 3)) & 0xFFFF;
            const uint8_t* p = &ega.vram[ega.display_address(ma, pos.row_scan) << 2];
            const int half = (sx >> 2) & 1, shift = 6 - 2 * (sx & 3);
            const uint8_t pixel = uint8_t(((p[half] >> shift) & 3) | (((p[2 + half] >> shift) & 3) << 2));
            uint8_t r, g, b;
            DecodeAttrColor(ega, GraphicsAttr(ega, pixel), r, g, b);
            PutPixel(rgba, W, x, y, r, g, b);
        }
    }
}

void RenderEgaNative16Screen(const Ega &ega, std::vector<uint8_t> &rgba, int &width, int &height) {
    width = (ega.crtc_horizontal_display_end() + 1) * 8;
    height = ega.crtc_vertical_display_end() + 1;
    const int row_stride = ega.crtc_scanline_stride();
    if (width <= 0 || height <= 0) { width = height = 0; rgba.clear(); return; }
    rgba.assign(std::size_t(width) * std::size_t(height) * 4, 0);

    const uint32_t top_base = ega.start_offset();
    const int lines_per_row = int(ega.crtc_max_scan_line()) + 1;
    const int pan = PelPan(ega, 8);
    for (int y = 0; y < height; ++y) {
        ScanPos pos = LocateRaster(ega, y, lines_per_row, top_base);
        const uint32_t row_base = pos.base + uint32_t(pos.row * row_stride);
        for (int x = 0; x < width; ++x) {
            const int sx = x + pan;
            const uint32_t plane_offset = ega.display_address((row_base + uint32_t(sx >> 3)) & 0xFFFF, pos.row_scan);
            const int shift = 7 - (sx & 7);
            const uint8_t* p = &ega.vram[plane_offset << 2];
            uint8_t nibble = uint8_t(((p[0] >> shift) & 1) | (((p[1] >> shift) & 1) << 1) |
                                      (((p[2] >> shift) & 1) << 2) | (((p[3] >> shift) & 1) << 3));
            uint8_t r, g, b;
            DecodeAttrColor(ega, GraphicsAttr(ega, nibble), r, g, b);
            PutPixel(rgba, width, x, y, r, g, b);
        }
    }
}

ScreenMode DetectScreenMode(const Ega &ega) {
    if (!ega.graphics_mode_active()) return ScreenMode::kText;
    if (ega.gc_cga_shift()) return ScreenMode::kCgaGraphics4;
    return ScreenMode::kEgaGraphics16;
}

void RenderScreen(const Ega &ega, RenderedFrame &out) {
    // CRTC 17h bit 7 clear holds the CRTC in reset with no syncs (IBM EGA Technical Reference).
    ScreenMode mode = ega.crtc_running() ? DetectScreenMode(ega) : ScreenMode::kBlank;
    switch (mode) {
        case ScreenMode::kCgaGraphics4:
            out.width = 320;
            out.height = 200;
            RenderCgaGraphics4Screen(ega, out.rgba);
            return;
        case ScreenMode::kEgaGraphics16:
            RenderEgaNative16Screen(ega, out.rgba, out.width, out.height);
            if (out.width > 0 && out.height > 0) return;
            // CRTC not yet at a sane resolution (mid mode-set): fall through to black.
            [[fallthrough]];
        case ScreenMode::kBlank:
            out.width = kTextRenderWidth;
            out.height = kTextRenderHeight;
            out.rgba.assign(std::size_t(out.width) * std::size_t(out.height) * 4, 0);
            for (std::size_t i = 3; i < out.rgba.size(); i += 4) out.rgba[i] = 255;  // opaque
            return;
        case ScreenMode::kText:
        default:
            RenderTextScreen(ega, out.rgba, out.width, out.height);
            return;
    }
}

}  // namespace ibmpcat
