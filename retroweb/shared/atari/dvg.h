// Atari Digital Vector Generator (Asteroids / Lunar Lander).
// Opcodes and geometry: computerarcheology.com/Arcade/Asteroids/DVG.html and
// the Atari Asteroids schematics (jmargolin notes).
// SVEC: Ss = ((op&0x800)>>11)|((op&8)>>2) remaps to VEC local scale 2..5, with
// the 2-bit deltas in bits 9:8 (Asteroids HDL; VectorROM LABS0+SVEC ss2 x=3 -> dx 24).
// Vector memory is little-endian 16-bit words, at $4000-$5FFF on the 6502 side.

#ifndef SHARED_ATARI_DVG_H
#define SHARED_ATARI_DVG_H

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace atari {

struct VectorSeg {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    uint8_t intensity = 0;  // 0 = move / blank, 1..15 draw
};

class Dvg {
public:
    static constexpr int kStackDepth = 4;
    static constexpr int kMaxOps = 100000;

    using MemRead = std::function<uint16_t(uint16_t word_addr)>;

    bool halt = true;
    int x = 0, y = 0;
    int scale = 0;  // global scale 0..15
    std::vector<VectorSeg> segments;

    void reset();
    void go(const MemRead& read_word);
    // CPU IN0 bit 2; Machine maps polarity
    bool halted() const { return halt; }

private:
    std::array<uint16_t, kStackDepth> stack_{};
    int sp_ = 0;
    uint16_t pc_ = 0;

    void run(const MemRead& read_word);
    void draw_delta(int dx, int dy, uint8_t intensity);
    static int apply_scale(int delta, int total_scale);
};

}  // namespace atari

#endif
