// Atari Digital Vector Generator (DVG) — Asteroids / Lunar Lander family.
//
// Opcode formats and screen geometry from:
//   http://computerarcheology.com/Arcade/Asteroids/DVG.html
//   Atari Asteroids schematics (jmargolin vector generator notes)
// SVEC: Ss bit packing ((op&0x800)>>11)|((op&8)>>2); hardware remaps Ss to
// VEC local scale 2..5 and puts the 2-bit deltas in bits 9:8 (Asteroids HDL /
// jmargolin; VectorROM.html LABS0+SVEC ss2 x=3 → dx 24).
//
// The DVG reads shared vector memory as little-endian 16-bit words. The
// 6502 side sees the same bytes at $4000–$5FFF.

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

    // Word-addressed vector memory callback (0..0x1FFF words = 8K bytes).
    using MemRead = std::function<uint16_t(uint16_t word_addr)>;

    bool halt = true;
    int x = 0, y = 0;
    int scale = 0;  // global scale 0..15
    std::vector<VectorSeg> segments;

    void reset();
    // Pulse GO: clear halt and run until HALT (or kMaxOps).
    void go(const MemRead& read_word);
    // True while a list is in progress (CPU IN0 bit 2, active high HALT
    // flag inverted on the board — Machine maps polarity).
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
