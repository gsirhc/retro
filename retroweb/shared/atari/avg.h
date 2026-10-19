// Atari AVG, Battlezone variant: STATE2.MAC sequencer, MAME avgdvg.cpp datapath.

#ifndef SHARED_ATARI_AVG_H
#define SHARED_ATARI_AVG_H

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace atari {

// Beam coordinates in 1/65536 of a picture unit; the picture is kWidth x
// kHeight units with y growing down, origin at the top left.
struct AvgSeg {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    uint8_t z = 0;  // 1..15
};

class Avg {
public:
    static constexpr int kWidth = 580;   // MAME bzone visarea
    static constexpr int kHeight = 400;
    static constexpr int kMasterPerState = 8;

    // Reads one byte of the CPU address space, as the AVG shares the bus.
    using MemRead = std::function<uint8_t(uint16_t addr)>;

    uint16_t base = 0x2000;  // vector memory origin in CPU space
    std::vector<AvgSeg> frame;  // last finished picture
    int frames = 0;

    void reset();
    void vgrst();
    void go();
    bool halted() const { return halt_; }

    // Advance by CPU cycles (1 state per CPU cycle, beam time in master clocks).
    void run(int cpu_cycles, const MemRead& read);

    int x() const { return xpos_; }
    int y() const { return ypos_; }
    uint8_t scale() const { return scale_; }
    uint8_t bin_scale() const { return bin_scale_; }
    uint8_t intensity() const { return intensity_; }

private:
    bool op0() const { return op_ & 1; }
    bool op1() const { return op_ & 2; }
    bool op2() const { return op_ & 4; }

    int step(const MemRead& read);
    void latch1();
    void strobe0();
    void strobe1();
    void strobe2();
    int strobe3();
    void emit(int x0, int y0, int x1, int y1, uint8_t z);
    void publish();

    bool halt_ = true;
    uint8_t state_ = 0;
    uint8_t op_ = 0;
    uint8_t data_ = 0;
    uint16_t pc_ = 0;
    uint8_t sp_ = 0;
    std::array<uint16_t, 4> stack_{};
    int dvx_ = 0, dvy_ = 0;
    int dvy12_ = 0;
    int timer_ = 0;
    uint8_t int_latch_ = 0;
    uint8_t scale_ = 0;
    uint8_t bin_scale_ = 0;
    uint8_t intensity_ = 0;
    int xpos_ = 0, ypos_ = 0;
    int64_t budget_ = 0;

    bool hst_ = true, lst_ = true;
    int clip_x0_ = 0, clip_y0_ = 0, clip_x1_ = 0, clip_y1_ = 0;
    bool clipping_ = false;
    int win_x0_ = 0, win_y0_ = 0, win_x1_ = 0, win_y1_ = 0;

    std::vector<AvgSeg> building_;
};

}  // namespace atari

#endif
