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
}

void Video::advance(int cpu_cycles) {
    vblank_edge = false;
    int pixels = cpu_cycles * 2;
    h += pixels;
    while (h >= kHTotal) {
        h -= kHTotal;
        v++;
        if (v >= kVTotal) v = 0;
        bool vb = v >= kVBlankLine;
        if (vb && !vblank) vblank_edge = true;
        vblank = vb;
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

void Video::render(uint32_t* out) const {
    uint32_t native[kVisW * kVisH];
    for (int row = 0; row < 28; row++) {
        for (int col = 0; col < 36; col++) {
            int offs = vram_offset(col, row) & 0x3FF;
            uint8_t code = videoram[unsigned(offs)];
            uint8_t attr = colorram[unsigned(offs)];
            if (attr & 0x20) code = uint8_t(code | 0x100);  // bank bit unused in 4K 5E
            for (int ty = 0; ty < 8; ty++) {
                for (int tx = 0; tx < 8; tx++) {
                    int x = col * 8 + tx;
                    int y = row * 8 + ty;
                    if (x >= kVisW || y >= kVisH) continue;
                    uint8_t pix = tile_pixel(uint8_t(code), tx, ty);
                    native[y * kVisW + x] = lookup_rgb(attr, pix);
                }
            }
        }
    }

    // Sprites: 8 objects in unrotated space. Pen 0 is always transparent, plus
    // any pen with the same RGB as pen 0 (see transpen_mask below).
    // ram2[i] feeds sy = ram2[i] - 31, ram2[i+1] feeds sx = 272 - ram2[i+1];
    // cocktail mode swaps to sx = ram2[i+1], sy = 240 - ram2[i] (MAME draw_sprites).
    for (int i = 7; i >= 0; i--) {
        uint8_t b0 = spriteram[unsigned(i * 2)];
        uint8_t b1 = spriteram[unsigned(i * 2 + 1)];
        uint8_t code = uint8_t(b0 >> 2);
        // Bit 0 = X flip, bit 1 = Y flip (MAME pacman_v.cpp), applied in native
        // space before the ROT90 swaps the axes.
        bool flipx = (b0 & 1) != 0;
        bool flipy = (b0 & 2) != 0;
        uint8_t color = uint8_t(b1 & 0x1F);
        // MAME draw_sprites uses transpen_mask(gfx, color, 0): any pen whose
        // RGB matches pen 0 is transparent. The ROM hides Pac-Man during the
        // ghost-eaten pause with an all-black color group.
        uint32_t transparent_rgb = lookup_rgb(color, 0);
        bool pen_transparent[4];
        for (int p = 0; p < 4; p++) pen_transparent[p] = lookup_rgb(color, uint8_t(p)) == transparent_rgb;
        int sx, sy;
        if (flip_screen) {
            sx = int(sprite_xy[unsigned(i * 2 + 1)]);
            sy = 240 - int(sprite_xy[unsigned(i * 2)]);
            flipx = !flipx;
            flipy = !flipy;
        } else {
            sx = 272 - int(sprite_xy[unsigned(i * 2 + 1)]);
            sy = int(sprite_xy[unsigned(i * 2)]) - 31;
        }
        for (int py = 0; py < 16; py++) {
            for (int px = 0; px < 16; px++) {
                int x = sx + (flipx ? 15 - px : px);
                int y = sy + (flipy ? 15 - py : py);
                if (x < 0 || y < 0 || x >= kVisW || y >= kVisH) continue;
                uint8_t pix = sprite_pixel(code, px, py);
                if (pen_transparent[pix]) continue;
                native[y * kVisW + x] = lookup_rgb(color, pix);
            }
        }
    }

    // MAME ROT90 = FLIP_X | SWAP_XY: dst(x = (H-1) - native_y, y = native_x).
    for (int ny = 0; ny < kVisH; ny++) {
        for (int nx = 0; nx < kVisW; nx++) {
            int dx = (kVisH - 1) - ny;
            int dy = nx;
            out[dy * kUprightW + dx] = native[ny * kVisW + nx];
        }
    }
}

}  // namespace pacman
