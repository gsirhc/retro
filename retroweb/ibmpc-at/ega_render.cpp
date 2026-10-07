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

}  // namespace

void RenderTextScreen(const Ega &ega, std::vector<uint8_t> &rgba, bool blink_on, int &width, int &height) {
    constexpr int cw = 8, rows = 25;
    // Max Scan Line 0 means unconfigured (freshly reset Ega): fall back to 14 lines per row.
    int scan_lines = int(ega.crtc_max_scan_line()) + 1;
    const int ch_h = scan_lines <= 1 ? 14 : scan_lines;
    // Columns come from R01; 0 before a mode set falls back to 80.
    int cols_reg = int(ega.crtc_horizontal_display_end()) + 1;
    const int cols = cols_reg <= 1 ? 80 : cols_reg;
    const int W = cw * cols, H = ch_h * rows;
    width = W; height = H;
    rgba.assign(std::size_t(W) * std::size_t(H) * 4, 0);

    auto cell_offset = [&](int row, int col) -> uint32_t {
        return (uint32_t(ega.start_offset()) + uint32_t(row * cols + col)) & 0xFFFF;
    };
    auto text_at = [&](int row, int col, uint8_t &ch, uint8_t &attr) {
        uint32_t plane_off = cell_offset(row, col);
        ch = ega.vram[(plane_off << 2) + 0];
        attr = ega.vram[(plane_off << 2) + 1];
    };

    uint16_t cursor_off = ega.cursor_offset();
    bool cursor_visible = blink_on && !ega.cursor_disabled();

    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
            uint8_t ch, attr;
            text_at(row, col, ch, attr);
            uint8_t fg_idx = attr & 0x0F, bg_idx = uint8_t((attr >> 4) & 0x07);  // bit7 = blink, unused
            uint8_t fr, fg, fb, br, bg, bb;
            DecodeEgaColor(ega.attr_palette(fg_idx), fr, fg, fb);
            DecodeEgaColor(ega.attr_palette(bg_idx), br, bg, bb);

            bool is_cursor_cell = cursor_visible && cell_offset(row, col) == cursor_off;

            for (int r = 0; r < ch_h; ++r) {
                uint32_t glyph_plane_off = uint32_t(ch) * 32 + uint32_t(r);
                uint8_t bits = ega.vram[(glyph_plane_off << 2) + 2];  // plane 2: char generator
                bool cursor_row = is_cursor_cell &&
                    r >= ega.cursor_start_scanline() && r <= ega.cursor_end_scanline();
                for (int c = 0; c < cw; ++c) {
                    bool set = cursor_row || (bits & (0x80 >> c)) != 0;
                    int px = col * cw + c, py = row * ch_h + r;
                    std::size_t i = (std::size_t(py) * W + std::size_t(px)) * 4;
                    rgba[i + 0] = set ? fr : br;
                    rgba[i + 1] = set ? fg : bg;
                    rgba[i + 2] = set ? fb : bb;
                    rgba[i + 3] = 255;
                }
            }
        }
    }
}

void RenderCgaGraphics4Screen(const Ega &ega, std::vector<uint8_t> &rgba) {
    constexpr int W = 320, H = 200;
    rgba.assign(std::size_t(W) * std::size_t(H) * 4, 0);

    for (int y = 0; y < H; ++y) {
        uint32_t half = uint32_t(y & 1);       // real CGA: even/odd scanlines in separate 8K banks
        uint32_t row = uint32_t(y >> 1);
        for (int byte_col = 0; byte_col < 80; ++byte_col) {
            // Flat CGA byte offset; odd/even chaining splits it across planes 0/1.
            uint32_t linear_offset = (half ? 0x2000u : 0u) + row * 80u + uint32_t(byte_col);
            uint32_t plane = linear_offset & 1;
            uint32_t plane_offset = linear_offset >> 1;
            uint8_t byte = ega.vram[(plane_offset << 2) + plane];
            for (int sub = 0; sub < 4; ++sub) {
                uint8_t pixel2 = uint8_t((byte >> (6 - 2 * sub)) & 0x3);
                uint8_t r, g, b;
                DecodeEgaColor(ega.attr_palette(pixel2), r, g, b);
                int x = byte_col * 4 + sub;
                std::size_t i = (std::size_t(y) * W + std::size_t(x)) * 4;
                rgba[i + 0] = r;
                rgba[i + 1] = g;
                rgba[i + 2] = b;
                rgba[i + 3] = 255;
            }
        }
    }
}

