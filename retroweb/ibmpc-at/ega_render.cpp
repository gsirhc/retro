#include "ega_render.h"

namespace ibmpcat {

namespace {

// Real EGA palette-register decode: 6 significant bits, 2 per channel
// (primary + secondary/"intensity" bit), each channel = primary*0xAA +
// secondary*0x55 -- the genuine hardware DAC-independent 64-color EGA
// scheme (not VGA's programmable DAC). See ega.h's file header.
void DecodeEgaColor(uint8_t v, uint8_t &r, uint8_t &g, uint8_t &b) {
    auto chan = [](bool lo, bool hi) -> uint8_t { return uint8_t(lo * 0xAA + hi * 0x55); };
    b = chan(v & 0x01, v & 0x08);
    g = chan(v & 0x02, v & 0x10);
    r = chan(v & 0x04, v & 0x20);
}

}  // namespace

void RenderTextScreen(const Ega &ega, std::vector<uint8_t> &rgba, bool blink_on) {
    constexpr int cw = 8, ch_h = 14, cols = 80, rows = 25;
    constexpr int W = kTextRenderWidth, H = kTextRenderHeight;
    rgba.assign(std::size_t(W) * std::size_t(H) * 4, 0);

    auto cell_offset = [&](int row, int col) -> uint32_t {
        return (uint32_t(ega.start_offset()) + uint32_t(row * 80 + col)) & 0xFFFF;
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
            uint8_t fg_idx = attr & 0x0F, bg_idx = uint8_t((attr >> 4) & 0x07);  // bit7 = blink, unused here
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

}  // namespace ibmpcat
