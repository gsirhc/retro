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

// Real pacman.5e/5f ROMs pack 4 pixels per byte, not 8 rows of 1bpp per
// plane: high nibble = plane 0 (pixel's low bit), low nibble = plane 1
// (pixel's high bit), one byte per 4-pixel-wide column slice. Ported from
// MAME's `tilelayout`/`spritelayout` gfx_layout (src/mame/pacman/pacman.cpp,
// pengo.cpp): planeoffset {0,4}, xoffset/yoffset resolve to this same
// byte-and-nibble addressing. The byte/nibble *position* was verified by
// decoding the "NAMCO" tiles ($28-$2C, named in that same source) and the
// Pac-Man/ghost sprites into recognizable glyphs -- the naive row-major
// layout a typical PROM dump might suggest instead decodes to unrecognizable
// noise. The plane-to-bit assignment below (which nibble is pen bit 0 vs.
// bit 1) was separately verified against the real ROM's color/lookup PROMs:
// the maze dot tile's "on" pixel resolves to a lookup index that is only a
// valid (non-black) color for pen 1, not pen 2 -- the reverse assignment
// silently rendered every dot invisible while leaving monochrome glyphs
// (which set both planes identically) looking fine.
uint8_t Video::tile_pixel(uint8_t code, int x, int y) const {
    const uint8_t* t = &tile_rom[unsigned(code) * 16];
    uint8_t byte = (x < 4) ? t[y + 8] : t[y];
    int xb = x & 3;
    return uint8_t((((byte >> (7 - xb)) & 1) << 1) | ((byte >> (3 - xb)) & 1));
}

uint8_t Video::sprite_pixel(uint8_t code, int x, int y) const {
    // 16×16 sprite: same nibble packing, in 4-pixel-wide column slices;
    // y 0-7 use bytes 0-7 (per column slice), y 8-15 use bytes 32-39.
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
    // 36×28 tilemap. col 0–35, row 0–27 in unrotated space.
    // MAME pacman_scan_rows (cross-check of the schematic decode):
    //   row += 2; col -= 2; if (col & 0x20) offs = row + ((col & 0x1f) << 5)
    //   else offs = col + (row << 5);
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

    // Sprites: 8 objects, drawn in unrotated space. Pen 0 is always
    // transparent, plus any other pen that resolves to the same RGB as pen 0
    // for this sprite's color (see the per-sprite transpen_mask comment
    // below) -- not a fixed "pen index 0 only" rule.
    // Coordinate transform: Midway sprite shifters; MAME draw_sprites is the
    // cross-check -- ram2[i] (the $5060/$5062/... register) feeds *sy*, and
    // ram2[i+1] (the $5061/$5063/... register) feeds *sx*: sy = ram2[i] - 31,
    // sx = 272 - ram2[i+1] before flipscreen (cocktail mode swaps to
    // sx = ram2[i+1], sy = 240 - ram2[i]). Getting this backwards still
    // produces sprites, just at the wrong place on screen -- e.g. bunched up
    // against the ghost house instead of at each character's real start spot.
    for (int i = 7; i >= 0; i--) {
        uint8_t b0 = spriteram[unsigned(i * 2)];
        uint8_t b1 = spriteram[unsigned(i * 2 + 1)];
        uint8_t code = uint8_t(b0 >> 2);
        // Bit 0 = X flip, bit 1 = Y flip -- MAME's own `fx = spriteram[offs]
        // & 1` / `fy = spriteram[offs] & 2` (src/mame/pacman/pacman_v.cpp).
        // Both bits are applied in *native* (pre-rotation, landscape) space,
        // before the ROT90 (FLIP_X | SWAP_XY) transform below swaps the axes:
        // native Y-flip ends up moving the sprite along the upright screen's
        // horizontal axis (dx depends on native y) and native X-flip along
        // its vertical axis (dy = native x). Verified against real gameplay
        // in all four joystick directions plus the ghosts (whose bits are
        // always 0 -- any wrong pairing here either mirrors Pac-Man's mouth
        // away from his direction of travel, or leaves the ghosts upside
        // down, depending on which bit is misassigned).
        bool flipx = (b0 & 1) != 0;
        bool flipy = (b0 & 2) != 0;
        uint8_t color = uint8_t(b1 & 0x1F);
        // Real hardware's transparency isn't "pen index 0 is transparent" --
        // MAME's draw_sprites resolves this per color group via
        // device_palette_interface::transpen_mask(gfx, color, 0), which
        // marks *any* pen whose looked-up RGB matches pen 0's RGB (for this
        // color) as transparent too, not just pen 0 itself. Most colors
        // never trigger this (their pens are all distinct), but the ROM
        // relies on it to hide Pac-Man during the "you ate a ghost" pause:
        // it points his sprite at a color group whose whole row maps to
        // black, making every one of his (otherwise opaque) circle pixels
        // vanish instead of painting an opaque black silhouette over the
        // score digits it's drawn on top of. Skipping only literal pen 0
        // leaves that silhouette blotting out part of the score.
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

    // MAME's GAME() macro tags every pacman.cpp driver ROT90, defined as
    // FLIP_X | SWAP_XY (src/emu/gamedrv.h): swap the axes, then mirror the
    // result's X. dst(x = (H-1) - native_y, y = native_x) = native(x, y).
    for (int ny = 0; ny < kVisH; ny++) {
        for (int nx = 0; nx < kVisW; nx++) {
            int dx = (kVisH - 1) - ny;
            int dy = nx;
            out[dy * kUprightW + dx] = native[ny * kVisW + nx];
        }
    }
}

}  // namespace pacman
