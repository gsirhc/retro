// Atari analog vector generator, Battlezone board (DP-156 sheet 3A, STATE2.MAC, VGMC.MAC).

#ifndef SHARED_ATARI_AVG_H
#define SHARED_ATARI_AVG_H

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace atari {

// Picture coordinates, y down, origin at the top left of the tube.
struct AvgSeg {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    uint8_t z = 0;  // 1..15
};

class Avg {
public:
    // SCAL 1 VCTR 512,384 reaches the tube corner (BZSADG.MAC WTBL), so full
    // size spans 512 x 384 vector units.
    static constexpr int kWidth = 512;
    static constexpr int kHeight = 384;
    // L8 gates the state clock with VGCK, 3MHZ and 6MHZ: one state per 8 clocks of 12 MHz.
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

    // Advance by CPU cycles; beam time is counted in 12 MHz clocks.
    void run(int cpu_cycles, const MemRead& read);

    // Beam position in full-size vector units from the centre, y up.
    int x() const;
    int y() const;
    uint8_t scale() const { return lin_; }
    uint8_t bin_scale() const { return bin_; }
    uint8_t intensity() const { return zstat_; }
    uint16_t pc() const { return pc_; }

private:
    int step(const MemRead& read);
    int strobe3();
    int beam(int dx, int dy, int timer_bits, bool center);
    void draw(double x0, double y0, double x1, double y1, uint8_t z);
    void emit(double x0, double y0, double x1, double y1, uint8_t z);

    bool halt_ = true;
    uint8_t state_ = 0;
    uint8_t op_ = 0;
    uint8_t hi1_ = 0, lo1_ = 0, hi2_ = 0, lo2_ = 0;
    uint16_t pc_ = 0;
    bool odd_ = true;
    uint8_t sp_ = 0;
    std::array<uint16_t, 4> stack_{};
    uint8_t zstat_ = 0;
    uint8_t lin_ = 0;
    uint8_t bin_ = 0;
    double bx_ = 0, by_ = 0;
    double lo_x_ = -1e9, lo_y_ = -1e9, hi_x_ = 1e9, hi_y_ = 1e9;
    bool outside_ = false;
    int64_t budget_ = 0;

    std::vector<AvgSeg> building_;
};

}  // namespace atari

#endif
