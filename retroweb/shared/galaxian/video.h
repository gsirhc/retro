// Galaxian-family video: Namco Galaxian (1979), Konami Frogger and Scramble (1981).
// Raster 384x264 at 6.144 MHz (18.432 / 3), visible 256x224 native, rotated 90
// (MAME ROT90 = FLIP_X | SWAP_XY) to 224x256. 32x32 tilemap of 8x8 2bpp tiles,
// MSB-left. Object RAM: per-column scroll+colour and eight 16x16 sprites.
// Frogger adds nibble-swap, river split, open PROM blue bit 0, attribute remap.
// Scramble adds star blink, background, two-pixel shells. Galaxian: stars with
// no 555 blink, four-pixel yellow bullets.

#ifndef GALAXIAN_VIDEO_H
#define GALAXIAN_VIDEO_H

#include <array>
#include <cstdint>

namespace galaxian {

constexpr int kHTotal = 384;
constexpr int kVTotal = 264;
constexpr int kVisW = 256;   // unrotated visible
constexpr int kVisH = 224;
constexpr int kVisY0 = 16;   // skip two tile rows (Galaxian visarea)
constexpr int kUprightW = 224;
constexpr int kUprightH = 256;
constexpr int kCpuPerLine = kHTotal / 2;                 // 192
constexpr int kCpuPerFrame = (kHTotal * kVTotal) / 2;    // 50688
constexpr int kVBlankLine = kVisY0 + kVisH;              // 240
constexpr int kRiverSplit = 128;                         // native x
constexpr uint32_t kRiverBlue = 0x000047;                // 470 Ω blue gun
constexpr uint32_t kScrambleBgBlue = 0x000056;           // 390 Ω blue gun
constexpr int kCpuHz = 3072000;
// 555 astable Ra=100k, Rb=10k, C=10µF. T = ln(2)*(Ra+2Rb)*C ≈ 0.8318 s.
constexpr int kStarBlinkPeriod = 2552468;

enum class Board { Frogger, Scramble, Galaxian };

class Video {
public:
    Board board = Board::Frogger;
    std::array<uint8_t, 0x400> videoram{};
    std::array<uint8_t, 0x100> objram{};
    std::array<uint8_t, 0x1000> gfx{};
    std::array<uint8_t, 32> color_prom{};
    bool flip_x = false;
    bool flip_y = false;
    bool stars_enable = false;
    bool background_enable = false;
    uint8_t stars_blink_state = 0;

    int h = 0, v = 0;
    bool vblank = false;
    bool vblank_edge = false;

    void reset();
    void advance(int cpu_cycles);
    void render(uint32_t* upright_224x256) const;

    uint8_t tile_pixel(uint8_t code, int x, int y) const;
    uint8_t sprite_pixel(uint8_t code, int x, int y) const;
    uint32_t prom_rgb(uint8_t pen) const;
    static uint8_t remap_color(uint8_t attr);

private:
    int blink_acc_ = 0;
    uint8_t pixel_at(int nx, int ny) const;
    uint8_t scroll_y(uint8_t v) const;
    uint8_t color_group(uint8_t attr) const;
    uint32_t backdrop(int nx, int ny) const;
    uint32_t bullet_at(int nx, int ny) const;
};

}  // namespace galaxian

#endif
