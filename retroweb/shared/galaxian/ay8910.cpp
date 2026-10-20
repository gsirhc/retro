#include "ay8910.h"

#include <cmath>

namespace galaxian {

namespace {

// Unused register bits read back as 0 (data sheet register array).
constexpr uint8_t kMask[16] = {0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF,
                               0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF};

int period(int p) { return p ? p : 1; }

}  // namespace

void Ay8910::reset() {
    regs.fill(0);
    addr = 0;
    host_acc_ = 0;
    host_sum_ = 0;
    host_n_ = 0;
    for (int c = 0; c < 3; c++) {
        tone_cnt_[c] = 0;
        tone_out_[c] = false;
    }
    noise_cnt_ = 0;
    noise_sr_ = 1;
    restart_envelope();
}

void Ay8910::write_addr(uint8_t v) { addr = uint8_t(v & 0x0F); }

void Ay8910::write_data(uint8_t v) {
    regs[addr] = uint8_t(v & kMask[addr]);
    if (addr == 13) restart_envelope();
}

uint8_t Ay8910::read_data() const {
    // R7 B6/B7 low make IOA/IOB inputs.
    if (addr == 14 && !(regs[7] & 0x40)) return port_a_r ? port_a_r() : 0xFF;
    if (addr == 15 && !(regs[7] & 0x80)) return port_b_r ? port_b_r() : 0xFF;
    return regs[addr];
}

void Ay8910::restart_envelope() {
    env_cnt_ = 0;
    env_step_ = 0;
    env_attack_ = (regs[13] & 0x04) != 0;
    env_done_ = false;
}

uint8_t Ay8910::env_level() const {
    const uint8_t shape = regs[13];
    if (env_done_) {
        if (!(shape & 0x08)) return 0;
        const bool attack = (shape & 0x04) != 0;
        const bool alternate = (shape & 0x02) != 0;
        return (attack != alternate) ? 15 : 0;
    }
    return uint8_t(env_attack_ ? env_step_ : 15 - env_step_);
}

void Ay8910::tick() {
    for (int c = 0; c < 3; c++) {
        const int tp = regs[unsigned(c * 2)] | ((regs[unsigned(c * 2 + 1)] & 0x0F) << 8);
        // Square wave of period 16 * TP: half a period is 8 * TP input clocks.
        if (++tone_cnt_[c] >= 8 * period(tp)) {
            tone_cnt_[c] = 0;
            tone_out_[c] = !tone_out_[c];
        }
    }
    if (++noise_cnt_ >= 16 * period(regs[6] & 0x1F)) {
        noise_cnt_ = 0;
        const uint32_t in = (noise_sr_ ^ (noise_sr_ >> 3)) & 1;
        noise_sr_ = (noise_sr_ >> 1) | (in << 16);
    }
    const int ep = regs[11] | (regs[12] << 8);
    // One of 16 envelope states per 256 * EP clocks.
    if (!env_done_ && ++env_cnt_ >= 16 * period(ep)) {
        env_cnt_ = 0;
        if (++env_step_ == 16) {
            env_step_ = 0;
            const uint8_t shape = regs[13];
            if (!(shape & 0x08) || (shape & 0x01)) env_done_ = true;
            else if (shape & 0x02) env_attack_ = !env_attack_;
        }
    }
}

uint8_t Ay8910::output_level(int ch) const {
    const uint8_t en = regs[7];
    const bool tone = tone_out_[ch] || (en & (1 << ch));
    const bool noise = (noise_sr_ & 1) || (en & (8 << ch));
    if (!(tone && noise)) return 0;
    const uint8_t amp = regs[unsigned(8 + ch)];
    return (amp & 0x10) ? env_level() : uint8_t(amp & 0x0F);
}

double Ay8910::dac_volts(uint8_t level) {
    if (level == 0) return 0.0;
    return std::pow(2.0, (int(level) - 15) / 2.0);
}

float Ay8910::mix() const {
    if (mute) return 0.0f;
    double s = 0;
    for (int ch = 0; ch < 3; ch++) s += dac_volts(output_level(ch));
    return float(s / 3.0);
}

void Ay8910::advance(int ay_cycles, int host_hz, std::vector<float>& out) {
    constexpr double kClock = 1789772.0;
    for (int i = 0; i < ay_cycles; i++) {
        tick();
        if (host_hz <= 0) continue;
        host_sum_ += mix();
        host_n_++;
        host_acc_ += double(host_hz);
        if (host_acc_ >= kClock) {
            host_acc_ -= kClock;
            out.push_back(float(host_sum_ / host_n_));
            host_sum_ = 0;
            host_n_ = 0;
        }
    }
}

}  // namespace galaxian
