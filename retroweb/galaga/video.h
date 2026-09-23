// Midway/Namco Galaga video board: 8×8 tiles, 64 sprites, 05XX starfield.
//
// Raster is 384×264 at 6.144 MHz (18.432 MHz / 3), same master clock as
// Pac-Man. Visible 288×224, upright monitor rotated 90° to 224×288.
// Tile fetch follows greyrogue's galaga.vhd (a cross-check). The center
// 32 columns are row-major into the 1K code / 1K color RAM. The two
// columns on each side are the hcnt bit 8 clear score strips.
// Sprite bytes sit at the top of the three work-RAM banks ($8B80/$9380/$9B80).

#ifndef GALAGA_VIDEO_H
#define GALAGA_VIDEO_H

#include <array>
#include <cstdint>

namespace galaga {

constexpr int kHTotal = 384;
constexpr int kVTotal = 264;
constexpr int kVisW = 288;
constexpr int kVisH = 224;
constexpr int kUprightW = 224;
constexpr int kUprightH = 288;
constexpr int kCpuPerLine = kHTotal / 2;
constexpr int kCpuPerFrame = (kHTotal * kVTotal) / 2;
constexpr int kVBlankLine = kVisH;
constexpr int kCpuHz = 3072000;

class Video {
public:
    std::array<uint8_t, 0x800> videoram{};
    std::array<uint8_t, 0x1000> tile_rom{};
    std::array<uint8_t, 0x2000> sprite_rom{};
    std::array<uint8_t, 32> palette{};
    std::array<uint8_t, 256> char_lut{};
    std::array<uint8_t, 256> sprite_lut{};
    std::array<uint8_t, 8> star_latch{};
    bool flip = false;

    int h = 0, v = 0;
    bool vblank = false;
    bool vblank_edge = false;
    bool sound_nmi_edge = false;

    void reset();
    void advance(int cpu_cycles);
    void render(uint32_t* upright, const uint8_t* ram1, const uint8_t* ram2,
                const uint8_t* ram3) const;

    uint8_t tile_pixel(uint8_t code, int x, int y) const;
    uint32_t prom_rgb(uint8_t idx) const;
    uint32_t star_rgb(uint8_t color6) const;

private:
    // 05XX LFSR advances once per frame during render (Hildinger RE).
    mutable uint16_t star_lfsr_ = 0x7FFF;

    static uint16_t next_star_lfsr(uint16_t lfsr);
    void draw_stars(uint32_t* native) const;
};

}  // namespace galaga

#endif