void RenderEgaNative16Screen(const Ega &ega, std::vector<uint8_t> &rgba, int &width, int &height) {
    width = (ega.crtc_horizontal_display_end() + 1) * 8;
    height = ega.crtc_vertical_display_end() + 1;
    // Scan doubling: the CRTC counters describe the doubled raster but VRAM holds
    // one copy of each row, so render at half height.
    if (ega.crtc_scan_doubling()) height /= 2;
    if (width <= 0 || height <= 0) { width = height = 0; rgba.clear(); return; }
    rgba.assign(std::size_t(width) * std::size_t(height) * 4, 0);

    int displayed_bytes = width / 8;  // bytes/scanline drawn, 1 bit/pixel/plane
    // Stride comes from the Offset Register, not the displayed width (see
    // crtc_scanline_stride() in ega.h). An unprogrammed register reads 0 and
    // falls back to the displayed width.
    int real_stride = ega.crtc_scanline_stride();
    int row_stride = real_stride > 0 ? real_stride : displayed_bytes;
    for (int y = 0; y < height; ++y) {
        for (int byte_col = 0; byte_col < displayed_bytes; ++byte_col) {
            uint32_t plane_offset = uint32_t(y) * uint32_t(row_stride) + uint32_t(byte_col);
            uint8_t p0 = ega.vram[(plane_offset << 2) + 0];
            uint8_t p1 = ega.vram[(plane_offset << 2) + 1];
            uint8_t p2 = ega.vram[(plane_offset << 2) + 2];
            uint8_t p3 = ega.vram[(plane_offset << 2) + 3];
            for (int bit = 0; bit < 8; ++bit) {
                int shift = 7 - bit;
                uint8_t nibble = uint8_t(((p0 >> shift) & 1) | (((p1 >> shift) & 1) << 1) |
                                          (((p2 >> shift) & 1) << 2) | (((p3 >> shift) & 1) << 3));
                uint8_t r, g, b;
                DecodeEgaColor(ega.attr_palette(nibble), r, g, b);
                int x = byte_col * 8 + bit;
                std::size_t i = (std::size_t(y) * std::size_t(width) + std::size_t(x)) * 4;
                rgba[i + 0] = r;
                rgba[i + 1] = g;
                rgba[i + 2] = b;
                rgba[i + 3] = 255;
            }
        }
    }
}

ScreenMode DetectScreenMode(const Ega &ega) {
    if (!ega.graphics_mode_active()) return ScreenMode::kText;
    if (ega.gc_shift_register_mode() == 1) return ScreenMode::kCgaGraphics4;
    if (ega.gc_shift_register_mode() == 0) return ScreenMode::kEgaGraphics16;
    return ScreenMode::kUnsupportedGraphics;
}

void RenderScreen(const Ega &ega, RenderedFrame &out, bool blink_on) {
    switch (DetectScreenMode(ega)) {
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
        case ScreenMode::kUnsupportedGraphics:
            // Black placeholder, same footprint as text mode.
            out.width = kTextRenderWidth;
            out.height = kTextRenderHeight;
            out.rgba.assign(std::size_t(out.width) * std::size_t(out.height) * 4, 0);
            for (std::size_t i = 3; i < out.rgba.size(); i += 4) out.rgba[i] = 255;  // opaque
            return;
        case ScreenMode::kText:
        default:
            RenderTextScreen(ega, out.rgba, blink_on, out.width, out.height);
            return;
    }
}

}  // namespace ibmpcat
