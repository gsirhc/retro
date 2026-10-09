#include "sound.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace galaxian {
namespace {

constexpr int kStepClocks = 8;
constexpr double kDt = kStepClocks / kSoundClock;
constexpr double kTtl = 4.0;

constexpr double kR15 = 100e3, kR16 = 220e3, kR17 = 470e3, kR18 = 1000e3, kR19 = 330e3;
constexpr double kR20 = 15e3, kR21 = 100e3, kR22 = 100e3, kR23 = 470e3, kR24 = 10e3;
constexpr double kR25 = 100e3, kR26 = 330e3, kR27 = 10e3, kR28 = 100e3, kR29 = 220e3;
constexpr double kR30 = 10e3, kR31 = 47e3, kR32 = 47e3, kR33 = 10e3, kR34 = 5.1e3;
constexpr double kR35 = 150e3, kR36 = 22e3, kR37 = 470e3, kR38 = 33e3, kR39 = 22e3;
constexpr double kR40 = 2.2e3, kR41 = 100e3, kR43 = 2.2e3, kR44 = 10e3, kR45 = 22e3;
constexpr double kR46 = 10e3, kR47 = 2.2e3, kR48 = 2.2e3, kR49 = 10e3;
constexpr double kR50 = 22e3, kR51 = 33e3, kR52 = 15e3, kR91 = 10e3;
constexpr double kC15 = 1e-6, kC17 = 0.01e-6, kC18 = 0.01e-6, kC19 = 0.01e-6;
constexpr double kC20 = 0.1e-6, kC21 = 2.2e-6, kC22 = 0.01e-6, kC23 = 0.01e-6;
constexpr double kC25 = 1e-6, kC26 = 0.01e-6, kC27 = 0.01e-6, kC28 = 47e-6, kC46 = 0.1e-6;

constexpr double kFsRa[3] = {kR22, kR25, kR28};
constexpr double kFsRb[3] = {kR23, kR26, kR29};
constexpr double kFsC[3] = {kC17, kC18, kC19};

double par(double a, double b) { return a * b / (a + b); }
double charge_exp(double rc) { return 1.0 - std::exp(-kDt / rc); }

// Noise LS164 chain: QC of the second into F0 with QH of the third inverted (MAME galaxian_lfsr).
const std::vector<uint8_t>& noise_table() {
    static const std::vector<uint8_t> t = [] {
        std::vector<uint8_t> out;
        uint32_t reg = 0;
        do {
            uint32_t f0 = ((reg >> 4) & 1) ^ (((reg >> 16) & 1) ^ 1);
            reg = ((reg << 1) | f0) & 0x1FFFF;
            out.push_back(uint8_t(f0));
        } while (reg != 0 && out.size() < (1u << 17));
        return out;
    }();
    return t;
}

// NE555 astable with pin 5 driven: thresholds CV and CV/2. Returns the fraction of dt spent high.
double astable(double& v, bool& charging, double ra, double rb, double c, double cv, bool run) {
    const double tau_d = rb * c;
    if (!run) {
        // RESET low holds the output low and the discharge pin drains C through Rb.
        v *= std::exp(-kDt / tau_d);
        charging = false;
        return 0.0;
    }
    if (cv <= 0.0) {
        v *= std::exp(-kDt / tau_d);
        charging = false;
        return 0.0;
    }
    const double tau_c = (ra + rb) * c;
    const double thr = std::min(cv, 4.999);
    const double trg = cv / 2.0;
    double t = kDt, high = 0.0;
    for (int guard = 0; t > 0.0 && guard < 64; guard++) {
        if (charging) {
            if (v >= thr) {
                charging = false;
                continue;
            }
            double tc = tau_c * std::log((5.0 - v) / (5.0 - thr));
            if (tc >= t) {
                v = 5.0 - (5.0 - v) * std::exp(-t / tau_c);
                high += t;
                t = 0.0;
            } else {
                v = thr;
                high += tc;
                t -= tc;
                charging = false;
            }
        } else {
            if (v <= trg) {
                charging = true;
                continue;
            }
            double td = tau_d * std::log(v / trg);
            if (td >= t) {
                v *= std::exp(-t / tau_d);
                t = 0.0;
            } else {
                v = trg;
                t -= td;
                charging = true;
            }
        }
    }
    return high / kDt;
}

// MFB band-pass around the hit noise, bilinear at the step rate (MAME DISC_OP_AMP_FILTER_IS_BAND_PASS_1M).
struct Bandpass {
    double b0, b2, a1, a2, r_total, v_ref;
};

const Bandpass& bandpass() {
    static const Bandpass bp = [] {
        Bandpass o{};
        o.r_total = par(kR35, kR36);
        o.v_ref = 5.0 * kR39 / (kR38 + kR39);
        const double pi = 3.14159265358979323846;
        double fc = 1.0 / (2 * pi * std::sqrt(o.r_total * kR37 * kC22 * kC23));
        double d = (kC22 + kC23) / std::sqrt(kR37 / o.r_total * kC22 * kC23);
        double gain = -kR37 / o.r_total * kC23 / (kC22 + kC23);
        double k = 1.0 / std::tan(pi * fc * kDt);
        double den = k * k + d * k + 1.0;
        o.b0 = gain * d * k / den;
        o.b2 = -o.b0;
        o.a1 = 2.0 * (1.0 - k * k) / den;
        o.a2 = (k * k - d * k + 1.0) / den;
        return o;
    }();
    return bp;
}

}  // namespace

