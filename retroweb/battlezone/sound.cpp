#include "sound.h"

#include <algorithm>
#include <cmath>

namespace battlezone {

namespace {

constexpr double kTtl = 3.4;
constexpr double kRon = 270;  // CD4066
constexpr double kNoiseHz = 12000.0 / 2;  // 12 kHz halved by B4 74LS109

constexpr double k(double v) { return v * 1e3; }
constexpr double u(double v) { return v * 1e-6; }

constexpr double kR5 = k(1), kR6 = k(4.7), kR7 = k(1), kR8 = k(100), kR9 = k(22);
constexpr double kR10 = k(100), kR11 = k(49);  // R11 engine pot at MAME's 40%
constexpr double kR12 = k(33), kR13 = k(10), kR14 = k(22), kR15 = k(1);
constexpr double kR16 = k(1), kR17 = k(22), kR18 = k(10), kR19 = k(33);
constexpr double kR20 = k(33), kR25 = k(100), kR26 = k(33), kR27 = k(330);
constexpr double kR28 = k(100), kR29 = k(22), kR32 = k(330), kR33 = k(330);
constexpr double kC9 = u(4.7), kC11 = u(0.015), kC13 = u(10), kC14 = u(10);
constexpr double kC20 = u(0.1), kC21 = u(0.0047), kC22 = u(0.0047), kC29 = u(0.47);

double par(double a, double b) { return a * b / (a + b); }
double divider(double r1, double r2) { return r2 / (r1 + r2); }

}  // namespace

void Sound::reset() {
    latch = 0;
    lfsr_ = 0;
    noise_phase_ = 0;
    noise_ = div31_ = nand33_ = div34_ = false;
    shell_cap_ = shell_out_ = expl_cap_ = expl_out_ = 0;
    eng_cv_ = 0;
    vco_cap_ = 0;
    vco_out_ = true;
    f4_ = 4;
    f5_ = 6;
    eng_mix_ = 0;
    hp_in_ = hp_out_ = 0;
}

void Sound::set_rate(int hz) {
    hz_ = std::max(1, hz);
    dt_ = 1.0 / hz_;
}

void Sound::clock_noise() {
    // 16-bit LS164 chain: taps QC of the second (bit 3) and QH of the third (bit 14).
    uint32_t fb = (lfsr_ >> 16) & 1;
    fb ^= 1;
    lfsr_ = ((lfsr_ << 1) & 0xFFFE) | fb;
    uint32_t f0 = ((lfsr_ >> 3) ^ (lfsr_ >> 14)) & 1;
    lfsr_ |= f0 << 16;

    bool out = (lfsr_ >> 15) & 1;
    if (out && !noise_) div31_ = !div31_;
    noise_ = out;
    bool nand = ((lfsr_ >> 11) & 0xF) != 0xF;
    if (nand && !nand33_) div34_ = !div34_;
    nand33_ = nand;
}

// MAME DST_RC_CIRCUIT_1: the enable charges the cap through a 4066, the gate
// discharges it through R and passes it to the output.
double Sound::rc_circuit(double& cap, bool charge, bool gate, double r, double c) const {
    const double drop = divider(kRon, kRon + r);
    if (!charge) {
        if (!gate) return 0;
        cap -= cap * (1 - std::exp(-dt_ / ((kRon + r) * c)));
        return cap * drop;
    }
    if (!gate) {
        cap += (5.0 - cap) * (1 - std::exp(-dt_ / (kRon * c)));
        return 0;
    }
    cap += (5.0 * drop - cap) * (1 - std::exp(-dt_ / (par(kRon, kRon + r) * c)));
    return cap * drop;
}

// MAME bzone_custom_filter: 4066-switched gain op-amp, R5/C low-pass.
double Sound::filter(double& out, bool loud, double in, const Parts& p) const {
    const double r1 = p.r1, r2 = p.r2, r3 = p.r3, r4 = p.r4, r5 = p.r5, c = p.c;
    double gain = loud ? r5 / (par(kRon, r1) + r2) + 1 : r5 / (r1 + r2) + 1;
    double v = std::clamp(in * divider(r3, r4) * gain, 0.0, 22.0 - 1.5);
    out += (v - out) * (1 - std::exp(-dt_ / (r5 * c)));
    return out;
}

double Sound::engine_hz() const {
    // 555 astable with CV: charge Vc/2 to Vc through R10+R11, discharge through R11.
    double vc = std::clamp(eng_cv_, 0.05, 4.9);
    double th = (kR10 + kR11) * kC11 * std::log((5.0 - vc / 2) / (5.0 - vc));
    double tl = kR11 * kC11 * std::log(2.0);
    return 1.0 / (th + tl);
}

void Sound::step_vco() {
    double vc = std::clamp(eng_cv_, 0.05, 4.9);
    if (vco_out_) {
        vco_cap_ += (5.0 - vco_cap_) * (1 - std::exp(-dt_ / ((kR10 + kR11) * kC11)));
        if (vco_cap_ >= vc) vco_out_ = false;
    } else {
        vco_cap_ -= vco_cap_ * (1 - std::exp(-dt_ / (kR11 * kC11)));
        if (vco_cap_ <= vc / 2) {
            vco_out_ = true;
            // F4 counts 4..15, F5 6..15, on the 555's rising edge.
            if (latch & 0x80) {
                f4_ = f4_ >= 15 ? 4 : f4_ + 1;
                f5_ = f5_ >= 15 ? 6 : f5_ + 1;
            }
        }
    }
    if (!(latch & 0x80)) {
        f4_ = 0;
        f5_ = 0;
    }
}

float Sound::sample(float pokey) {
    noise_phase_ += kNoiseHz * dt_;
    while (noise_phase_ >= 1.0) {
        noise_phase_ -= 1.0;
        clock_noise();
    }

    // D2 shell, D3 shell loud (game LOUDSH=8); D0 explosion, D1 explosion loud.
    double shell = rc_circuit(shell_cap_, latch & 0x04, div31_, kR14 + kR15, kC9);
    shell = filter(shell_out_, latch & 0x08, shell, {kR12, kR13, kR14, kR15, kR32, kC21});
    double expl = rc_circuit(expl_cap_, latch & 0x01, div34_, kR17 + kR16, kC14);
    expl = filter(expl_out_, latch & 0x02, expl, {kR19, kR18, kR17, kR16, kR33, kC22});

    // D4 engine rev pulls the 555 CV down through R5 and the 4066.
    double target = (latch & 0x10) ? 5.0 * divider(kR7, par(kRon + kR5, kR6))
                                   : 5.0 * divider(kR7, kR6);
    double diff = target - eng_cv_;
    // RCDISC3, diode drop 0.5 V (MAME NODE_62).
    double tau = diff > 0.5 ? par(kR8, kR9) * kC13 : kR8 * kC13;
    eng_cv_ += diff * (1 - std::exp(-dt_ / tau));
    step_vco();
    double eng_in = ((f4_ > 7) + (f4_ == 15) + (f5_ > 7) + (f5_ == 15)) * kTtl / 4;
    eng_mix_ += (eng_in - eng_mix_) * (1 - std::exp(-dt_ / ((kR20 / 4) * kC29)));

    const double g[4] = {1 / kR25, 1 / kR28, 1 / (kR26 + kR20 / 4), 1 / kR27};
    const double sum = g[0] + g[1] + g[2] + g[3] + 1 / kR29;
    double out = (shell * g[0] + expl * g[1] + eng_mix_ * g[2] + double(pokey) * g[3]) / sum;
    // C20 couples the speaker; the source is every mixer leg in parallel.
    double rc = 1 / (1 / kR25 + 1 / kR28 + 1 / (kR26 + kR20 / 4) + 1 / kR27 + 1 / kR29) * kC20;
    double a = rc / (rc + dt_);
    hp_out_ = a * (hp_out_ + out - hp_in_);
    hp_in_ = out;
    // D5 mutes the output, past C20 (MAME system_mute), so it doesn't pop.
    if (!(latch & 0x20)) return 0.0f;
    return float(std::clamp(hp_out_ * 0.8, -1.0, 1.0));
}

}  // namespace battlezone
