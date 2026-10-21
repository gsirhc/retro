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
constexpr int kSpeedXOffset[8] = {0, 1, 2, 3, -4, -3, -2, -1};

// Share of full scale for a high leg among legs that all drive (monitor input taken as high impedance).
constexpr int dac_level(const double* ohms, int n, int leg) {
    double total = 0;
    for (int i = 0; i < n; i++) total += 1.0 / ohms[i];
    return int(255.0 * (1.0 / ohms[leg]) / total + 0.5);
}

// R6-R8 / R3-R5 / R1-R2 into J2 (Midway video PCB A084-91405-B508).
constexpr double kRedGreenOhms[3] = {1000, 470, 220};
constexpr double kBlueOhms[2] = {470, 220};
constexpr int kPromLevel[3] = {dac_level(kRedGreenOhms, 3, 0), dac_level(kRedGreenOhms, 3, 1),
                               dac_level(kRedGreenOhms, 3, 2)};
constexpr int kTwoLegLevel[2] = {dac_level(kBlueOhms, 2, 0), dac_level(kBlueOhms, 2, 1)};

// V count of the visible line, less the one-line buffer delay (Unsourced).
constexpr int kSpriteVBias = 15;
// Line-buffer address read out at visible x = 0 (Unsourced).
constexpr int kLineBufferOrigin = 40;

uint8_t shifter_pen(uint8_t byte, int x) {
    return uint8_t((((byte >> (7 - x)) & 1) << 1) | ((byte >> (3 - x)) & 1));
}

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
    // High nibble is plane 0, low is plane 1 (same packing as Pac-Man, Lomont).
    const uint8_t* t = &tile_rom[(unsigned(code) * 16) & (tile_rom.size() - 1)];
    return shifter_pen((x < 4) ? t[y + 8] : t[y], x & 3);
}

uint32_t Video::prom_rgb(uint8_t idx) const {
    uint8_t c = palette[idx & 31];
    int r = 0, g = 0, b = 0;
    for (int i = 0; i < 3; i++) {
        if (c & (1 << i)) r += kPromLevel[i];
        if (c & (8 << i)) g += kPromLevel[i];
    }
    if (c & 0x40) b += kTwoLegLevel[0];
    if (c & 0x80) b += kTwoLegLevel[1];
    return uint32_t((r << 16) | (g << 8) | b);
}

uint32_t Video::star_rgb(uint8_t color6) const {
    // 05XX pins 11..6 drive the 470/220 legs; its pin 20 floats the PROM, so the 1K legs carry nothing.
    int level[3] = {};
    for (int ch = 0; ch < 3; ch++) {
        if (color6 & (1 << (ch * 2))) level[ch] += kTwoLegLevel[0];
        if (color6 & (2 << (ch * 2))) level[ch] += kTwoLegLevel[1];
    }
    return uint32_t((level[0] << 16) | (level[1] << 8) | level[2]);
}

uint16_t Video::next_star_lfsr(uint16_t lfsr) {
    // 16-bit Fibonacci LFSR, taps 16/13/11/6.
    uint16_t bit = uint16_t((lfsr ^ (lfsr >> 3) ^ (lfsr >> 5) ^ (lfsr >> 10)) & 1);
    return uint16_t((lfsr >> 1) | (bit << 15));
}

void Video::frame_start() {
    // The 05XX samples its controls as vblank ends. Low $A005 clears and reseeds.
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

    load_sprite_line(line);
}

void Video::load_sprite_line(int line) {
    if (!ram1 || !ram2 || !ram3) return;
    const int vcount = (line + kSpriteVBias) & 0xFF;
    for (int n = 0; n < 64; n++) {
        const unsigned reg = 0x380 + unsigned(n) * 2;
        const uint8_t attr = ram3[reg];
        const bool vsize = (attr & 8) != 0;
        const bool hsize = (attr & 4) != 0;
        // 4B/4C and 3B XOR the sprite's flip bits with FLIP.
        const int vflip = ((attr >> 1) & 1) ^ (flip ? 1 : 0);
        const int hflip = (attr & 1) ^ (flip ? 1 : 0);
        // 3D/3C add Y to V; 2B flags the line when the sum is in the top 16 (32 with VSIZE).
        const int sum = (vcount + ram2[reg]) & 0xFF;
        if (sum < (vsize ? 0xE0 : 0xF0)) continue;
        const int row = (sum & 0x1F) ^ (vflip ? 0x1F : 0);
        // 1B swaps code bit 1 for 16V* under VSIZE and bit 0 for 16H* under HSIZE.
        int picture = ram1[reg] & 0x7F;
        if (vsize) picture = (picture & ~2) | ((row >> 3) & 2);
        const int x_start = ram2[reg + 1] | ((ram3[reg + 1] & 3) << 8);
        const int width = hsize ? 32 : 16;
        const unsigned color = unsigned(ram1[reg + 1] & 0x3F) << 2;
        for (int dot = 0; dot < width; dot++) {
            // 5C/5D/5E count the write address on from X; bit 9 set skips the buffer (polarity Unsourced).
            const int addr = (x_start + dot) & 0x3FF;
            if (addr & 0x200) continue;
            const int x = addr - kLineBufferOrigin;
            if (x < 0 || x >= kVisW) continue;
            const int col = dot ^ (hflip ? width - 1 : 0);
            int code = picture;
            if (hsize) code = (code & ~1) | (col >> 4);
            // 4D/4F address: code, 8V* on A5, 8H/4H on A4:A3, 4V-1V on A2:A0.
            const unsigned rom_addr = unsigned(code) << 6 | unsigned(row & 8) << 2 | unsigned((col >> 2) & 3) << 3 | unsigned(row & 7);
            const uint8_t pen = shifter_pen(sprite_rom[rom_addr % sprite_rom.size()], col & 3);
            const uint8_t li = sprite_lut[color | pen] & 0x0F;
            if (li == 0x0F) continue;
            sprite_line_[unsigned(x)] = prom_rgb(li) | (1u << 24);
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
        // hcnt bit 8 selects the side strips.
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
