#include "video.h"

namespace pacman {

void Video::reset() {
    videoram.fill(0);
    colorram.fill(0);
    spriteram.fill(0);
    sprite_xy.fill(0);
    flip_screen = false;
    h = v = 0;
    vblank = false;
    vblank_edge = false;
    sprite_line_.fill(0);
}

void Video::advance(int cpu_cycles) {
    int pixels = cpu_cycles * 2;
    while (pixels > 0) {
        if (h < kVisW) {
            int end = h + pixels < kVisW ? h + pixels : kVisW;
            if (v < kVisH) paint(v, h, end);
            pixels -= end - h;
            h = end;
            if (h == kVisW) hblank_setup(v + 1 == kVTotal ? 0 : v + 1);
            continue;
        }
        int end = h + pixels < kHTotal ? h + pixels : kHTotal;
        pixels -= end - h;
        h = end;
        if (h < kHTotal) continue;
        h = 0;
        if (++v == kVTotal) v = 0;
        if (v == kVBlankLine) {
            vblank = true;
            vblank_edge = true;
        } else if (v == 0) {
            vblank = false;
        }
    }
}

// ROM packs 4 pixels per byte: high nibble = plane 0 (pixel low bit), low
// nibble = plane 1 (pixel high bit). Layout from MAME pacman.cpp tilelayout/spritelayout.
uint8_t Video::tile_pixel(uint8_t code, int x, int y) const {
    const uint8_t* t = &tile_rom[unsigned(code) * 16];
    uint8_t byte = (x < 4) ? t[y + 8] : t[y];
    int xb = x & 3;
    return uint8_t((((byte >> (7 - xb)) & 1) << 1) | ((byte >> (3 - xb)) & 1));
}

uint8_t Video::sprite_pixel(uint8_t code, int x, int y) const {
    // Same nibble packing in 4-pixel column slices; y 0-7 use bytes 0-7, y 8-15 bytes 32-39.
    static constexpr int kColByte[4] = {8, 16, 24, 0};
    int base_y = (y < 8) ? y : (32 + (y - 8));
    const uint8_t* t = &sprite_rom[unsigned(code) * 64];
    uint8_t byte = t[kColByte[x >> 2] + base_y];
    int xb = x & 3;
    return uint8_t((((byte >> (7 - xb)) & 1) << 1) | ((byte >> (3 - xb)) & 1));
}

uint32_t Video::lookup_rgb(uint8_t attr, uint8_t pix) const {
    uint8_t idx = lookup_prom[unsigned(((attr & 0x3F) << 2) | (pix & 3)) & 0xFF];
    uint8_t v = color_prom[idx & 0x1F];
    int r = ((v & 1) ? 0x21 : 0) + ((v & 2) ? 0x47 : 0) + ((v & 4) ? 0x97 : 0);
    int g = ((v & 8) ? 0x21 : 0) + ((v & 16) ? 0x47 : 0) + ((v & 32) ? 0x97 : 0);
    int b = ((v & 64) ? 0x51 : 0) + ((v & 128) ? 0xAE : 0);
    return uint32_t((r << 16) | (g << 8) | b);
}

int Video::vram_offset(int col, int row) const {
    // 36x28 tilemap, MAME pacman_scan_rows.
    int c = col - 2;
    int r = row + 2;
    if (c & 0x20)
        return r + ((c & 0x1F) << 5);
    return c + (r << 5);
}

void Video::hblank_setup(int line) {
    sprite_line_.fill(0);
    if (line >= kVisH) return;
    // Lower slots win, as MAME draws 7..0 last-on-top.
    for (int i = 0; i < 8; i++) {
        uint8_t b0 = spriteram[unsigned(i * 2)];
        uint8_t color = uint8_t(spriteram[unsigned(i * 2 + 1)] & 0x1F);
        int sx = 272 - int(sprite_xy[unsigned(i * 2 + 1)]);
        // Slots 0-2 land a line later, like the Galaxian line buffer (MAME m_xoffsethack).
        int sy = int(sprite_xy[unsigned(i * 2)]) - 31 + (i < 3 ? 1 : 0);
        int row = line - sy;
        if (row < 0 || row >= 16) continue;
        bool fx = (b0 & 1) != 0;
        bool fy = (b0 & 2) != 0;
        uint8_t code = uint8_t(b0 >> 2);
        for (int wrap = 0; wrap < 2; wrap++) {
            int base = sx - wrap * 256;
            for (int px = 0; px < 16; px++) {
                int x = base + (fx ? 15 - px : px);
                // Sprites never reach the two score columns at each end (MAME spriteclip).
                if (x < 16 || x > 271 || sprite_line_[unsigned(x)]) continue;
                uint8_t pix = sprite_pixel(code, px, fy ? 15 - row : row);
                // A pen is clear when its lookup entry selects colour 0 (MAME transpen_mask).
                if ((lookup_prom[unsigned((color << 2) | pix)] & 0x0F) == 0) continue;
                sprite_line_[unsigned(x)] = lookup_rgb(color, pix) | (1u << 24);
            }
        }
    }
}

void Video::paint(int line, int x0, int x1) {
    uint32_t* row = &frame_[unsigned(line * kVisW)];
    for (int x = x0; x < x1; x++) {
        if (uint32_t s = sprite_line_[unsigned(x)]) {
            row[x] = s & 0xFFFFFF;
            continue;
        }
        // Flip inverts both tilemap counters; sprites are positioned by the game.
        int tx = flip_screen ? kVisW - 1 - x : x;
        int ty = flip_screen ? kVisH - 1 - line : line;
        int offs = vram_offset(tx >> 3, ty >> 3) & 0x3FF;
        uint8_t attr = colorram[unsigned(offs)];
        uint8_t pix = tile_pixel(videoram[unsigned(offs)], tx & 7, ty & 7);
        row[x] = lookup_rgb(attr, pix);
    }
}

void Video::render(uint32_t* out) const {
    // MAME ROT90 = FLIP_X | SWAP_XY: dst(x = (H-1) - native_y, y = native_x).
    for (int ny = 0; ny < kVisH; ny++) {
        for (int nx = 0; nx < kVisW; nx++) {
            out[nx * kUprightW + (kVisH - 1) - ny] = frame_[unsigned(ny * kVisW + nx)];
        }
    }
}

}  // namespace pacman
