#include "video.h"

namespace frogger {
namespace {

// Galaxian gfx: 607 at $000 is the high plane (pen bit 1), 606 at $800 the
// low plane (pen bit 0). MAME gfx_layout lists 607 first; drawgfx assigns
// that first plane the MSB. Bit 7 is the native-left pixel (MSB-first).
uint8_t plane_bit(uint8_t b, int x) { return uint8_t((b >> (7 - (x & 7))) & 1); }

// Frogger PCB: upper/lower nibbles swap entering the scroll and sprite-Y adder.
// Computer Archaeology Frogger hardware notes; MAME frogger_adjust is a
// cross-check of the wiring, not a source.
uint8_t nibble_swap(uint8_t v) { return uint8_t((v << 4) | (v >> 4)); }

}  // namespace

void Video::reset() {
    h = v = 0;
    vblank = false;
    vblank_edge = false;
}

void Video::advance(int cpu_cycles) {
    vblank_edge = false;
    // 2 pixels per main-CPU T-state (18.432/3 pixel clock, 18.432/6 CPU).
    int pix = cpu_cycles * 2;
    h += pix;
    while (h >= kHTotal) {
        h -= kHTotal;
        v++;
        if (v == kVBlankLine) {
            vblank = true;
            vblank_edge = true;
        }
        if (v >= kVTotal) {
            v = 0;
            vblank = false;
        }
    }
}

uint8_t Video::remap_color(uint8_t attr) {
    // Frogger PROM address wiring: attribute bits 2/1/0 become PROM
    // group bits 0/1/2. Cross-checked against MAME frogger_extend_tile_info.
    uint8_t c = attr & 7;
    return uint8_t(((c >> 1) & 0x03) | ((c << 2) & 0x04));
}

uint8_t Video::tile_pixel(uint8_t code, int x, int y) const {
    int row = code * 8 + (y & 7);
    uint8_t hi = gfx[unsigned(row)];            // 607
    uint8_t lo = gfx[unsigned(0x800 + row)];    // 606
    return uint8_t(plane_bit(lo, x) | (plane_bit(hi, x) << 1));
}

uint8_t Video::sprite_pixel(uint8_t code, int x, int y) const {
    // 16×16, 32 bytes/plane. y 0–7 at +0..+7, y 8–15 at +16..+23;
    // x 8–15 comes from the byte 8 further on. MAME spritelayout.
    int yy = y & 15;
    int row = (yy < 8) ? yy : (16 + (yy - 8));
    int base = (code & 0x3F) * 32 + row;
    if (x & 8) base += 8;
    uint8_t hi = gfx[unsigned(base)];
    uint8_t lo = gfx[unsigned(0x800 + base)];
    return uint8_t(plane_bit(lo, x) | (plane_bit(hi, x) << 1));
}

uint32_t Video::prom_rgb(uint8_t pen) const {
    uint8_t p = color_prom[pen & 31];
    int r = 0x21 * ((p >> 0) & 1) + 0x47 * ((p >> 1) & 1) + 0x97 * ((p >> 2) & 1);
    int g = 0x21 * ((p >> 3) & 1) + 0x47 * ((p >> 4) & 1) + 0x97 * ((p >> 5) & 1);
    // Blue bit 0 is unconnected on the Frogger board.
    int b = 0x47 * ((p >> 6) & 1) + 0x97 * ((p >> 7) & 1);
    return uint32_t((r << 16) | (g << 8) | b);
}

uint8_t Video::pixel_at(int nx, int ny) const {
    if (nx < 0 || nx >= kVisW || ny < kVisY0 || ny >= kVisY0 + kVisH) return 0;

    int col = nx / 8;
    int row = ny / 8;
    uint8_t scroll = nibble_swap(objram[unsigned((col * 2) & 0x3F)]);
    int sy = (ny + scroll) & 0xFF;
    row = sy / 8;
    int tx = nx & 7;
    int ty = sy & 7;
    if (flip_x) {
        col = 31 - col;
        tx = 7 - tx;
    }
    if (flip_y) {
        row = 31 - row;
        ty = 7 - ty;
    }
    uint8_t code = videoram[unsigned((row * 32 + col) & 0x3FF)];
    uint8_t attr = objram[unsigned((col * 2 + 1) & 0x3F)];
    uint8_t pix = tile_pixel(code, tx, ty);
    uint8_t color = remap_color(attr);
    uint8_t pen = uint8_t((color << 2) | pix);

    // Sprites on top. Eight records at $40, 4 bytes each.
    // byte0 = Y (nibble-swapped into the adder), byte1 = code/flip,
    // byte2 = colour, byte3 = X. First three sprites match V−1.
    // Cross-checked against MAME galaxian sprites_draw / frogger_adjust.
    for (int i = 7; i >= 0; i--) {
        int offs = 0x40 + i * 4;
        int spr_y = nibble_swap(objram[unsigned(offs)]);
        uint8_t bits = objram[unsigned(offs + 1)];
        uint8_t scolor = remap_color(objram[unsigned(offs + 2)]);
        int spr_x = objram[unsigned(offs + 3)] + 1;
        int sx = nx - spr_x;
        int spr_top = 240 - (spr_y - (i < 3 ? 1 : 0));
        int syy = ny - spr_top;
        if (sx < 0 || sx >= 16 || syy < 0 || syy >= 16) continue;
        if (bits & 0x40) sx = 15 - sx;
        if (bits & 0x80) syy = 15 - syy;
        uint8_t sp = sprite_pixel(uint8_t(bits & 0x3F), sx, syy);
        if (sp == 0) continue;
        pen = uint8_t((scolor << 2) | sp);
        break;
    }
    return pen;
}

void Video::render(uint32_t* out) const {
    for (int ny = 0; ny < kVisH; ny++) {
        for (int nx = 0; nx < kVisW; nx++) {
            uint8_t pen = pixel_at(nx, kVisY0 + ny);
            uint32_t rgb;
            if ((pen & 3) == 0) {
                // Transparent tile/sprite pixel: hardware river split.
                // Native x < 128 is blue; flip_x swaps the banks.
                bool river = flip_x ? (nx >= kRiverSplit) : (nx < kRiverSplit);
                rgb = river ? kRiverBlue : 0;
            } else {
                rgb = prom_rgb(pen);
            }
            // ROT90 = FLIP_X | SWAP_XY: upright((H-1)-y, x) = native(x, y).
            int ux = (kVisH - 1) - ny;
            int uy = nx;
            out[uy * kUprightW + ux] = rgb;
        }
    }
}

}  // namespace frogger
