// Midway/Namco Galaga video board: 8×8 tiles, 64 sprites, 05XX starfield.
//
// 384×264 raster at 6.144 MHz, visible 288×224, rotated to 224×288.
// Tile fetch follows greyrogue's galaga.vhd. Sprite bytes sit at the top of the
// three work-RAM banks ($8B80/$9380/$9B80).

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
    // Sprite registers live in the three work-RAM banks; the board points these at them.
    const uint8_t* ram1 = nullptr;
    const uint8_t* ram2 = nullptr;
    const uint8_t* ram3 = nullptr;

    int h = 0, v = 0;
    bool vblank = false;
    // Edges stay set until the board clears them.
    bool vblank_edge = false;
    bool sound_nmi_edge = false;

    void reset();
    // Moves the beam `cpu_cycles` T-states (2 pixels each), painting as it goes.
    void advance(int cpu_cycles);
    void render(uint32_t* upright) const;

    uint8_t tile_pixel(uint8_t code, int x, int y) const;
    uint32_t prom_rgb(uint8_t idx) const;
    uint32_t star_rgb(uint8_t color6) const;
    uint16_t star_lfsr() const { return star_lfsr_; }

private:
    uint16_t star_lfsr_ = 0x7FFF;
    bool stars_on_ = false;
    int star_set_a_ = 0, star_set_b_ = 2;
    std::array<uint32_t, kVisW * kVisH> frame_{};
    std::array<uint32_t, kVisW> sprite_line_{};  // RGB | 1 << 24, 0 = empty
    std::array<uint32_t, kVisW> star_line_{};

    static uint16_t next_star_lfsr(uint16_t lfsr);
    void frame_start();
    void hblank_setup(int line);
    void paint(int line, int x0, int x1);
};

}  // namespace galaga

#endif
