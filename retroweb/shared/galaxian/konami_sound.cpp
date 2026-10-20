#include "konami_sound.h"

#include <algorithm>

namespace galaxian {

namespace {

constexpr double kClock = 1789772.0;
constexpr int kStepClocks = 8;
constexpr double kH = kStepClocks / kClock;
constexpr double kR1 = 1000.0;
constexpr double kR2 = 5100.0;
constexpr double kCap[2] = {0.22e-6, 0.047e-6};
// CD4066B rON, VDD = 5 V, 25 C typical (TI CD4066B data sheet).
constexpr double kRon = 470.0;
// LM741 swings to within 2 V of each +/-5 V rail (TI LM741: +/-13 V at +/-15 V, RL >= 2k).
constexpr double kSwing = 3.0;
// 4.7k, then 0.15 uF into the M51516L, whose input is taken as 100k.
constexpr double kRc = 4700.0 + 100000.0;
constexpr double kCc = 0.15e-6;

}  // namespace

void KonamiSound::reset() {
    ch_ = {};
    sw_ = {};
    vout_ = 0;
    coupling_v_ = 0;
    out_ = 0;
    phase_ = 0;
    host_acc_ = 0;
    sum_ = 0;
    n_ = 0;
}

void KonamiSound::filter_w(uint16_t offset) {
    for (int n = 0; n < 6; n++) sw_[unsigned(n)] = (offset >> (6 + n)) & 1;
    for (int n = 0; n < 6; n++) sw_[unsigned(6 + n)] = (offset >> n) & 1;
}

float KonamiSound::sample() const {
    if (mute) return 0.0f;
    // One full-level channel swings Rf / 6.1k volts; scale that to 0.5 peak to peak.
    return float(out_ * 0.5 * (kR1 + kR2) / feedback_ohms);
}

void KonamiSound::solve() {
    double i_sum = 0;
    for (int c = 0; c < 6; c++) {
        Ay8910* chip = ay[unsigned(c / 3)];
        if (!chip) continue;
        const double vs = chip->mute ? 0.0 : Ay8910::dac_volts(chip->output_level(c % 3));
        Channel& ch = ch_[unsigned(c)];
        // Backward Euler: each switched cap is a conductance g to its held voltage.
        double g_sum = 1.0 / kR1 + 1.0 / kR2;
        double i_src = vs / kR1;
        double g[2]{};
        for (int k = 0; k < 2; k++) {
            if (!sw_[unsigned(c * 2 + k)]) continue;
            const double a = kH / (kCap[k] * kRon);
            g[k] = 1.0 / (kRon * (1.0 + a));
            g_sum += g[k];
            i_src += g[k] * ch.vc[k];
        }
        const double vn = i_src / g_sum;
        for (int k = 0; k < 2; k++) {
            if (!g[k]) continue;
            const double a = kH / (kCap[k] * kRon);
            ch.vc[k] = (ch.vc[k] + a * vn) / (1.0 + a);
        }
        // Into the summer's virtual ground.
        i_sum += vn / kR2;
    }
    vout_ = std::clamp(-feedback_ohms * i_sum, -kSwing, kSwing);
    const double a = kH / (kRc * kCc);
    coupling_v_ = (coupling_v_ + a * vout_) / (1.0 + a);
    out_ = vout_ - coupling_v_;
}

void KonamiSound::advance(int ay_cycles, int host_hz, std::vector<float>& out) {
    for (int i = 0; i < ay_cycles; i++) {
        for (Ay8910* chip : ay)
            if (chip) chip->tick();
        if (++phase_ < kStepClocks) continue;
        phase_ = 0;
        solve();
        if (host_hz <= 0) continue;
        sum_ += sample();
        n_++;
        host_acc_ += double(host_hz) * kStepClocks;
        if (host_acc_ >= kClock) {
            host_acc_ -= kClock;
            out.push_back(float(sum_ / n_));
            sum_ = 0;
            n_ = 0;
        }
    }
}

}  // namespace galaxian
