#include "video.h"

namespace pacman {
namespace {

// 7F drives each gun through R18/R17/R16 1k/470/220 (red), R14/R15/R13 (green) and
// R34/R33 470/220 (blue) (manual p.33). A divider into the monitor's input is linear in
// the conductance switched high, so full drive normalises to 255 and the load drops out.
constexpr double kRedGreenOhms[3] = {1000.0, 470.0, 220.0};
constexpr double kBlueOhms[2] = {470.0, 220.0};

template <int N>
constexpr std::array<int, 1 << N> gun_levels(const double (&ohms)[N]) {
    std::array<int, 1 << N> out{};
    double total = 0;
    for (int k = 0; k < N; k++) total += 1.0 / ohms[k];
    for (int bits = 0; bits < (1 << N); bits++) {
        double on = 0;
        for (int k = 0; k < N; k++)
            if (bits & (1 << k)) on += 1.0 / ohms[k];
        out[unsigned(bits)] = int(255.0 * on / total + 0.5);
    }
    return out;
}

constexpr auto kGunLevel3 = gun_levels(kRedGreenOhms);
constexpr auto kGunLevel2 = gun_levels(kBlueOhms);

// H counter 3R/3S reloads 128 after 511 and 3N unblanks 240-511 then 128-143 (manual p.32).
int h_count(int x) { return x < 272 ? 240 + x : x - 144; }
// V counter 2R/2S reloads 248 after 511; /VBLANK releases at V = 16 of the 256V half.
int v_count(int line) { return 272 + line; }
// Line buffer cell to first displayed pixel; not readable from the scanned schematic.
constexpr int kLineBufferLag = 1;

}  // namespace

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
// nibble = plane 1 (pixel high bit) (Lomont, Pac-Man hardware notes v0.1, tile and sprite ROM).
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
    int r = kGunLevel3[v & 7];
    int g = kGunLevel3[(v >> 3) & 7];
    int b = kGunLevel2[v >> 6];
    return uint32_t((r << 16) | (g << 8) | b);
}

// V-RAM addresser A082-91384-B000 (Midway Pac-Man manual p.38).
int Video::tile_address(int x, int line) const {
    const int h = h_count(x);
    const int v = v_count(line);
    // U7-U9 XOR 8H-128H and 8V-128V with FLIP.
    const int flip = flip_screen ? 0xF8 : 0;
    const int hp = h ^ flip;
    const int vp = v ^ flip;
    if (h & 0x100) return ((hp >> 3) & 31) | (((vp >> 3) & 31) << 5);
    // 16H = 32H = 64H: U4-U6 take the score columns, AB5 = 8HP and AB6-AB9 = 64HP.
    return ((vp >> 3) & 31) | (((hp >> 3) & 1) << 5) | ((hp & 0x40) ? 0x3C0 : 0);
}

void Video::hblank_setup(int line) {
    sprite_line_.fill(0);
    if (line >= kVisH) return;
    // Lower slots win (Lomont: lower numbered sprites overwrite higher).
    for (int i = 0; i < 8; i++) {
        const uint8_t b0 = spriteram[unsigned(i * 2)];
        const uint8_t color = uint8_t(spriteram[unsigned(i * 2 + 1)] & 0x1F);
        // 74LS89 3F/3H read back inverted; 1F/2F add V with carry-in low during hblank.
        const int y_inv = ~sprite_xy[unsigned(i * 2)] & 0xFF;
        const int x_inv = ~sprite_xy[unsigned(i * 2 + 1)] & 0xFF;
        const int sum = (v_count(line) + y_inv) & 0xFF;
        if ((sum & 0xF0) != 0xF0) continue;  // 3E 74LS20 MATCH
        const int row = sum & 15;
        const bool fx = (b0 & 1) != 0;
        const bool fy = (b0 & 2) != 0;
        const uint8_t code = uint8_t(b0 >> 2);
        for (int px = 0; px < 16; px++) {
            // 1E/2E address a 256-cell buffer, read out while the counter is 256-511.
            const int cell = (x_inv + kLineBufferLag + px) & 0xFF;
            const int x = 16 + cell;
            const int sp = fx ? 15 - px : px;
            if (sprite_line_[unsigned(x)]) continue;
            uint8_t pix = sprite_pixel(code, sp, fy ? 15 - row : row);
            // A pen is clear when its lookup entry selects colour 0 (Lomont: colour 0 is transparent).
            if ((lookup_prom[unsigned((color << 2) | pix)] & 0x0F) == 0) continue;
            sprite_line_[unsigned(x)] = lookup_rgb(color, pix) | (1u << 24);
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
        // Flip turns the tile around too; sprites are positioned by the game.
        const int fine = flip_screen ? 7 : 0;
        int offs = tile_address(x, line);
        uint8_t attr = colorram[unsigned(offs)];
        uint8_t pix = tile_pixel(videoram[unsigned(offs)], (x & 7) ^ fine, (line & 7) ^ fine);
        row[x] = lookup_rgb(attr, pix);
    }
}

void Video::render(uint32_t* out) const {
    // Monitor is rotated 90 degrees: dst(x = (H-1) - native_y, y = native_x).
    for (int ny = 0; ny < kVisH; ny++) {
        for (int nx = 0; nx < kVisW; nx++) {
            out[nx * kUprightW + (kVisH - 1) - ny] = frame_[unsigned(ny * kVisW + nx)];
        }
    }
}

}  // namespace pacman