void DiscreteSound::reset() {
    fs[0] = fs[1] = fs[2] = false;
    hit = fire = false;
    vol[0] = vol[1] = false;
    lfo_bits = 0;
    pitch = 0xFF;
    cpu_rem_ = clock_rem_ = 0;
    note_cnt1_ = 0;
    note_cnt2_ = 0;
    qa_acc_ = qc_acc_ = qd_acc_ = 0;
    lfsr_clock_ = 0;
    line_phase_ = 0;
    noise_q_ = false;
    bck_cap_ = 0;
    bck_cv_ = 0;
    for (Astable& a : fs555_) a = {};
    bck_mix_ = 0;
    hit_cap_ = 0;
    bp_x1_ = bp_x2_ = bp_y1_ = bp_y2_ = 0;
    fire_rc_ = 0;
    fire_cv_ = 0;
    fire555_ = {};
    fire_cap_ = 0;
    c26_ = c46_ = out_ = 0;
    sum_ = 0;
    n_ = 0;
}

void DiscreteSound::sound_w(int offset, bool on) {
    switch (offset & 7) {
        case 0: case 1: case 2:
            fs[offset & 7] = on;
            break;
        case 3:
            hit = on;
            break;
        case 5:
            fire = on;
            break;
        case 6:
            vol[0] = on;
            break;
        case 7:
            vol[1] = on;
            break;
        default:
            break;
    }
}

void DiscreteSound::lfo_freq_w(int bit, bool on) {
    uint8_t mask = uint8_t(1 << (bit & 3));
    if (on) lfo_bits |= mask;
    else lfo_bits = uint8_t(lfo_bits & ~mask);
}

void DiscreteSound::pitch_w(uint8_t data) { pitch = data; }

void DiscreteSound::advance(int cpu_cycles) {
    const auto& noise = noise_table();
    cpu_rem_ += cpu_cycles;
    int clocks = cpu_rem_ / 2;
    cpu_rem_ -= clocks * 2;
    for (int i = 0; i < clocks; i++) {
        // Two LS164s preset from the pitch latch divide by 256 - pitch into a 74393.
        if (++note_cnt1_ > 255) {
            note_cnt1_ = pitch;
            note_cnt2_ = (note_cnt2_ + 1) & 15;
        }
        qa_acc_ += note_cnt2_ & 1;
        qc_acc_ += (note_cnt2_ >> 2) & 1;
        qd_acc_ += (note_cnt2_ >> 3) & 1;
        // The RNG runs at 12.288 MHz, 8 per sound clock; a D flip-flop samples it once per two lines.
        lfsr_clock_ += 8;
        if (++line_phase_ == 192) {
            line_phase_ = 0;
            noise_q_ = noise[size_t(lfsr_clock_ % noise.size())] != 0;
        }
        if (++clock_rem_ == kStepClocks) {
            clock_rem_ = 0;
            step(kTtl * qa_acc_ / kStepClocks, kTtl * qc_acc_ / kStepClocks, kTtl * qd_acc_ / kStepClocks, noise_q_);
            qa_acc_ = qc_acc_ = qd_acc_ = 0;
        }
    }
}

