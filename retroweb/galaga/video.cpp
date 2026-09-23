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
// Hildinger's starfield_05xx notes (MAME is the cross-check).
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
}

void Video::advance(int cpu_cycles) {
    vblank_edge = false;
    sound_nmi_edge = false;
    int pixels = cpu_cycles * 2;
    h += pixels;
    while (h >= kHTotal) {
        h -= kHTotal;
        v++;
        if (v >= kVTotal) v = 0;
        // Sound-CPU NMI comes from the 07XX vertical chain, twice a frame
        // (scanlines 64 and 192). MAME's cpu3 timer is a cross-check; the
        // enable is !NMION on the CPU-board latch.
        if (v == 64 || v == 192) sound_nmi_edge = true;
        bool vb = v >= kVBlankLine;
        if (vb && !vblank) vblank_edge = true;
        vblank = vb;
    }
}

uint8_t Video::tile_pixel(uint8_t code, int x, int y) const {
    // Same 2bpp nibble packing as the Namco charlayout_2bpp ROMs: high
    // nibble is plane 0, low nibble is plane 1, four pixels per byte.
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
    // 05XX RGB is a separate 2-bit-per-channel ladder from the PROM
    // (Hildinger). R and G sit on a 1k pulldown; B does not.
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

void Video::draw_stars(uint32_t* native) const {
    // $A005 low clears the field and reseeds. Galaga SCROLL_Y is tied
    // low, so only SCROLL_X (latches 0–2) changes the per-frame clocks.
    if (!star_latch[5]) {
        star_lfsr_ = kStarSeed;
        return;
    }
    int speed_x = (star_latch[0] ? 1 : 0) | (star_latch[1] ? 2 : 0) | (star_latch[2] ? 4 : 0);
    int set_a = star_latch[3] ? 1 : 0;
    int set_b = (star_latch[4] ? 1 : 0) | 2;
    int pre = 22 * kStarFieldW + kSpeedXOffset[speed_x & 7];
    int post = 10 * kStarFieldW;
    for (int i = 0; i < pre; i++) star_lfsr_ = next_star_lfsr(star_lfsr_);
    for (int y = 0; y < kVisH; y++) {
        for (int x = kStarOffsetX; x < kStarLimitX; x++) {
            if ((star_lfsr_ & kStarHitMask) == kStarHitValue) {
                int star_set = int(((star_lfsr_ >> 10) & 1) << 1) | int((star_lfsr_ >> 8) & 1);
                if (star_set == set_a || star_set == set_b) {
                    uint8_t color = uint8_t((star_lfsr_ >> 5) & 7);
                    color = uint8_t(color | ((star_lfsr_ << 3) & 0x18));
                    color = uint8_t(color | ((star_lfsr_ << 2) & 0x20));
                    color = uint8_t((~color) & 0x3F);
                    int dx = x;
                    if (flip) dx += 64;
                    if (dx >= 0 && dx < kVisW)
                        native[y * kVisW + dx] = star_rgb(color);
                }
            }
            star_lfsr_ = next_star_lfsr(star_lfsr_);
        }
    }
    for (int i = 0; i < post; i++) star_lfsr_ = next_star_lfsr(star_lfsr_);
}

void Video::render(uint32_t* upright, const uint8_t* ram1, const uint8_t* ram2,
                   const uint8_t* ram3) const {
    std::array<uint32_t, kVisW * kVisH> native{};
    // Stars, then sprites, then tiles. The score strips sit in the tile
    // plane, so they stay on top of any sprite that wanders into that
    // band (MAME screen_update_galaga is the cross-check).
    draw_stars(native.data());

    if (ram1 && ram2 && ram3) {
        // 04XX sprite address. X is 10 bits: the position byte plus two
        // bits in the next register. Register 0 sits 40 pixels left of the
        // visible origin. Y counts down from the register, one line late,
        // then wraps by the chain's 32-pixel offset. A 2× sprite is four
        // consecutive codes, not one stretched picture.
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
            int sx = int(ram2[o + 1]) - 40 + 0x100 * (ram3[o + 1] & 3);
            int sy = 256 - int(ram2[o]) + 1;
            sy -= 16 * sizey;
            sy = (sy & 0xff) - 32;
            int base = ram1[o] & 0x7f;
            uint8_t color = ram1[o + 1];
            // sphcnt(3 downto 2) selects bytes 0, 8, 16, 24 (greyrogue
            // galaga.vhd spgraphx_addr). Pac-Man's sprite ROM starts at
            // byte 8; reading Galaga that way swaps the two ends.
            static constexpr int kOffs[2][2] = {{0, 1}, {2, 3}};
            static constexpr int kCol[4] = {0, 8, 16, 24};
            for (int ty = 0; ty <= sizey; ty++) {
                for (int tx = 0; tx <= sizex; tx++) {
                    int code = base + kOffs[ty ^ (sizey & flipy)][tx ^ (sizex & flipx)];
                    const uint8_t* t = &sprite_rom[(unsigned(code) * 64) % sprite_rom.size()];
                    for (int py = 0; py < 16; py++) {
                        for (int px = 0; px < 16; px++) {
                            int x = sx + tx * 16 + (flipx ? (15 - px) : px);
                            int y = sy + ty * 16 + (flipy ? (15 - py) : py);
                            if (x < 0 || y < 0 || x >= kVisW || y >= kVisH) continue;
                            int base_y = (py < 8) ? py : (32 + (py - 8));
                            uint8_t byte = t[kCol[px >> 2] + base_y];
                            int xb = px & 3;
                            uint8_t pen = uint8_t((((byte >> (7 - xb)) & 1) << 1) |
                                                  ((byte >> (3 - xb)) & 1));
                            if (pen == 0) continue;
                            uint8_t li = sprite_lut[((color & 0x3F) << 2) | pen] & 0x0F;
                            native[y * kVisW + x] = prom_rgb(li);
                        }
                    }
                }
            }
        }
    }

    // hcnt bit 8 selects the side strips (greyrogue galaga.vhd,
    // flip_h = 0). A credit line stored across row 1 is walked by
    // vcnt, so it lies on the bottom of the upright screen.
    for (int row = 0; row < 28; row++) {
        for (int col = 0; col < 36; col++) {
            int sr = flip ? (27 - row) : row;
            int sc = flip ? (35 - col) : col;
            int r = sr + 2;
            int c = sc - 2;
            int offs = (c & 0x20) ? (r + ((c & 0x1f) << 5)) : (c + (r << 5));
            offs &= 0x3ff;
            uint8_t code = videoram[offs];
            uint8_t attr = videoram[0x400 + offs];
            int ox = col * 8;
            int oy = row * 8;
            for (int py = 0; py < 8; py++) {
                for (int px = 0; px < 8; px++) {
                    uint8_t pen = tile_pixel(code, px, py);
                    if (pen == 0) continue;
                    // Character LUT is 4 bits; the board ORs 0x10 so tiles
                    // read the upper half of the 32-color PROM. Index 0x1F
                    // is the transparent group.
                    uint8_t li = uint8_t((char_lut[((attr & 0x3F) << 2) | pen] & 0x0F) | 0x10);
                    if (li == 0x1F) continue;
                    int x = ox + px;
                    int y = oy + py;
                    if (x >= 0 && x < kVisW && y >= 0 && y < kVisH)
                        native[y * kVisW + x] = prom_rgb(li);
                }
            }
        }
    }

    for (int ny = 0; ny < kVisH; ny++) {
        for (int nx = 0; nx < kVisW; nx++) {
            int dx = (kVisH - 1) - ny;
            int dy = nx;
            upright[dy * kUprightW + dx] = native[ny * kVisW + nx];
        }
    }
}

}  // namespace galaga
