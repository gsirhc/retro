#include "pit8253.h"

#include <algorithm>

namespace ibmpcat {

void Pit8253::reset() {
    for (auto &ch : ch_) ch = Channel{};
    pit_credit_ = 0.0;
    prev_cycles_ = 0;
}

void Pit8253::set_reload(int idx, uint16_t v) {
    Channel &ch = ch_[idx];
    ch.reload = v;
    ch.toggle_period = v ? std::max<uint16_t>(1, uint16_t(v / 2)) : 0x8000;
    ch.counter = ch.toggle_period;
    ch.armed = true;
    ch.output = true;
}

uint8_t Pit8253::in(uint16_t port) {
    if (port == 0x43) return 0xFF;  // control word register is write-only on real hardware
    Channel &ch = ch_[port - 0x40];
    if (ch.latched) {
        if (ch.access == 2) { ch.latched = false; return uint8_t(ch.latch_value >> 8); }
        if (ch.access == 1) { ch.latched = false; return uint8_t(ch.latch_value & 0xFF); }
        if (!ch.latch_msb_pending) { ch.latch_msb_pending = true; return uint8_t(ch.latch_value & 0xFF); }
        ch.latched = false;
        ch.latch_msb_pending = false;
        return uint8_t(ch.latch_value >> 8);
    }
    if (ch.access == 2) return uint8_t(ch.counter >> 8);
    if (ch.access == 1) return uint8_t(ch.counter & 0xFF);
    if (!ch.msb_pending) { ch.msb_pending = true; return uint8_t(ch.counter & 0xFF); }
    ch.msb_pending = false;
    return uint8_t(ch.counter >> 8);
}

void Pit8253::out(uint16_t port, uint8_t v) {
    if (port == 0x43) {
        int channel = (v >> 6) & 3;
        if (channel == 3) return;  // 8254 read-back command -- not modeled, unused by AT BIOS/DOS
        int rw = (v >> 4) & 3;
        Channel &ch = ch_[channel];
        if (rw == 0) {  // counter latch command
            ch.latched = true;
            ch.latch_value = ch.counter;
            ch.latch_msb_pending = false;
            return;
        }
        ch.access = rw;
        ch.msb_pending = false;
        ch.armed = false;  // real chip stops counting until a new count is loaded
        return;
    }
    int idx = port - 0x40;
    Channel &ch = ch_[idx];
    switch (ch.access) {
        case 1: set_reload(idx, v); break;
        case 2: set_reload(idx, uint16_t(uint16_t(v) << 8)); break;
        default:
            if (!ch.msb_pending) { ch.pending_lsb = v; ch.msb_pending = true; }
            else { set_reload(idx, uint16_t(uint16_t(ch.pending_lsb) | (uint16_t(v) << 8))); ch.msb_pending = false; }
            break;
    }
}

void Pit8253::set_gate2(bool level) {
    Channel &ch = ch_[2];
    bool was = ch.gate;
    ch.gate = level;
    if (!level) {
        // Gate low: force the output high immediately -- see pit8253.h.
        ch.output = true;
    } else if (!was) {
        // Gate's rising edge: reload the counter, restarting the square
        // wave cleanly -- see pit8253.h.
        ch.counter = ch.toggle_period;
    }
}

void Pit8253::step_channel(int idx) {
    Channel &ch = ch_[idx];
    ch.just_rose = false;
    if (!ch.armed) return;
    if (idx == 2 && !ch.gate) return;
    if (ch.counter == 0) ch.counter = ch.toggle_period;
    --ch.counter;
    if (ch.counter == 0) {
        bool was = ch.output;
        ch.output = !ch.output;
        if (!was && ch.output) ch.just_rose = true;
    }
}

int Pit8253::tick(uint64_t cpu_cycles, double cpu_hz) {
    uint64_t d = cpu_cycles - prev_cycles_;
    prev_cycles_ = cpu_cycles;
    pit_credit_ += double(d) * (PIT_HZ / cpu_hz);
    int whole = int(pit_credit_);
    pit_credit_ -= whole;
    int ch0_rises = 0;
    for (int i = 0; i < whole; ++i) {
        for (int c = 0; c < 3; ++c) step_channel(c);
        if (ch_[0].just_rose) ++ch0_rises;
    }
    return ch0_rises;
}

}  // namespace ibmpcat
