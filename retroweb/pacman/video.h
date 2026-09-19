// Namco Pac-Man video: 8×8 tiles + eight 16×16 sprites, 2bpp, PROM palette.
//
// Raster is 384×264 at 6.144 MHz (18.432 MHz / 3). Visible 288×224, then the
// upright monitor rotates 90° CCW to 224×288. Tilemap scan and sprite X/Y
// match the Midway Pac-Man schematics; MAME pacman_v.cpp is a cross-check
// of the decoded VRAM mapper, not a source of behavior.

#ifndef PACMAN_VIDEO_H
#define PACMAN_VIDEO_H

#include <array>
#include <cstdint>

namespace pacman {

constexpr int kHTotal = 384;
constexpr int kVTotal = 264;
constexpr int kVisW = 288;   // unrotated visible
constexpr int kVisH = 224;
constexpr int kUprightW = 224;
constexpr int kUprightH = 288;
constexpr int kCpuPerLine = kHTotal / 2;          // 2 pixels per CPU cycle
constexpr int kCpuPerFrame = (kHTotal * kVTotal) / 2;  // 50688
constexpr int kVBlankLine = kVisH;                // IRQ at start of vblank

struct Sprite {
    uint8_t code = 0;
    uint8_t color = 0;
    bool flip_x = false;
    bool flip_y = false;
    uint8_t x = 0;
    uint8_t y = 0;
};

class Video {
public:
    std::array<uint8_t, 0x400> videoram{};
    std::array<uint8_t, 0x400> colorram{};
    std::array<uint8_t, 16> spriteram{};     // $4FF0
    std::array<uint8_t, 16> sprite_xy{};     // $5060
    std::array<uint8_t, 4096> tile_rom{};    // 5E
    std::array<uint8_t, 4096> sprite_rom{};  // 5F
    std::array<uint8_t, 32> color_prom{};    // 7F
    std::array<uint8_t, 256> lookup_prom{};  // 4A
    bool flip_screen = false;

    int h = 0, v = 0;            // pixel position in the full raster
    bool vblank = false;
    bool vblank_edge = false;    // set for one advance() that enters vblank

    void reset();
    // Advance `cpu_cycles` (2 pixels each). Sets vblank_edge on the 0→1 edge.
    void advance(int cpu_cycles);
    // Render upright 224×288 RGB888 (packed 0x00RRGGBB) into `out` (224*288).
    void render(uint32_t* out) const;

    uint8_t tile_pixel(uint8_t code, int x, int y) const;
    uint8_t sprite_pixel(uint8_t code, int x, int y) const;
    uint32_t lookup_rgb(uint8_t attr, uint8_t pix) const;

private:
    int vram_offset(int col, int row) const;
};

}  // namespace pacman

#endif
