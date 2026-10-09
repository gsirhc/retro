#include "video.h"

namespace galaga {

namespace {

constexpr uint16_t kStarHitMask = 0xFA14;
constexpr uint16_t kStarHitValue = 0x7800;
constexpr uint16_t kStarSeed = 0x7FFF;
constexpr int kStarFieldW = 256;
constexpr int kStarOffsetX = 16;
constexpr int kStarLimitX = kStarOffsetX + kStarFieldW;

// SCROLL_X index → extra LFSR clocks in the pre-visible interval.
// Hildinger starfield_05xx notes.
constexpr int kSpeedXOffset[8] = {0, 1, 2, 3, -4, -3, -2, -1};

}  // namespace

void Video::reset() {
    videoram.fill(0);
    star_latch.fill(0);
    flip = false;
    h = v = 0;
    vblank = false;
    vblank_edge = false;
    sound_nmi_edge = false;
    star_lfsr_ = kStarSeed;
    stars_on_ = false;
    sprite_line_.fill(0);
    star_line_.fill(0);
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
        // Sound-CPU NMI twice a frame (scanlines 64 and 192), enabled by !NMION.
        if (v == 64 || v == 192) sound_nmi_edge = true;
        if (v == kVBlankLine) {
            vblank = true;
            vblank_edge = true;
            if (stars_on_)
                for (int i = 0; i < 10 * kStarFieldW; i++) star_lfsr_ = next_star_lfsr(star_lfsr_);
        } else if (v == 0) {
            vblank = false;
        }
    }
}

uint8_t Video::tile_pixel(uint8_t code, int x, int y) const {
    // Namco charlayout_2bpp: high nibble is plane 0, low is plane 1.
    const uint8_t* t = &tile_rom[(unsigned(code) * 16) & (tile_rom.size() - 1)];
    uint8_t byte = (x < 4) ? t[y + 8] : t[y];
    int xb = x & 3;
    return uint8_t((((byte >> (7 - xb)) & 1) << 1) | ((byte >> (3 - xb)) & 1));
}

uint32_t Video::prom_rgb(uint8_t idx) const {
    uint8_t c = palette[idx & 31];
    int r = ((c & 1) ? 0x21 : 0) + ((c & 2) ? 0x47 : 0) + ((c & 4) ? 0x97 : 0);
    int g = ((c & 8) ? 0x21 : 0) + ((c & 16) ? 0x47 : 0) + ((c & 32) ? 0x97 : 0);
    int b = ((c & 64) ? 0x51 : 0) + ((c & 128) ? 0xAE : 0);
    return uint32_t((r << 16) | (g << 8) | b);
}

uint32_t Video::star_rgb(uint8_t color6) const {
    // 05XX RGB ladder from the PROM (Hildinger); R and G have a 1k pulldown, B does not.
    int r = ((color6 & 1) ? 0x47 : 0) + ((color6 & 2) ? 0x97 : 0);
    int g = ((color6 & 4) ? 0x47 : 0) + ((color6 & 8) ? 0x97 : 0);
    int b = ((color6 & 16) ? 0x47 : 0) + ((color6 & 32) ? 0x97 : 0);
    if (r) r = 0x20 + r;
    if (g) g = 0x20 + g;
    return uint32_t((r << 16) | (g << 8) | b);
}

uint16_t Video::next_star_lfsr(uint16_t lfsr) {
    // 16-bit Fibonacci LFSR, taps 16/13/11/6 (Hildinger RE of the 05XX).
    uint16_t bit = uint16_t((lfsr ^ (lfsr >> 3) ^ (lfsr >> 5) ^ (lfsr >> 10)) & 1);
    return uint16_t((lfsr >> 1) | (bit << 15));
}

void Video::frame_start() {
    // The 05XX samples its controls as vblank ends (MAME screen_vblank_galaga). Low $A005 clears and reseeds.
    stars_on_ = star_latch[5] != 0;
    if (!stars_on_) {
        star_lfsr_ = kStarSeed;
        return;
    }
    int speed_x = (star_latch[0] ? 1 : 0) | (star_latch[1] ? 2 : 0) | (star_latch[2] ? 4 : 0);
    star_set_a_ = star_latch[3] ? 1 : 0;
    star_set_b_ = (star_latch[4] ? 1 : 0) | 2;
    // SCROLL_Y is tied low; SCROLL_X adds or drops clocks ahead of the visible field.
    for (int i = 0; i < 22 * kStarFieldW + kSpeedXOffset[speed_x & 7]; i++) star_lfsr_ = next_star_lfsr(star_lfsr_);
}

