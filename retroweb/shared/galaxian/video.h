// Galaxian-family video (Midway Galaxian A082-91352, Stern Scramble 33A-106, Gremlin/Sega Frogger 834-0068).

#ifndef GALAXIAN_VIDEO_H
#define GALAXIAN_VIDEO_H

#include <array>
#include <cstdint>

namespace galaxian {

constexpr int kHTotal = 384;  // H 128..511
constexpr int kVTotal = 264;  // V 248..511, VSYNC while 256V is low
constexpr int kVisW = 256;
constexpr int kVisH = 224;
constexpr int kVisY0 = 16;    // VBLANK clears on 16V at V = $110
constexpr int kUprightW = 224;
constexpr int kUprightH = 256;
constexpr int kCpuPerLine = kHTotal / 2;
constexpr int kCpuPerFrame = (kHTotal * kVTotal) / 2;
constexpr int kVBlankLine = kVisY0 + kVisH;  // VBLANK sets on 16V at V = $1F0
constexpr int kRiverSplit = 128;
constexpr int kCpuHz = 3072000;
// 555 astable Ra=100k, Rb=10k, C=10uF: 0.693*(Ra+2Rb)*C = 0.8316 s.
constexpr int kStarBlinkPeriod = 2554675;
constexpr int kStarPeriod = (1 << 17) - 1;

// Each gun: TTL outputs (3.4 V high, 0 V low) through their resistors into 470 ohm to ground.
constexpr double kTtlHigh = 3.4;
constexpr double kGunLoad = 1.0 / 470;
constexpr double gun_volts(double g_hi, double g_lo) {
    return kTtlHigh * g_hi / (g_hi + g_lo + kGunLoad);
}
// Full scale: the three PROM outputs of a red or green gun all high.
constexpr double kGunFullScale = gun_volts(1.0 / 220 + 1.0 / 470 + 1.0 / 1000, 0);
constexpr uint32_t gun_level(double volts) {
    const double v = volts / kGunFullScale * 255 + 0.5;
    return v >= 255 ? 255u : v <= 0 ? 0u : uint32_t(v);
}
constexpr uint32_t kRiverBlue = gun_level(gun_volts(1.0 / 470, 0));       // Frogger LS126 93, 470 ohm
constexpr uint32_t kScrambleBgBlue = gun_level(gun_volts(1.0 / 390, 0));  // Scramble LS126 7H, 390 ohm

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
    // Star LFSR state when this frame began, or when STARS ON last went high.
    uint32_t star_origin = 0;

    void reset();
    // Moves the beam `cpu_cycles` T-states (2 pixels each), painting as it goes.
    void advance(int cpu_cycles);
    void set_stars_enable(bool on);
    bool stars_enable() const { return stars_enable_; }
    // Galaxian NOISE: inverted star LFSR stage 17, latched by 7474 2D on 2V
    // (Midway A082-91352-A000 page 22).
    bool noise() const { return noise_; }
    void render(uint32_t* upright_224x256) const;

    uint8_t tile_pixel(uint8_t code, int x, int y) const;
    uint8_t sprite_pixel(uint8_t code, int x, int y) const;
    uint32_t prom_rgb(uint8_t pen) const;
    static uint32_t star_rgb(uint8_t color6);
    static uint8_t remap_color(uint8_t attr);

private:
    struct Bullet {
        int load = -1;
        bool in_window = false;
        bool done = false;
    };

    bool stars_enable_ = false;
    bool noise_ = true;
    bool star_drop_ = true;
    int star_base_ = 0;
    int blink_acc_ = 0;
    std::array<uint32_t, kVisW * kVisH> frame_{};
    std::array<uint8_t, 256> line_buf_{};  // sprite pen | group << 2, 0 = empty
    Bullet shell_;
    Bullet missile_;
    std::array<uint32_t, 32 * 3 * 2> rgb_prom_{};
    std::array<uint32_t, 65 * 3 * 2> rgb_dark_{};
    std::array<bool, 32 * 3 * 2> rgb_prom_ok_{};
    std::array<bool, 65 * 3 * 2> rgb_dark_ok_{};
    std::array<uint8_t, 32> prom_seen_{};
    Board board_seen_ = Board::Frogger;

    void line_start();
    void object_slot(int slot);
    void paint(int x);
    uint8_t scroll_of(uint8_t value) const;
    uint8_t color_group(uint8_t attr) const;
    int star_at(int x, int y) const;
    bool bullet_hit(Bullet& b, int x, int lo, int hi);
    uint32_t mix(int prom_index, int star, int bullet, bool backdrop);
    void check_palette();
};

}  // namespace galaxian

#endif