void DiscreteSound::step(double qa, double qc, double qd, bool noise) {
    // Background: R1 ladder sets a PNP current source charging C15; the 555 dumps it at 2/3 Vcc.
    static const double dac_rtotal = 1.0 / (1 / kR18 + 1 / kR17 + 1 / kR16 + 1 / kR15 + 1 / kR20 + 1 / kR19);
    static constexpr double kLadder[4] = {kR18, kR17, kR16, kR15};
    double i_dac = 4.4 / kR20;
    for (int b = 0; b < 4; b++)
        if (lfo_bits & (1 << b)) i_dac += kTtl / kLadder[b];
    double vin = i_dac * dac_rtotal;
    double limit = vin + 0.7;
    double i_cc = std::max(0.0, (5.0 - limit) / kR21);
    double t = kDt;
    for (int guard = 0; t > 0.0 && guard < 8; guard++) {
        double next = bck_cap_ + i_cc * t / kC15;
        if (next < 5.0 * 2 / 3 || i_cc <= 0.0) {
            bck_cap_ = std::min(next, limit);
            break;
        }
        t -= (5.0 * 2 / 3 - bck_cap_) * kC15 / i_cc;
        bck_cap_ = 5.0 / 3;
    }
    bck_cv_ = std::clamp(bck_cap_ * kR33 / (1.0 / (1 / kR31 + 1 / kR32 + 1 / kR33)) - 5.0 * kR33 / kR31, 0.0, 5.0);
    double fs_sum = 0;
    for (int i = 0; i < 3; i++)
        fs_sum += 4.5 * astable(fs555_[i].v, fs555_[i].charging, kFsRa[i], kFsRb[i], kFsC[i], bck_cv_, fs[i]);
    static const double bck_exp = charge_exp((kR24 / 3) * kC20);
    bck_mix_ += (fs_sum / 3.0 - bck_mix_) * bck_exp;

    // HIT: C21 follows the HIT line through a diode and bleeds through R35+R36 while the noise gate is open.
    static const double hit_exp = charge_exp((kR35 + kR36) * kC21);
    double u = std::max(0.0, (hit ? kTtl : 0.0) - 0.7);
    double v155 = 0;
    if (noise) {
        double diff = u - hit_cap_;
        hit_cap_ += diff < 0 ? diff * hit_exp : diff;
        v155 = hit_cap_;
    } else if (u > hit_cap_) {
        hit_cap_ = u;
    }
    const Bandpass& bp = bandpass();
    double vbp = ((v155 - bp.v_ref) / kR35 + (0.0 - bp.v_ref) / kR36) * bp.r_total;
    double y = -bp.a1 * bp_y1_ - bp.a2 * bp_y2_ + bp.b0 * vbp + bp.b2 * bp_x2_ + bp.v_ref;
    bp_x2_ = bp_x1_;
    bp_x1_ = vbp;
    bp_y2_ = bp_y1_;
    // The op-amp stops 1.5 V short of the 5 V rail (MAME OP_AMP_VP_RAIL_OFFSET).
    y = std::clamp(y, 0.0, 5.0 - 1.5);
    bp_y1_ = y - bp.v_ref;
    double v157 = y;

    // FIRE: R47/C28 recovers after the latch drops, sweeping the 555's CV down in pitch.
    static const double fire_rc_exp = charge_exp(kR47 * kC28);
    fire_rc_ += ((fire ? 0.0 : kTtl) - fire_rc_) * fire_rc_exp;
    fire_cv_ = par(kR46, kR48) * ((noise ? kTtl : 0.0) / kR46 + fire_rc_ / kR48);
    double fire_hi = astable(fire555_.v, fire555_.charging, kR44, kR45, kC27, fire_cv_, true);
    double uf = std::max(0.0, (fire ? kTtl : 0.0) - 0.7);
    if (uf > fire_cap_) fire_cap_ = uf;
    else fire_cap_ -= (fire_cap_ - uf) * (1.0 - std::exp(-kDt * fire_hi / (kR41 * kC25)));
    double v182 = fire_cap_ * fire_hi;

    // VOL1/VOL2 switch R49 and R52 into the pitch mix (MAME galaxian_mixerpre_desc).
    double g = 1 / kR51 + 1 / kR50 + 1 / kR34;
    double i = qa / kR51 + qc / kR50 + bck_mix_ / kR34;
    if (vol[0]) {
        g += 1 / kR49;
        i += qc / kR49;
    }
    if (vol[1]) {
        g += 1 / kR52;
        i += qd / kR52;
    }
    double v279 = i / g;

    static const double c26_exp = charge_exp(par(kR43, kR91) * kC26);
    c26_ += (v182 - c26_) * c26_exp;
    static const double r_final = 1.0 / (1 / kR34 + 1 / kR40 + 1 / kR43 + 1 / kR91);
    double v = (v279 / kR34 + v157 / kR40 + (v182 - c26_) / kR43) * r_final;
    // C46 into the amp, whose input MAME takes as 100k.
    static const double c46_exp = charge_exp(100e3 * kC46);
    c46_ += (v - c46_) * c46_exp;
    out_ = v - c46_;
    sum_ += out_;
    n_++;
}

float DiscreteSound::mix() {
    float s = n_ ? float(sum_ / n_) : float(out_);
    sum_ = 0;
    n_ = 0;
    return s;
}

}  // namespace galaxian