void Video::hblank_setup(int line) {
    sprite_line_.fill(0);
    star_line_.fill(0);
    if (line == 0) frame_start();
    if (line >= kVisH) return;

    if (stars_on_) {
        for (int x = kStarOffsetX; x < kStarLimitX; x++) {
            if ((star_lfsr_ & kStarHitMask) == kStarHitValue) {
                int star_set = int(((star_lfsr_ >> 10) & 1) << 1) | int((star_lfsr_ >> 8) & 1);
                if (star_set == star_set_a_ || star_set == star_set_b_) {
                    uint8_t color = uint8_t((star_lfsr_ >> 5) & 7);
                    color = uint8_t(color | ((star_lfsr_ << 3) & 0x18));
                    color = uint8_t(color | ((star_lfsr_ << 2) & 0x20));
                    color = uint8_t((~color) & 0x3F);
                    int dx = flip ? x + 64 : x;
                    if (dx < kVisW) star_line_[unsigned(dx)] = star_rgb(color) | (1u << 24);
                }
            }
            star_lfsr_ = next_star_lfsr(star_lfsr_);
        }
    }

    if (!ram1 || !ram2 || !ram3) return;
    // 04XX: X is 10 bits, register 0 is 40 px left of the visible origin. Y is one line late
    // and wraps by 32. Later sprites overwrite earlier ones (MAME draw_sprites order).
    static constexpr int kOffs[2][2] = {{0, 1}, {2, 3}};
    static constexpr int kCol[4] = {0, 8, 16, 24};
    for (int i = 0; i < 64; i++) {
        int o = 0x380 + i * 2;
        uint8_t attr = ram3[o];
        bool flipx = (attr & 1) != 0;
        bool flipy = (attr & 2) != 0;
        int sizex = (attr & 4) ? 1 : 0;
        int sizey = (attr & 8) ? 1 : 0;
        if (flip) {
            flipx = !flipx;
            flipy = !flipy;
        }
        int sy = 256 - int(ram2[o]) + 1;
        sy -= 16 * sizey;
        sy = (sy & 0xff) - 32;
        int rel = line - sy;
        if (rel < 0 || rel >= 16 * (sizey + 1)) continue;
        int sx = int(ram2[o + 1]) - 40 + 0x100 * (ram3[o + 1] & 3);
        int ty = rel >> 4;
        int py = flipy ? 15 - (rel & 15) : (rel & 15);
        uint8_t color = uint8_t(ram1[o + 1] & 0x3F);
        for (int tx = 0; tx <= sizex; tx++) {
            int code = (ram1[o] & 0x7f) + kOffs[ty ^ (sizey & flipy)][tx ^ (sizex & flipx)];
            const uint8_t* t = &sprite_rom[(unsigned(code) * 64) % sprite_rom.size()];
            int base_y = (py < 8) ? py : (32 + (py - 8));
            for (int px = 0; px < 16; px++) {
                int x = sx + tx * 16 + (flipx ? (15 - px) : px);
                if (x < 0 || x >= kVisW) continue;
                uint8_t byte = t[kCol[px >> 2] + base_y];
                int xb = px & 3;
                uint8_t pen = uint8_t((((byte >> (7 - xb)) & 1) << 1) | ((byte >> (3 - xb)) & 1));
                // A pen is clear when its LUT entry is 0x0F (MAME transpen_mask(..., 0x0f)).
                uint8_t li = sprite_lut[unsigned((color << 2) | pen)] & 0x0F;
                if (li == 0x0F) continue;
                sprite_line_[unsigned(x)] = prom_rgb(li) | (1u << 24);
            }
        }
    }
}

void Video::paint(int line, int x0, int x1) {
    uint32_t* row = &frame_[unsigned(line * kVisW)];
    for (int x = x0; x < x1; x++) {
        uint32_t px = sprite_line_[unsigned(x)];
        if (!px) px = star_line_[unsigned(x)];
        // Flip inverts the counters, which pick the byte half; the second char set reverses each nibble.
        int tx = flip ? kVisW - 1 - x : x;
        int ty = flip ? kVisH - 1 - line : line;
        int r = (ty >> 3) + 2;
        int c = (tx >> 3) - 2;
        // hcnt bit 8 selects the side strips (galaga.vhd).
        int offs = ((c & 0x20) ? (r + ((c & 0x1f) << 5)) : (c + (r << 5))) & 0x3ff;
        uint8_t code = uint8_t((videoram[unsigned(offs)] & 0x7f) | (flip ? 0x80 : 0));
        uint8_t attr = videoram[unsigned(0x400 + offs)];
        uint8_t pen = tile_pixel(code, (tx & 4) | (x & 3), ty & 7);
        // Character LUT is 4 bits; the board ORs 0x10 to use the upper PROM half. 0x1F is transparent.
        uint8_t li = uint8_t((char_lut[unsigned(((attr & 0x3F) << 2) | pen)] & 0x0F) | 0x10);
        if (li != 0x1F) px = prom_rgb(li);
        row[x] = px & 0xFFFFFF;
    }
}

void Video::render(uint32_t* upright) const {
    for (int ny = 0; ny < kVisH; ny++) {
        for (int nx = 0; nx < kVisW; nx++) {
            upright[nx * kUprightW + (kVisH - 1) - ny] = frame_[unsigned(ny * kVisW + nx)];
        }
    }
}

}  // namespace galaga
