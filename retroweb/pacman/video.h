// Namco Pac-Man video: 8x8 tiles + eight 16x16 sprites, 2bpp, PROM palette.
// Raster is 384x264 at 6.144 MHz, visible 288x224, rotated 90 deg CCW upright.

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
    // Set when the beam enters vblank; the board clears it once handled.
    bool vblank_edge = false;

    void reset();
    // Moves the beam `cpu_cycles` T-states (2 pixels each), painting as it goes.
    void advance(int cpu_cycles);
    // Upright 224x288 RGB888 (0x00RRGGBB) of the last painted frame.
    void render(uint32_t* out) const;

    uint8_t tile_pixel(uint8_t code, int x, int y) const;
    uint8_t sprite_pixel(uint8_t code, int x, int y) const;
    uint32_t lookup_rgb(uint8_t attr, uint8_t pix) const;

private:
    std::array<uint32_t, kVisW * kVisH> frame_{};
    std::array<uint32_t, kVisW> sprite_line_{};  // RGB | 1 << 24, 0 = empty

    int vram_offset(int col, int row) const;
    void hblank_setup(int line);
    void paint(int line, int x0, int x1);
};

}  // namespace pacman

#endif
