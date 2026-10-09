#include "konami_sound.h"

#include <algorithm>

namespace galaxian {
namespace {

constexpr double kAyHz = 1789772.0;
constexpr int kSolveClocks = 8;
constexpr double kDt = kSolveClocks / kAyHz;

constexpr double kRi = 1000.0;
constexpr double kRo = 5000.0;
constexpr double kRSwitch = 270.0;
constexpr double kCap[2] = {0.22e-6, 0.047e-6};

constexpr double kRFeedback = 2200.0;
constexpr double kR2 = 4700.0;
constexpr double kVr = 200.0;
constexpr double kCCouple = 0.15e-6;
constexpr double kRAmpIn = 100000.0;
// uA741 on +/-5 V swings to about 1 V inside each rail (TI uA741 V_OM).
constexpr double kOpAmpSwing = 4.0;

}  // namespace

void KonamiSound::reset() {
    ch_ = {};
    sw_.fill(false);
    coupling_v_ = 0;
    out_ = 0;
    phase_ = 0;
    host_acc_ = 0;
    sum_ = 0;
    n_ = 0;
    mute = false;
}

void KonamiSound::filter_w(uint16_t offset) {
    // AV6-AV11 drive 3D's switches, AV0-AV5 3C's (MAME konami_sound_filter_w).
    for (int which = 0; which < 2; which++)
        for (int flt = 0; flt < 6; flt++)
            sw_[unsigned(which * 6 + flt)] = ((offset >> (flt + 6 * (1 - which))) & 1) != 0;
}

void KonamiSound::solve() {
    // Backward Euler on each channel node: AY source to 5 V, 1k, switched caps, 5k into the virtual ground.
    double i_sum = 0;
    for (int which = 0; which < 2; which++) {
        if (!ay[unsigned(which)]) continue;
        for (int c = 0; c < 3; c++) {
            Channel& ch = ch_[unsigned(which * 3 + c)];
            double gs = 1.0 / (Ay8910::output_resistance(ay[unsigned(which)]->output_level(c)) + kRi);
            double num = 5.0 * gs;
            double den = gs + 1.0 / kRo;
            double a[2] = {}, b[2] = {};
            for (int k = 0; k < 2; k++) {
                if (!sw_[unsigned(which * 6 + c * 2 + k)]) continue;
                double g = 1.0 / kRSwitch;
                double kc = kCap[k] / kDt;
                a[k] = kc / (kc + g);
                b[k] = g / (kc + g);
                num += g * a[k] * ch.vc[k];
                den += g * (1.0 - b[k]);
            }
            double vn = num / den;
            for (int k = 0; k < 2; k++)
                if (sw_[unsigned(which * 6 + c * 2 + k)]) ch.vc[k] = a[k] * ch.vc[k] + b[k] * vn;
            i_sum += vn / kRo;
        }
    }
    double vamp = std::clamp(-kRFeedback * i_sum, -kOpAmpSwing, kOpAmpSwing);

    // R2 into the 200 ohm volume pot, then 0.15 uF into the M51516L's ~100k input.
    double kc = kCCouple / kDt;
    double gx = 1.0 / kR2 + 1.0 / kVr;
    double u0 = coupling_v_;
    double u = (vamp / kR2 + u0 * (kRAmpIn * kc * gx + kc)) / ((1.0 + kRAmpIn * kc) * gx + kc);
    out_ = kRAmpIn * kc * (u - u0);
    coupling_v_ = u;
}

void KonamiSound::advance(int ay_cycles, int host_hz, std::vector<float>& out) {
    const double step = host_hz > 0 ? kAyHz / double(host_hz) : 0.0;
    for (int i = 0; i < ay_cycles; i++) {
        for (Ay8910* a : ay)
            if (a) a->tick();
        if (++phase_ == kSolveClocks) {
            phase_ = 0;
            solve();
            sum_ += sample();
            n_++;
        }
        if (host_hz <= 0) continue;
        host_acc_ += 1.0;
        if (host_acc_ >= step) {
            host_acc_ -= step;
            out.push_back(n_ ? float(sum_ / n_) : sample());
            sum_ = 0;
            n_ = 0;
        }
    }
}

}  // namespace galaxian
