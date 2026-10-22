#include "video.h"

namespace galaga {

namespace {

// 16-bit M-sequence clocked once per dot over a 256x256 field (Namco, CEDEC 2014 slide 22).
constexpr int kFieldDots = 256;
// The field fills the 256 columns between the score strips (CEDEC 2014 slide 22 screenshot).
constexpr int kFieldLeft = (kVisW - kFieldDots) / 2;
// Own design, unsourced: seed, and a 0 then eight 1s in bits 15-7 lights a star (128 per field).
constexpr uint16_t kFieldSeed = 0x0001;
constexpr uint16_t kHitMask = 0xFF80;
constexpr uint16_t kHitBits = 0x7F80;

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
// H and V counts at the first visible dot and line, placed so 256H spans the 256 playfield dots (Unsourced).
constexpr int kTileHOrigin = 0xF0;
constexpr int kTileVOrigin = 16;

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
    field_rng_ = kFieldSeed;
    field_enabled_ = false;
    field_trim_ = 0;
    field_blink_ = 0;
    sprite_line_.fill(0);
    field_dots_.fill(0);
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
    // 05XX pins 11/10, 9/8, 7/6 drive only the 470/220 legs of red, green, blue (J2, page 7-7).
    auto level = [](int pair) { return ((pair & 1) ? kTwoLegLevel[0] : 0) + ((pair & 2) ? kTwoLegLevel[1] : 0); };
    return uint32_t(level(color6) << 16 | level(color6 >> 2) << 8 | level(color6 >> 4));
}

uint16_t Video::field_next(uint16_t s) {
    // Taps 16,15,13,4 (Xilinx XAPP052 Table 3), XOR feedback into bit 0.
    const unsigned in = ((s >> 15) ^ (s >> 14) ^ (s >> 12) ^ (s >> 3)) & 1;
    return uint16_t((s << 1) | in);
}

void Video::sample_field_controls() {
    field_enabled_ = star_latch[5] != 0;
    // Own design, unsourced: latches 0-1 count 0-3 clocks, latch 2 drops instead of adds.
    const int clocks = star_latch[0] | star_latch[1] << 1;
    field_trim_ = star_latch[2] ? -clocks : clocks;
    // Own design, unsourced: latches 3-4 pick the one star set of four held dark.
    field_blink_ = star_latch[3] | star_latch[4] << 1;
}

void Video::hblank_setup(int line) {
    sprite_line_.fill(0);
    field_dots_.fill(0);
    if (line == 0) sample_field_controls();
    if (field_enabled_ && line < kFieldDots) {
        // The off-screen field lines carry the scroll trim, gated in before VSYNC (pin4.at 05XX).
        int dots = kFieldDots + (line == kFieldDots - 1 ? field_trim_ : 0);
        for (int i = 0; i < dots; i++) {
            const bool hit = line < kVisH && (field_rng_ & kHitMask) == kHitBits;
            // Own design, unsourced: bits 6-5 are the star set, bits 5-0 the colour.
            if (hit && ((field_rng_ >> 5) & 3) != field_blink_)
                field_dots_[unsigned(kFieldLeft + i)] = star_rgb(uint8_t(field_rng_ & 0x3F)) | (1u << 24);
            field_rng_ = field_next(field_rng_);
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
        if (!px) px = field_dots_[unsigned(x)];
        // 07XX count at this dot (origin Unsourced); FLIP inverts 8H-128H and 1V-128V.
        int hc = (x + kTileHOrigin) & 0x1FF;
        int vc = line + kTileVOrigin;
        if (flip) {
            hc ^= 0xFF;
            vc ^= 0xFF;
        }
        // 1L 00XX builds the address from the H and V counts (sheet 7-7); 256H low swaps the two fields.
        const unsigned tile_col = unsigned(hc >> 3) & 31, tile_row = unsigned(vc >> 3) & 31;
        const unsigned offs = (hc & 0x100) ? (tile_row << 5 | tile_col) : (tile_col << 5 | tile_row);
        uint8_t code = uint8_t((videoram[offs] & 0x7f) | (flip ? 0x80 : 0));
        uint8_t attr = videoram[0x400 + offs];
        // 4H picks the byte half; the second char set reverses each nibble under flip.
        uint8_t pen = tile_pixel(code, (hc & 4) | (x & 3), vc & 7);
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
