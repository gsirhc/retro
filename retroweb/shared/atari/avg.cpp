#include "avg.h"

#include <algorithm>
#include <cmath>

namespace atari {

namespace {

// Next state by opcode and current state (STATE2.MAC, B2S129 at K7). X entries read as 0.
constexpr uint8_t kNext[8][16] = {
    {9, 0, 0, 0, 0, 0, 0, 0, 0xB, 8, 0xC, 0xA, 0xD, 0xF, 0, 0},  // VCTR
    {9, 0, 0, 0, 0, 0, 0, 0, 0xF, 8, 0, 0, 0, 0, 0, 0},          // HALT
    {9, 0, 0, 0, 0, 0, 0, 0, 0, 0xB, 0, 0xC, 0xD, 0xF, 0, 0},    // SVEC
    {9, 0, 1, 2, 0, 0, 0, 0, 0xE, 8, 0, 0, 0, 0, 3, 0},          // STAT / SCAL
    {9, 0, 0, 0, 0, 0, 0, 0, 0xC, 8, 0, 0, 0xF, 0, 0, 0},        // WAIT / CNTR
    {9, 0, 0, 0, 0, 0, 0, 0, 0xC, 8, 0, 0, 0xD, 0xE, 9, 0},      // JSRL
    {9, 0, 0, 0, 0, 0, 0, 0, 0xD, 8, 0, 0, 0, 0xE, 9, 0},        // RTSL
    {9, 0, 0, 0, 0, 0, 0, 0, 0xE, 8, 0, 0, 0, 0, 9, 0},          // JMPL
};

int sext13(int v) { return (v & 0x1000) ? (v | ~0x1FFF) : (v & 0x1FFF); }

bool normalized(int v) { return ((v >> 12) ^ (v >> 11)) & 1; }

}  // namespace

void Avg::reset() {
    vgrst();
    pc_ = 0;
    odd_ = true;
    bx_ = by_ = 0;
    lo_x_ = lo_y_ = -1e9;
    hi_x_ = hi_y_ = 1e9;
    outside_ = false;
    stack_.fill(0);
    building_.clear();
    frame.clear();
    frames = 0;
}

// DISRST presets the halt latch, clears F8, F9 and the linear scale, loads J6 with 0.
void Avg::vgrst() {
    halt_ = true;
    state_ = 0;
    zstat_ = 0;
    bin_ = 0;
    lin_ = 0;
    sp_ = 0;
    budget_ = 0;
}

// VGGO clears the halt latch and the H5/F5/E5 program counter.
void Avg::go() {
    halt_ = false;
    state_ = 0;
    pc_ = 0;
    odd_ = true;
    budget_ = 0;
    building_.clear();
}

int Avg::x() const { return int(std::lround(bx_)); }
int Avg::y() const { return int(std::lround(by_)); }

void Avg::run(int cpu_cycles, const MemRead& read) {
    if (halt_) {
        budget_ = 0;
        return;
    }
    budget_ += int64_t(cpu_cycles) * kMasterPerState;
    while (budget_ > 0 && !halt_) budget_ -= step(read);
    if (halt_) budget_ = 0;
}

// One state of K7/L7; LATCH0-3 and STROBE0-3 are J7 outputs for states 8-F.
int Avg::step(const MemRead& read) {
    int clocks = kMasterPerState;
    uint8_t s = state_;
    if (s >= 8) {
        switch (s & 7) {
        case 0: case 1: case 2: case 3: {
            uint8_t b = read(uint16_t(base + pc_ * 2 + (odd_ ? 1 : 0)));
            if (!odd_) pc_ = (pc_ + 1) & 0xFFF;
            odd_ = !odd_;
            if (s == 9) {
                hi1_ = b;
                op_ = b >> 5;
                lo1_ = hi2_ = lo2_ = 0;  // LATCH1 clears A5, A6, B5, D5, D6 and J8
            } else if (s == 8) {
                lo1_ = b;
            } else if (s == 0xB) {
                hi2_ = b;
            } else {
                lo2_ = b;
            }
            break;
        }
        case 4:
            if (op_ & 1) stack_[sp_] = pc_;  // GW = /OP0 + /STROBE0
            break;
        case 5:
            if (op_ & 4) sp_ = (op_ & 2) ? ((sp_ - 1) & 3) : ((sp_ + 1) & 3);  // J6, D/U = OP1
            break;
        case 6:
            if (op_ & 4) {
                pc_ = (op_ & 1) ? uint16_t(((hi1_ & 0xF) << 8) | lo1_) : stack_[sp_];
            } else if (op_ == 3) {
                if (hi1_ & 0x10) {
                    bin_ = hi1_ & 7;  // SCALELD into F9
                    lin_ = lo1_;
                } else {
                    zstat_ = lo1_ >> 4;  // STATCLK into F8
                    if (!(hi1_ & 0x04)) {  // H10 and J10 clock on STATCLK + DVY10
                        if (hi1_ & 0x02) {
                            hi_x_ = bx_;
                            hi_y_ = by_;
                        } else {
                            lo_x_ = bx_;
                            lo_y_ = by_;
                        }
                        outside_ = hi1_ & 0x01;
                    }
                }
            }
            break;
        case 7:
            clocks += strobe3();
            break;
        }
    }
    state_ = halt_ ? 0 : kNext[op_][s];
    return clocks;
}

// M9 latches OP0 as HALT; VCTR, SVEC and CNTR start the vector timer.
int Avg::strobe3() {
    halt_ = op_ & 1;
    if (halt_) {
        frame.swap(building_);
        building_.clear();
        frames++;
        return 0;
    }
    int dy = sext13(((hi1_ & 0x1F) << 8) | lo1_);
    int dx = sext13(((hi2_ & 0x1F) << 8) | lo2_);
    switch (op_) {
    case 0: return beam(dx, dy, 15, false);
    case 2: return beam(dx, dy, 8, false);  // N6, M6 held clear by OP1
    default: return beam(dx, dy, 15, true);
    }
}

// Shift X and Y left until either sign differs from bit 11 while the timer
// shifts right, then the timer runs; DAC gets DV bits 12-3 (DP-156 sheet 3A).
int Avg::beam(int dx, int dy, int timer_bits, bool center) {
    int shifts = 0;
    int nx = dx, ny = dy;
    while (shifts < timer_bits && !normalized(nx) && !normalized(ny)) {
        nx = sext13(nx << 1);
        ny = sext13(ny << 1);
        shifts++;
    }
    int left = timer_bits - shifts - bin_;
    int time = left > 0 ? (1 << left) : 1;
    int clocks = shifts + bin_ + time;
    if (center) {
        bx_ = by_ = 0;
        return clocks;
    }
    double k = double(time) / 32768.0 * (256 - lin_) / 256.0;
    double x1 = bx_ + double((nx >> 3) * 8) * k;
    double y1 = by_ + double((ny >> 3) * 8) * k;
    uint8_t z = hi2_ >> 5;
    // H8 selects the STAT level when Z = 1, else Z drives the top three resistors (sheet 2B).
    uint8_t level = z == 1 ? zstat_ : uint8_t(z << 1);
    if (level) draw(bx_, by_, x1, y1, level);
    bx_ = x1;
    by_ = y1;
    return clocks;
}

// B7 XORs the window comparators with J10 (IN), so IN = 1 lights only outside.
void Avg::draw(double x0, double y0, double x1, double y1, uint8_t z) {
    double t0 = 0, t1 = 1;
    double dx = x1 - x0, dy = y1 - y0;
    auto clip = [&](double p, double q) {
        if (p == 0) return q >= 0;
        double r = q / p;
        if (p < 0) {
            if (r > t1) return false;
            t0 = std::max(t0, r);
        } else {
            if (r < t0) return false;
            t1 = std::min(t1, r);
        }
        return true;
    };
    bool hit = clip(-dx, x0 - lo_x_) && clip(dx, hi_x_ - x0) &&
               clip(-dy, y0 - lo_y_) && clip(dy, hi_y_ - y0) && t0 <= t1;
    auto at = [&](double t, double& px, double& py) {
        px = x0 + dx * t;
        py = y0 + dy * t;
    };
    double ax, ay, bx, by;
    if (!outside_) {
        if (!hit) return;
        at(t0, ax, ay);
        at(t1, bx, by);
        emit(ax, ay, bx, by, z);
        return;
    }
    if (!hit) {
        emit(x0, y0, x1, y1, z);
        return;
    }
    if (t0 > 0) {
        at(t0, bx, by);
        emit(x0, y0, bx, by, z);
    }
    if (t1 < 1) {
        at(t1, ax, ay);
        emit(ax, ay, x1, y1, z);
    }
}

void Avg::emit(double x0, double y0, double x1, double y1, uint8_t z) {
    constexpr double cx = kWidth / 2.0, cy = kHeight / 2.0;
    building_.push_back({float(cx + x0), float(cy - y0), float(cx + x1), float(cy - y1), z});
}

}  // namespace atari
