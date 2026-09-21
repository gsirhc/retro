#include "video.h"

#include <array>

namespace galaxian {
namespace {

constexpr int kStarPeriod = (1 << 17) - 1;

// Galaxian gfx: first chip at $000 is the high plane (pen bit 1), second at
// $800 the low plane (pen bit 0). Bit 7 is the native-left pixel (MSB-first).
uint8_t plane_bit(uint8_t b, int x) { return uint8_t((b >> (7 - (x & 7))) & 1); }

// Frogger PCB: upper/lower nibbles swap entering the scroll and sprite-Y adder.
uint8_t nibble_swap(uint8_t v) { return uint8_t((v << 4) | (v >> 4)); }

// Galaxian 17-bit star LFSR table: enable in bit 7, 6-bit colour in 0–5.
// Cross-checked against MAME stars_init (galaxian_v.cpp), not copied as a
// behavior source — the LFSR taps are the hardware.
const std::array<uint8_t, kStarPeriod>& star_table() {
    static const std::array<uint8_t, kStarPeriod> t = [] {
        std::array<uint8_t, kStarPeriod> out{};
        uint32_t shiftreg = 0;
        for (int i = 0; i < kStarPeriod; i++) {
            int enabled = ((shiftreg & 0x1fe01) == 0x1fe00);
            int color = (~shiftreg & 0x1f8) >> 3;
            out[unsigned(i)] = uint8_t(color | (enabled << 7));
            shiftreg = (shiftreg >> 1) | ((((shiftreg >> 12) ^ ~shiftreg) & 1) << 16);
        }
        return out;
    }();
    return t;
}

// Star guns: 150 Ω LSB / 100 Ω MSB, compressed into 0x00/0x66/0x99/0xFF.
constexpr uint8_t kStarGun[4] = {0x00, 0x66, 0x99, 0xFF};

uint32_t star_rgb(uint8_t color) {
    int r = kStarGun[((color >> 4) & 1) << 1 | ((color >> 5) & 1)];
    int g = kStarGun[((color >> 2) & 1) << 1 | ((color >> 3) & 1)];
    int b = kStarGun[(color & 1) << 1 | ((color >> 1) & 1)];
    return uint32_t((r << 16) | (g << 8) | b);
}

}  // namespace

void Video::reset() {
    h = v = 0;
    vblank = false;
    vblank_edge = false;
    stars_enable = false;
    background_enable = false;
    stars_blink_state = 0;
    blink_acc_ = 0;
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
    if (board == Board::Scramble) {
        blink_acc_ += cpu_cycles;
        while (blink_acc_ >= kStarBlinkPeriod) {
            blink_acc_ -= kStarBlinkPeriod;
            stars_blink_state++;
        }
    }
}

uint8_t Video::remap_color(uint8_t attr) {
    // Frogger PROM address wiring: attribute bits 2/1/0 become PROM
    // group bits 0/1/2. Cross-checked against MAME frogger_extend_tile_info.
    uint8_t c = attr & 7;
    return uint8_t(((c >> 1) & 0x03) | ((c << 2) & 0x04));
}

uint8_t Video::scroll_y(uint8_t v) const {
    return board == Board::Frogger ? nibble_swap(v) : v;
}

uint8_t Video::color_group(uint8_t attr) const {
    return board == Board::Frogger ? remap_color(attr) : uint8_t(attr & 7);
}

uint8_t Video::tile_pixel(uint8_t code, int x, int y) const {
    int row = code * 8 + (y & 7);
    uint8_t hi = gfx[unsigned(row)];            // first chip, high plane
    uint8_t lo = gfx[unsigned(0x800 + row)];    // second chip, low plane
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
    // 82S123: bits 0–2 R, 3–5 G (1 kΩ / 470 Ω / 220 Ω). Blue is only two
    // bits. Namco Galaxian (and Frogger) wire bit 6 = 470 Ω, bit 7 = 220 Ω;
    // there is no 1 kΩ blue. Scramble's else path keeps the 1 kΩ / 470 Ω
    // weights it shipped with. Cross-checked against MAME galaxian_palette
    // resistor table, not a source.
    int b = board == Board::Scramble
                ? (0x21 * ((p >> 6) & 1) + 0x47 * ((p >> 7) & 1))
                : (0x47 * ((p >> 6) & 1) + 0x97 * ((p >> 7) & 1));
    return uint32_t((r << 16) | (g << 8) | b);
}

uint8_t Video::pixel_at(int nx, int ny) const {
    if (nx < 0 || nx >= kVisW || ny < kVisY0 || ny >= kVisY0 + kVisH) return 0;

    int col = nx / 8;
    int row = ny / 8;
    uint8_t scroll = scroll_y(objram[unsigned((col * 2) & 0x3F)]);
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
    uint8_t color = color_group(attr);
    uint8_t pen = uint8_t((color << 2) | pix);

    // Sprites on top. Eight records at $40, 4 bytes each.
    // byte0 = Y, byte1 = code/flip, byte2 = colour, byte3 = X.
    // First three sprites match V−1.
    for (int i = 7; i >= 0; i--) {
        int offs = 0x40 + i * 4;
        int spr_y = scroll_y(objram[unsigned(offs)]);
        uint8_t bits = objram[unsigned(offs + 1)];
        uint8_t scolor = color_group(objram[unsigned(offs + 2)]);
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

uint32_t Video::backdrop(int nx, int ny) const {
    if (board == Board::Frogger) {
        bool river = flip_x ? (nx >= kRiverSplit) : (nx < kRiverSplit);
        return river ? kRiverBlue : 0;
    }
    uint32_t bg = (board == Board::Scramble && background_enable) ? kScrambleBgBlue : 0;
    if (!stars_enable) return bg;
    int blink = (board == Board::Scramble) ? (stars_blink_state & 3) : 3;
    // Scramble blink state 2 suppresses stars when 2V == 0. Namco Galaxian
    // has the star LFSR but no 555 blink. Cross-checked against MAME
    // scramble_draw_stars / galaxian_draw_stars; 555 Ra/Rb/C from Konami.
    if (board == Board::Scramble && blink == 2 && (ny & 2) == 0) return bg;
    static constexpr uint8_t kMask[4] = {0x20, 0x08, 0xFF, 0xFF};
    int enable_star = (ny ^ (nx >> 3)) & 1;
    if (!enable_star) return bg;
    uint32_t offs = uint32_t((ny * 512 + nx * 2) % kStarPeriod);
    uint8_t star = star_table()[offs];
    if ((star & 0x80) != 0 && (star & kMask[blink]) != 0)
        return star_rgb(uint8_t(star & 0x3F));
    return bg;
}

uint32_t Video::bullet_at(int nx, int ny) const {
    if (board != Board::Scramble && board != Board::Galaxian) return 0;
    // Eight shells in object RAM at $60. First three match V−1, the rest
    // match V. Namco Galaxian draws four yellow pixels; Scramble draws two.
    // Cross-checked against MAME bullets_draw / galaxian_draw_bullet.
    int y = ny;
    for (int which = 0; which < 8; which++) {
        int base = 0x60 + which * 4;
        int effy = (which < 3) ? (y - 1) : y;
        if (uint8_t(objram[unsigned(base + 1)] + effy) != 0xFF) continue;
        int x = 255 - objram[unsigned(base + 3)] - 4;
        int width = (board == Board::Galaxian) ? 4 : 2;
        if (nx <= x && nx > x - width) return 0xFFFF00;
    }
    return 0;
}

void Video::render(uint32_t* out) const {
    for (int ny = 0; ny < kVisH; ny++) {
        for (int nx = 0; nx < kVisW; nx++) {
            int visy = kVisY0 + ny;
            uint8_t pen = pixel_at(nx, visy);
            uint32_t rgb;
            if ((pen & 3) == 0) {
                rgb = backdrop(nx, visy);
            } else {
                rgb = prom_rgb(pen);
            }
            uint32_t bullet = bullet_at(nx, visy);
            if (bullet) rgb = bullet;
            // ROT90 = FLIP_X | SWAP_XY: upright((H-1)-y, x) = native(x, y).
            int ux = (kVisH - 1) - ny;
            int uy = nx;
            out[uy * kUprightW + ux] = rgb;
        }
    }
}

}  // namespace galaxian
