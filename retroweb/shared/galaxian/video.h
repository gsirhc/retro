// Galaxian-family video (Namco Galaxian 1979; Konami Frogger, Scramble 1981), MAME galaxian_v.cpp.

#ifndef GALAXIAN_VIDEO_H
#define GALAXIAN_VIDEO_H

#include <array>
#include <cstdint>

namespace galaxian {

constexpr int kHTotal = 384;
constexpr int kVTotal = 264;
constexpr int kVisW = 256;
constexpr int kVisH = 224;
constexpr int kVisY0 = 16;
constexpr int kUprightW = 224;
constexpr int kUprightH = 256;
constexpr int kCpuPerLine = kHTotal / 2;
constexpr int kCpuPerFrame = (kHTotal * kVTotal) / 2;
constexpr int kVBlankLine = kVisY0 + kVisH;
constexpr int kRiverSplit = 128;
constexpr uint32_t kRiverBlue = 0x000047;
constexpr uint32_t kScrambleBgBlue = 0x000056;
constexpr int kCpuHz = 3072000;
// 555 astable Ra=100k, Rb=10k, C=10uF: 0.693*(Ra+2Rb)*C = 0.8316 s.
constexpr int kStarBlinkPeriod = 2554675;
constexpr int kStarPeriod = (1 << 17) - 1;

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
    bool background_enable = false;
    uint8_t stars_blink_state = 0;

    int h = 0, v = 0;
    bool vblank = false;
    // Set when the beam enters vblank; the board clears it once handled.
    bool vblank_edge = false;
    uint32_t star_origin = 0;

    void reset();
    // Moves the beam `cpu_cycles` T-states (2 pixels each), painting as it goes.
    void advance(int cpu_cycles);
    void set_stars_enable(bool on);
    bool stars_enable() const { return stars_enable_; }
    void render(uint32_t* upright_224x256) const;

    uint8_t tile_pixel(uint8_t code, int x, int y) const;
    uint8_t sprite_pixel(uint8_t code, int x, int y) const;
    uint32_t prom_rgb(uint8_t pen) const;
    static uint32_t star_rgb(uint8_t color6);
    static uint8_t remap_color(uint8_t attr);

private:
    bool stars_enable_ = false;
    int blink_acc_ = 0;
    std::array<uint32_t, kVisW * kVisH> frame_{};
    std::array<uint8_t, kVisW> sprite_line_{};  // pen+1, 0 = empty
    int shell_x_ = -1;
    int missile_x_ = -1;

    void hblank_setup(int line);
    void paint(int line, int x0, int x1);
    uint8_t tile_pen(int x, int y) const;
    uint8_t scroll_y(uint8_t value) const;
    uint8_t color_group(uint8_t attr) const;
    uint32_t backdrop(int x, int y) const;
    uint32_t bullet_or(int x, uint32_t rgb) const;
};

}  // namespace galaxian

#endif
