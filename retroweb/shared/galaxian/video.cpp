#include "video.h"

#include <array>

namespace galaxian {
namespace {

uint8_t plane_bit(uint8_t b, int x) { return uint8_t((b >> (7 - (x & 7))) & 1); }

// Frogger PCB: upper and lower nibbles swap entering the scroll and sprite-Y adder.
uint8_t nibble_swap(uint8_t v) { return uint8_t((v << 4) | (v >> 4)); }

// 17-bit star LFSR: enable in bit 7, 6-bit colour in 0-5 (MAME stars_init).
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

// PROM R/G 1k/470/220, B 470/220, 470 ohm pulldown, scaled so white is 224
// to leave headroom for stars and shells (MAME galaxian_palette).
struct Weights {
    double rg[3];
    double b[2];
};

const Weights& weights() {
    static const Weights w = [] {
        auto divider = [](const double* r, int n, int bit) {
            double g_on = 1.0 / r[bit];
            double g_off = 1.0 / 470.0;
            for (int j = 0; j < n; j++)
                if (j != bit) g_off += 1.0 / r[j];
            return g_on / (g_on + g_off);
        };
        const double rg[3] = {1000, 470, 220};
        const double bl[2] = {470, 220};
        Weights out{};
        double max_rg = 0, max_b = 0;
        for (int i = 0; i < 3; i++) max_rg += (out.rg[i] = divider(rg, 3, i));
        for (int i = 0; i < 2; i++) max_b += (out.b[i] = divider(bl, 2, i));
        double scale = 224.0 / (max_rg > max_b ? max_rg : max_b);
        for (double& x : out.rg) x *= scale;
        for (double& x : out.b) x *= scale;
        return out;
    }();
    return w;
}

uint32_t channel_or(uint32_t rgb, uint32_t bullet) {
    uint32_t out = rgb;
    for (int shift = 0; shift < 24; shift += 8)
        if ((bullet >> shift) & 0xFF) out |= 0xFFu << shift;
    return out;
}

uint32_t blend3(uint32_t first, uint32_t second) {
    uint32_t out = 0;
    for (int shift = 0; shift < 24; shift += 8) {
        uint32_t a = (first >> shift) & 0xFF;
        uint32_t b = (second >> shift) & 0xFF;
        out |= ((a + 2 * b + 1) / 3) << shift;
    }
    return out;
}

constexpr uint32_t kWhite = 0xFFFFFF;
constexpr uint32_t kYellow = 0xFFFF00;

}  // namespace

void Video::reset() {
    h = v = 0;
    vblank = false;
    vblank_edge = false;
    stars_enable_ = false;
    background_enable = false;
    flip_x = flip_y = false;
    stars_blink_state = 0;
    blink_acc_ = 0;
    star_origin = 0;
    sprite_line_.fill(0);
    shell_x_ = missile_x_ = -1;
}

void Video::set_stars_enable(bool on) {
    // Releasing CLR on the shift registers restarts the count at the beam (MAME stars_enable_w).
    if (on && !stars_enable_) {
        int hpos = h < kVisW ? h : kVisW;
        star_origin = uint32_t((kStarPeriod - ((v * 512 + 2 * hpos) % kStarPeriod)) % kStarPeriod);
    }
    stars_enable_ = on;
}

uint8_t Video::remap_color(uint8_t attr) {
    // Frogger PROM wiring: attribute bits 2/1/0 become PROM group bits 0/1/2
    uint8_t c = attr & 7;
    return uint8_t(((c >> 1) & 0x03) | ((c << 2) & 0x04));
}

uint8_t Video::scroll_y(uint8_t value) const {
    return board == Board::Frogger ? nibble_swap(value) : value;
}

uint8_t Video::color_group(uint8_t attr) const {
    return board == Board::Frogger ? remap_color(attr) : uint8_t(attr & 7);
}

uint8_t Video::tile_pixel(uint8_t code, int x, int y) const {
    int row = code * 8 + (y & 7);
    uint8_t hi = gfx[unsigned(row)];
    uint8_t lo = gfx[unsigned(0x800 + row)];
    return uint8_t(plane_bit(lo, x) | (plane_bit(hi, x) << 1));
}

uint8_t Video::sprite_pixel(uint8_t code, int x, int y) const {
    // 16x16: rows 8-15 sit 16 bytes on, columns 8-15 a further 8 (MAME spritelayout)
    int yy = y & 15;
    int row = (yy < 8) ? yy : (16 + (yy - 8));
    int base = (code & 0x3F) * 32 + row;
    if (x & 8) base += 8;
    uint8_t hi = gfx[unsigned(base)];
    uint8_t lo = gfx[unsigned(0x800 + base)];
    return uint8_t(plane_bit(lo, x) | (plane_bit(hi, x) << 1));
}

uint32_t Video::prom_rgb(uint8_t pen) const {
    const Weights& w = weights();
    uint8_t p = color_prom[pen & 31];
    auto bit = [p](int n) { return (p >> n) & 1; };
    int r = int(w.rg[0] * bit(0) + w.rg[1] * bit(1) + w.rg[2] * bit(2) + 0.5);
    int g = int(w.rg[0] * bit(3) + w.rg[1] * bit(4) + w.rg[2] * bit(5) + 0.5);
    int b = int(w.b[0] * bit(6) + w.b[1] * bit(7) + 0.5);
    return uint32_t((r << 16) | (g << 8) | b);
}

uint32_t Video::star_rgb(uint8_t color) {
    // 150 and 100 ohm star resistors against the 130 ohm PROM maximum, compressed
    // into 194..255 (MAME galaxian_palette starmap).
    static constexpr uint8_t kLevel[4] = {0, 194, 214, 255};
    int r = kLevel[((color >> 4) & 1) << 1 | ((color >> 5) & 1)];
    int g = kLevel[((color >> 2) & 1) << 1 | ((color >> 3) & 1)];
    int b = kLevel[(color & 1) << 1 | ((color >> 1) & 1)];
    return uint32_t((r << 16) | (g << 8) | b);
}

uint8_t Video::tile_pen(int x, int y) const {
    // Flip inverts the H and V counters ahead of the scroll adder.
    int hx = flip_x ? 255 - x : x;
    int vy = flip_y ? 255 - y : y;
    int col = hx >> 3;
    int sy = (vy + scroll_y(objram[unsigned(col * 2)])) & 0xFF;
    uint8_t code = videoram[unsigned((sy >> 3) * 32 + col)];
    uint8_t attr = objram[unsigned(col * 2 + 1)];
    return uint8_t((color_group(attr) << 2) | tile_pixel(code, hx & 7, sy & 7));
}

void Video::hblank_setup(int line) {
    // Sprites load the line buffer during HBLANK; it only takes a pixel where it
    // still holds 0, so lower-numbered sprites win (MAME sprites_draw).
    sprite_line_.fill(0);
    for (int i = 0; i < 8; i++) {
        const uint8_t* base = &objram[unsigned(0x40 + i * 4)];
        // The first three sprites match against V-1 (MAME sprites_draw).
        uint8_t sy = uint8_t(240 - (scroll_y(base[0]) - (i < 3 ? 1 : 0)));
        bool fx = base[1] & 0x40;
        bool fy = base[1] & 0x80;
        uint8_t sx = uint8_t(base[3] + 1);
        if (flip_x) {
            sx = uint8_t(240 - sx);
            fx = !fx;
        }
        if (flip_y) {
            sy = uint8_t(240 - sy);
            fy = !fy;
        }
        int row = uint8_t(line - sy);
        if (row >= 16) continue;
        uint8_t color = color_group(base[2]);
        for (int px = 0; px < 16; px++) {
            int x = sx + px;
            // 16 pixels are hard-clipped at the line buffer, the first 16 after the +1.
            if (x > 255 || (flip_x ? x >= 256 - 17 : x < 17)) continue;
            if (sprite_line_[unsigned(x)]) continue;
            uint8_t pix = sprite_pixel(uint8_t(base[1] & 0x3F), fx ? 15 - px : px, fy ? 15 - row : row);
            if (pix) sprite_line_[unsigned(x)] = uint8_t(((color << 2) | pix) + 1);
        }
    }

    // One shell counter and one missile counter, the last match wins (MAME bullets_draw).
    shell_x_ = missile_x_ = -1;
    uint8_t effy = uint8_t(flip_y ? ((line - 1) ^ 255) : (line - 1));
    for (int which = 0; which < 3; which++)
        if (uint8_t(objram[unsigned(0x60 + which * 4 + 1)] + effy) == 0xFF)
            shell_x_ = 255 - objram[unsigned(0x60 + which * 4 + 3)];
    effy = uint8_t(flip_y ? (line ^ 255) : line);
    for (int which = 3; which < 8; which++) {
        if (uint8_t(objram[unsigned(0x60 + which * 4 + 1)] + effy) != 0xFF) continue;
        int x = 255 - objram[unsigned(0x60 + which * 4 + 3)];
        if (which == 7) missile_x_ = x;
        else shell_x_ = x;
    }
}

uint32_t Video::backdrop(int x, int y) const {
    if (board == Board::Frogger) {
        bool river = flip_x ? (x >= kRiverSplit) : (x < kRiverSplit);
        return river ? kRiverBlue : 0;
    }
    uint32_t bg = (board == Board::Scramble && background_enable) ? kScrambleBgBlue : 0;
    if (!stars_enable_) return bg;
    uint8_t mask = 0xFF;
    uint32_t base = uint32_t(y) * 512;
    if (board == Board::Scramble) {
        static constexpr uint8_t kMask[4] = {0x20, 0x08, 0xFF, 0xFF};
        int blink = stars_blink_state & 3;
        if (blink == 2 && (y & 2) == 0) return bg;
        mask = kMask[blink];
    } else {
        base += star_origin;
    }
    // Stars show only when V1 ^ H8.
    if (((y ^ (x >> 3)) & 1) == 0) return bg;
    // The RNG steps twice per pixel on the 18 MHz clock gated by the 2/3-duty
    // pixel clock: the first sample covers a third of the pixel, the second two.
    const auto& t = star_table();
    uint32_t offs = (base + uint32_t(2 * x)) % kStarPeriod;
    uint8_t s1 = t[offs];
    uint8_t s2 = t[(offs + 1) % kStarPeriod];
    uint32_t c1 = ((s1 & 0x80) && (s1 & mask)) ? star_rgb(uint8_t(s1 & 0x3F)) : bg;
    uint32_t c2 = ((s2 & 0x80) && (s2 & mask)) ? star_rgb(uint8_t(s2 & 0x3F)) : bg;
    return c1 == c2 ? c1 : blend3(c1, c2);
}

uint32_t Video::bullet_or(int x, uint32_t rgb) const {
    // Shells and missiles are OR'd into the RGB through 100 ohm resistors.
    if (board == Board::Scramble) {
        // Scramble shells start at $FA and last two pixel clocks, all yellow.
        auto hit = [x](int bx) { return bx >= 0 && (x == bx - 5 || x == bx - 6); };
        if (hit(shell_x_) || hit(missile_x_)) rgb = channel_or(rgb, kYellow);
        return rgb;
    }
    // Galaxian and Frogger: $FC to $00 is four pixels; shells white, missile yellow.
    auto hit = [x](int bx) { return bx >= 0 && x >= bx - 4 && x < bx; };
    if (hit(shell_x_)) rgb = channel_or(rgb, kWhite);
    if (hit(missile_x_)) rgb = channel_or(rgb, kYellow);
    return rgb;
}

void Video::paint(int line, int x0, int x1) {
    uint32_t* row = &frame_[unsigned((line - kVisY0) * kVisW)];
    for (int x = x0; x < x1; x++) {
        uint8_t pen = tile_pen(x, line);
        if (uint8_t s = sprite_line_[unsigned(x)]) pen = uint8_t(s - 1);
        uint32_t rgb = (pen & 3) ? prom_rgb(pen) : backdrop(x, line);
        row[x] = bullet_or(x, rgb);
    }
}

void Video::advance(int cpu_cycles) {
    int pixels = cpu_cycles * 2;
    while (pixels > 0) {
        bool visible_line = v >= kVisY0 && v < kVisY0 + kVisH;
        if (h < kVisW) {
            int end = h + pixels < kVisW ? h + pixels : kVisW;
            if (visible_line) paint(v, h, end);
            pixels -= end - h;
            h = end;
            if (h == kVisW) hblank_setup(v + 1);
            continue;
        }
        int end = h + pixels < kHTotal ? h + pixels : kHTotal;
        pixels -= end - h;
        h = end;
        if (h < kHTotal) continue;
        h = 0;
        v++;
        if (v == kVBlankLine) {
            vblank = true;
            vblank_edge = true;
        }
        if (v >= kVTotal) {
            v = 0;
            vblank = false;
            // 2^17 RNG clocks a frame, less two through 6B unflipped: one step of drift (MAME).
            if (board == Board::Galaxian)
                star_origin = (star_origin + (flip_x ? 1u : uint32_t(kStarPeriod - 1))) % kStarPeriod;
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

void Video::render(uint32_t* out) const {
    for (int ny = 0; ny < kVisH; ny++) {
        for (int nx = 0; nx < kVisW; nx++) {
            // ROT90 = FLIP_X | SWAP_XY: upright((H-1)-y, x) = native(x, y).
            out[nx * kUprightW + (kVisH - 1) - ny] = frame_[unsigned(ny * kVisW + nx)];
        }
    }
}

}  // namespace galaxian
