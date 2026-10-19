#include "avg.h"

#include <algorithm>
#include <utility>

namespace atari {

namespace {

constexpr int kCenterX = (Avg::kWidth / 2) << 16;
constexpr int kCenterY = (Avg::kHeight / 2) << 16;
constexpr int kOpen = 0x5000000;

// Next state by [op][state]; don't-cares are 0 (STATE2.MAC).
constexpr uint8_t kNext[8][16] = {
    {9, 0, 0, 0, 0, 0, 0, 0, 0xB, 8, 0xC, 0xA, 0xD, 0xF, 0, 0},  // VCTR
    {9, 0, 0, 0, 0, 0, 0, 0, 0xF, 8, 0, 0, 0, 0, 0, 0},          // HALT
    {9, 0, 0, 0, 0, 0, 0, 0, 0, 0xB, 0, 0xC, 0xD, 0xF, 0, 0},    // SVEC
    {9, 0, 1, 2, 0, 0, 0, 0, 0xE, 8, 0, 0, 0, 0, 3, 0},          // STAT/SCAL
    {9, 0, 0, 0, 0, 0, 0, 0, 0xC, 8, 0, 0, 0xF, 0, 0, 0},        // CNTR
    {9, 0, 0, 0, 0, 0, 0, 0, 0xC, 8, 0, 0, 0xD, 0xE, 9, 0},      // JSRL
    {9, 0, 0, 0, 0, 0, 0, 0, 0xD, 8, 0, 0, 0, 0xE, 9, 0},        // RTSL
    {9, 0, 0, 0, 0, 0, 0, 0, 0xE, 8, 0, 0, 0, 0, 9, 0},          // JMPL
};

}  // namespace

void Avg::reset() {
    vgrst();
    pc_ = 0;
    sp_ = 0;
    stack_.fill(0);
    dvx_ = dvy_ = dvy12_ = 0;
    timer_ = 0;
    int_latch_ = 0;
    data_ = 0;
    op_ = 0;
    intensity_ = 0;
    xpos_ = kCenterX;
    ypos_ = kCenterY;
    hst_ = lst_ = true;
    clip_x0_ = clip_y0_ = clip_x1_ = clip_y1_ = 0;
    clipping_ = false;
    budget_ = 0;
    building_.clear();
    frame.clear();
    frames = 0;
}

void Avg::vgrst() {
    state_ = 0;
    bin_scale_ = 0;
    scale_ = 0;
    halt_ = true;
}

void Avg::go() {
    pc_ = 0;
    sp_ = 0;
    halt_ = false;
    clipping_ = false;
    building_.clear();
}

void Avg::run(int cpu_cycles, const MemRead& read) {
    if (halt_) {
        budget_ = 0;
        return;
    }
    budget_ += int64_t(cpu_cycles) * kMasterPerState;
    while (budget_ > 0 && !halt_)
        budget_ -= step(read);
    if (halt_) budget_ = 0;
}

int Avg::step(const MemRead& read) {
    int cost = kMasterPerState;
    state_ = halt_ ? 0 : kNext[op_ & 7][state_ & 0xF];
    if (state_ & 8) {
        data_ = read(uint16_t(base + (pc_ ^ 1)));
        switch (state_ & 7) {
            case 0:
                dvy_ = (dvy_ & 0x1F00) | data_;
                pc_++;
                break;
            case 1: latch1(); break;
            case 2:
                dvx_ = (dvx_ & 0x1F00) | data_;
                pc_++;
                break;
            case 3:
                int_latch_ = data_ >> 4;
                dvx_ = ((int_latch_ & 1) << 12) | ((data_ & 0xF) << 8) | (dvx_ & 0xFF);
                pc_++;
                break;
            case 4: strobe0(); break;
            case 5: strobe1(); break;
            case 6: strobe2(); break;
            case 7: cost += strobe3(); break;
        }
    }
    if (halt_) {
        state_ = 0;
        publish();
    }
    return cost;
}

void Avg::latch1() {
    // Battlezone clip: the beam position latches a window corner (MAME bzone_latch1).
    if (!hst_) {
        clip_x1_ = xpos_;
        clip_y0_ = ypos_;
    }
    if (!lst_) {
        clip_x0_ = xpos_;
        clip_y1_ = ypos_;
    }
    if (!lst_ || !hst_) {
        win_x0_ = std::min(clip_x0_, clip_x1_);
        win_x1_ = std::max(clip_x0_, clip_x1_);
        win_y0_ = std::min(clip_y0_, clip_y1_);
        win_y1_ = std::max(clip_y0_, clip_y1_);
        clipping_ = true;
    }
    lst_ = hst_ = true;

    dvy12_ = (data_ >> 4) & 1;
    op_ = data_ >> 5;
    int_latch_ = 0;
    dvy_ = (dvy12_ << 12) | ((data_ & 0xF) << 8);
    dvx_ = 0;
    pc_++;
}

void Avg::strobe0() {
    if (op0()) {
        stack_[sp_ & 3] = pc_;
        return;
    }
    // Normalize until bit 12 differs from bit 11; each shift halves the beam time.
    int i = 0;
    while ((((dvy_ ^ (dvy_ << 1)) & 0x1000) == 0) &&
           (((dvx_ ^ (dvx_ << 1)) & 0x1000) == 0) && (i++ < 16)) {
        dvy_ = (dvy_ & 0x1000) | ((dvy_ << 1) & 0x1FFF);
        dvx_ = (dvx_ & 0x1000) | ((dvx_ << 1) & 0x1FFF);
        timer_ >>= 1;
        timer_ |= 0x4000 | (op1() << 7);
    }
    if (op1()) timer_ &= 0xFF;
}

void Avg::strobe1() {
    if (!op2()) {
        for (int i = bin_scale_; i > 0; i--) {
            timer_ >>= 1;
            timer_ |= 0x4000 | (op1() << 7);
        }
        if (op1()) timer_ &= 0xFF;
        return;
    }
    if (op1()) sp_ = uint8_t((sp_ - 1) & 0xF);
    else sp_ = uint8_t((sp_ + 1) & 0xF);
}

void Avg::strobe2() {
    if (op2()) {
        if (op0()) pc_ = uint16_t(dvy_ << 1);
        else pc_ = stack_[sp_ & 3];
        return;
    }
    if (dvy12_) {
        scale_ = uint8_t(dvy_ & 0xFF);
        bin_scale_ = uint8_t((dvy_ >> 8) & 7);
        return;
    }
    // STAT: D7-D4 intensity; D10=0 loads the clip switches from D9 (MAME bzone_strobe2).
    intensity_ = uint8_t((dvy_ >> 4) & 0xF);
    if (!(dvy_ & 0x400)) {
        lst_ = (dvy_ & 0x200) != 0;
        hst_ = !lst_;
    }
}

int Avg::strobe3() {
    int cycles = 0;
    halt_ = op0();
    if (!op0() && !op2()) {
        cycles = op1() ? 0x100 - (timer_ & 0xFF) : 0x8000 - timer_;
        timer_ = 0;
        // 10-bit DACs see bits 12-3 of the counters; scale is inverted.
        int64_t dx = int64_t(((dvx_ >> 3) ^ 0x200) - 0x200) * cycles * (scale_ ^ 0xFF);
        int64_t dy = int64_t(((dvy_ >> 3) ^ 0x200) - 0x200) * cycles * (scale_ ^ 0xFF);
        int x0 = xpos_, y0 = ypos_;
        xpos_ += int(dx >> 4);
        ypos_ -= int(dy >> 4);
        uint8_t z = (int_latch_ >> 1) == 1 ? intensity_ : uint8_t(int_latch_ & 0xE);
        if (z) emit(x0, y0, xpos_, ypos_, z);
    }
    if (op2()) {
        cycles = 0x8000 - timer_;
        timer_ = 0;
        xpos_ = kCenterX;
        ypos_ = kCenterY;
    }
    return cycles;
}

void Avg::emit(int x0, int y0, int x1, int y1, uint8_t z) {
    double ax = x0, ay = y0, bx = x1, by = y1;
    double lo_x = clipping_ ? win_x0_ : -kOpen, hi_x = clipping_ ? win_x1_ : kOpen;
    double lo_y = clipping_ ? win_y0_ : -kOpen, hi_y = clipping_ ? win_y1_ : kOpen;
    double t0 = 0, t1 = 1, dx = bx - ax, dy = by - ay;
    auto edge = [&](double p, double q) {
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
    if (!edge(-dx, ax - lo_x) || !edge(dx, hi_x - ax) ||
        !edge(-dy, ay - lo_y) || !edge(dy, hi_y - ay))
        return;
    constexpr double k = 1.0 / 65536.0;
    building_.push_back(AvgSeg{float((ax + t0 * dx) * k), float((ay + t0 * dy) * k),
                               float((ax + t1 * dx) * k), float((ay + t1 * dy) * k), z});
}

void Avg::publish() {
    frame.swap(building_);
    building_.clear();
    frames++;
}

}  // namespace atari
