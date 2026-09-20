#include "ay8910.h"

#include <algorithm>
#include <cmath>

namespace frogger {
namespace {

// 16-level AY DAC, relative to full scale. Datasheet resistor ladder
// (normalized); not a linear 4-bit scale.
constexpr float kVol[16] = {
    0.0000f, 0.0078f, 0.0110f, 0.0156f, 0.0221f, 0.0312f, 0.0441f, 0.0624f,
    0.0883f, 0.1249f, 0.1766f, 0.2498f, 0.3534f, 0.4998f, 0.7071f, 1.0000f,
};

int tone_period(const std::array<uint8_t, 16>& r, int ch) {
    int p = r[unsigned(ch * 2)] | (int(r[unsigned(ch * 2 + 1)] & 0x0F) << 8);
    return p == 0 ? 1 : p;
}

}  // namespace

void Ay8910::reset() {
    regs.fill(0);
    addr = 0;
    mute = false;
    ay_cycle_ = 0;
    host_acc_ = 0;
    for (int i = 0; i < 3; i++) {
        tone_cnt_[i] = 0;
        tone_out_[i] = false;
    }
    noise_cnt_ = 0;
    noise_lfsr_ = 1;
    noise_out_ = false;
    env_cnt_ = 0;
    env_pos_ = 0;
    env_step_ = 1;
    env_hold_ = false;
}

void Ay8910::write_addr(uint8_t v) { addr = uint8_t(v & 0x0F); }

void Ay8910::write_data(uint8_t v) {
    // Period / mixer / amplitude coarse masks match the datasheet.
    static constexpr uint8_t kMask[16] = {
        0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF,
        0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF,
    };
    regs[addr] = uint8_t(v & kMask[addr]);
    if (addr == 13) restart_envelope();
}

uint8_t Ay8910::read_data() const {
    if (addr == 14 && port_a_r) return port_a_r();
    if (addr == 15 && port_b_r) return port_b_r();
    return regs[addr];
}

void Ay8910::restart_envelope() {
    env_cnt_ = 0;
    env_hold_ = false;
    uint8_t shape = regs[13];
    // Continue bit: if clear, the envelope runs once then holds 0
    // (or 15 if alternate+hold). Attack starts at 0 going up, else 15 down.
    if (shape & 0x04) {
        env_pos_ = 0;
        env_step_ = 1;
    } else {
        env_pos_ = 15;
        env_step_ = -1;
    }
}

uint8_t Ay8910::env_level() const {
    return uint8_t(std::clamp(env_pos_, 0, 15));
}

void Ay8910::tick() {
    // Tone: period is in units of 16 AY clocks.
    if ((ay_cycle_ & 15) == 0) {
        for (int ch = 0; ch < 3; ch++) {
            if (++tone_cnt_[ch] >= tone_period(regs, ch)) {
                tone_cnt_[ch] = 0;
                tone_out_[ch] = !tone_out_[ch];
            }
        }
        int np = regs[6] & 0x1F;
        if (np == 0) np = 1;
        if (++noise_cnt_ >= np) {
            noise_cnt_ = 0;
            // 17-bit LFSR, tap bits 0 and 3 (GI AY-3-8910).
            uint32_t bit = (noise_lfsr_ ^ (noise_lfsr_ >> 3)) & 1;
            noise_lfsr_ = (noise_lfsr_ >> 1) | (bit << 16);
            noise_out_ = (noise_lfsr_ & 1) != 0;
        }

        int ep = regs[11] | (int(regs[12]) << 8);
        if (ep == 0) ep = 1;
        if (!env_hold_ && ++env_cnt_ >= ep) {
            env_cnt_ = 0;
            env_pos_ += env_step_;
            uint8_t shape = regs[13];
            if (env_pos_ < 0 || env_pos_ > 15) {
                if ((shape & 0x08) == 0) {
                    env_pos_ = 0;
                    env_hold_ = true;
                } else if (shape & 0x01) {  // hold
                    env_hold_ = true;
                    bool alt = (shape & 0x02) != 0;
                    bool att = (shape & 0x04) != 0;
                    env_pos_ = (att ^ alt) ? 0 : 15;
                } else if (shape & 0x02) {  // alternate
                    env_step_ = -env_step_;
                    env_pos_ += env_step_;
                } else {
                    env_pos_ = (shape & 0x04) ? 0 : 15;
                }
            }
        }
    }
    ay_cycle_++;
}

float Ay8910::mix() const {
    if (mute) return 0.0f;
    float s = 0;
    uint8_t mixer = regs[7];
    for (int ch = 0; ch < 3; ch++) {
        bool tone_off = (mixer & (1u << ch)) != 0;
        bool noise_off = (mixer & (1u << (ch + 3))) != 0;
        bool on = (tone_off || tone_out_[ch]) && (noise_off || noise_out_);
        uint8_t amp = regs[unsigned(8 + ch)];
        uint8_t level = (amp & 0x10) ? env_level() : uint8_t(amp & 0x0F);
        if (on) s += kVol[level];
    }
    return s / 3.0f;
}

void Ay8910::advance(int ay_cycles, int host_hz, std::vector<float>& out) {
    if (host_hz <= 0) {
        for (int i = 0; i < ay_cycles; i++) tick();
        return;
    }
    const double step = 1789772.0 / double(host_hz);
    for (int i = 0; i < ay_cycles; i++) {
        tick();
        host_acc_ += 1.0;
        while (host_acc_ >= step) {
            host_acc_ -= step;
            out.push_back(mix());
        }
    }
}

}  // namespace frogger
