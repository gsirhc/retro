#include "sound.h"

#include <algorithm>
#include <cmath>

namespace mw8080 {
namespace circuit {
namespace {

constexpr double kDt = 1.0 / kSampleRate;
constexpr double kVbe = 0.5;  // OP_AMP_NORTON_VBE
constexpr double kTtlHigh = 3.4;  // DEFAULT_TTL_V_LOGIC_1

constexpr double K(double v) { return v * 1e3; }
constexpr double M(double v) { return v * 1e6; }
constexpr double U(double v) { return v * 1e-6; }
constexpr double P(double v) { return v * 1e-12; }

double rc_exp(double rc) { return 1.0 - std::exp(-kDt / rc); }
double rc_exp_dt(double rc, double dt) { return 1.0 - std::exp(-dt / rc); }
double par(double a, double b) { return a * b / (a + b); }

}  // namespace

// --- DST_OP_AMP ---

void OpAmp::init(const OpAmpInfo& info) {
    info_ = info;
    has_r1_ = info.r1 > 0;
    has_r4_ = info.r4 > 0;
    v_max_ = info.vp - kVbe;
    v_cap_ = 0;
    has_cap_ = info.c > 0;
    if (has_cap_) exponent_ = has_r4_ ? rc_exp(info.r4 * info.c) : kSampleRate * info.c;
    i_fixed_ = info.r3 > 0 ? (info.vp - kVbe) / info.r3 : 0;
}

double OpAmp::step(double in0, double in1) {
    double i_neg = 0;
    if (has_r1_) {
        i_neg = (in0 - kVbe) / info_.r1;
        if (i_neg < 0) i_neg = 0;
    }
    i_neg += i_fixed_;

    double i_pos = (in1 - kVbe) / info_.r2;
    if (i_pos < 0) i_pos = 0;

    double i = i_pos - i_neg;
    double v_out;
    if (has_cap_) {
        if (has_r4_) {
            i *= info_.r4;
            v_cap_ += (i - v_cap_) * exponent_;
        } else {
            v_cap_ += i / exponent_;
        }
        v_out = v_cap_;
    } else if (has_r4_) {
        v_out = i * info_.r4;
    } else {
        v_out = i > 0 ? v_max_ : 0;
    }

    if (v_out > v_max_) v_out = v_max_;
    else if (v_out < info_.vn) v_out = info_.vn;
    v_cap_ = v_out;
    return v_out;
}

// --- DST_OP_AMP_1SHT ---

void OneShot::init(const OneShotInfo& info) {
    info_ = info;
    exponent1c_ = rc_exp(par(info.r3, info.r4) * info.c1);
    exponent1d_ = rc_exp(info.r4 * info.c1);
    exponent2_ = rc_exp(info.r2 * info.c2);
    i_fixed_ = (info.vp - kVbe) / info.r1;
    v_cap1_ = v_cap2_ = 0;
    v_out_ = 0;
    v_max_ = info.vp - kVbe;
    r34ratio_ = info.r3 / (info.r3 + info.r4);
}

double OneShot::step(double trigger) {
    double i_pos = (trigger - v_cap2_) / info_.r2;
    i_pos += v_out_ / info_.r5;
    v_cap2_ += (trigger - v_cap2_) * exponent2_;

    double i_neg = (v_cap1_ - kVbe) / info_.r3;
    if (i_neg < 0) i_neg = 0;
    i_neg += i_fixed_;

    v_out_ = i_pos > i_neg ? v_max_ : info_.vn;

    double v = v_out_ + 0.6;
    if (v_cap1_ > v_out_) {
        if (v_cap1_ > v) v_cap1_ = v;
        else v_cap1_ += (v_out_ - v_cap1_) * exponent1d_;
    } else {
        v_cap1_ += ((v_out_ - kVbe) * r34ratio_ + kVbe - v_cap1_) * exponent1c_;
    }
    return v_out_;
}

// --- DST_TVCA_OP_AMP ---

void Tvca::init(const TvcaInfo& info) {
    info_ = info;
    v_out_max_ = info.vp - kVbe;
    v_trig_ = (info.v1 - 0.6 - kVbe) * (info.r7 / (info.r5 + info.r7)) + kVbe;
    i_fixed_ = v_out_max_ / info.r1;
    v_cap1_ = 0;
    exponent_c_ = rc_exp(par(info.r5, info.r7) * info.c1);
    exponent_d_ = rc_exp(info.r7 * info.c1);
}

double Tvca::step(double trig0, double in0) {
    double i2 = (in0 - kVbe) / info_.r2;
    if (i2 < 0) i2 = 0;
    double i_neg = i_fixed_ + i2;

    if (static_cast<int>(trig0)) v_cap1_ += (v_trig_ - v_cap1_) * exponent_c_;
    else v_cap1_ += (kVbe - v_cap1_) * exponent_d_;

    double i_pos = (v_cap1_ - kVbe) / info_.r7;
    if (i_pos < 0) i_pos = 0;

    double i_out = i_pos - i_neg;
    if (i_out < 0) i_out = 0;

    double v_out = i_out * info_.r4;
    if (v_out > v_out_max_) v_out = v_out_max_;
    return v_out;
}

// --- DSS_OP_AMP_OSC ---

void OpAmpOsc::init(const OscInfo& info) {
    info_ = info;
    v_out_high_ = info.vp - kVbe;
    v_cap_ = 0;
    i_fixed_ = 0;
    double i1, i2;
    switch (info.type) {
    case kOsc1:
        flip_flop_xor_ = false;
        flip_flop_ = true;
        i1 = v_out_high_ / info.r5;
        i2 = (0.0 - kVbe) / info.r4;
        threshold_low_ = (i1 + i2) * info.r3 + kVbe;
        i2 = (v_out_high_ - kVbe) / info.r4;
        threshold_high_ = (i1 + i2) * info.r3 + kVbe;
        break;
    case kVco1:
        flip_flop_xor_ = true;
        flip_flop_ = false;
        i1 = (info.vp - kVbe) / info.r5;
        i2 = (info.vp - kVbe - kVbe) / info.r4;
        threshold_low_ = (i1 - i2) * info.r3 + kVbe;
        i2 = (0.0 - kVbe) / info.r4;
        threshold_high_ = (i1 - i2) * info.r3 + kVbe;
        break;
    case kVco3:
        flip_flop_xor_ = false;
        flip_flop_ = true;
        if (info.r7 != 0) i_fixed_ = (info.vp - kVbe) / info.r7;
        temp1_ = (info.vp - kVbe - kVbe) / info.r2;
        i1 = (info.vp - kVbe) / info.r5;
        i2 = (0.0 - kVbe) / info.r4;
        threshold_low_ = (i1 + i2) * info.r3 + kVbe;
        i2 = (info.vp - kVbe - kVbe) / info.r4;
        threshold_high_ = (i1 + i2) * info.r3 + kVbe;
        break;
    }
}

double OpAmpOsc::step(double vmod1, double vmod2) {
    double dt = kDt;
    double v_cap = v_cap_;
    double v_cap_next = 0;
    double charge[2] = {0, 0};
    bool flip_flop = flip_flop_;
    int count_f = 0, count_r = 0;

    switch (info_.type) {
    case kOsc1:
        charge[0] = v_out_high_ / info_.r1;
        charge[1] = (v_out_high_ - kVbe) / info_.r2 - charge[0];
        break;
    case kVco1: {
        double v = vmod1 - kVbe;
        charge[0] = v / info_.r1;
        charge[1] = v / info_.r2 - charge[0];
        break;
    }
    case kVco3: {
        charge[0] = i_fixed_;
        double v = vmod1 - kVbe;
        if (v < 0) v = 0;
        charge[0] += v / info_.r1;
        if (info_.r6 != 0) {
            v = vmod2 - kVbe;
            charge[0] += v / info_.r6;
        }
        charge[1] = temp1_ - charge[0];
        break;
    }
    }

    do {
        if (flip_flop ^ flip_flop_xor_) {
            v_cap_next = v_cap + charge[1] * dt / info_.c;
            dt = 0;
            if (v_cap_next > threshold_high_) {
                flip_flop = flip_flop_xor_;
                if (flip_flop) count_r++;
                else count_f++;
                dt = info_.c * (v_cap_next - threshold_high_) / charge[1];
                v_cap_next = threshold_high_;
            }
        } else {
            v_cap_next = v_cap - charge[0] * dt / info_.c;
            dt = 0;
            if (v_cap_next < threshold_low_) {
                flip_flop = !flip_flop_xor_;
                if (flip_flop) count_r++;
                else count_f++;
                dt = info_.c * (threshold_low_ - v_cap_next) / charge[0];
                v_cap_next = threshold_low_;
            }
        }
        v_cap = v_cap_next;
    } while (dt);
    if (v_cap > v_out_high_) v_cap = v_out_high_;
    if (v_cap < 0) v_cap = 0;
    v_cap_ = v_cap;

    double v_out;
    if (info_.out == kOutCap) {
        v_out = v_cap;
    } else if (count_f + count_r >= 2) {
        v_out = flip_flop_ ? 0 : v_out_high_;
    } else {
        v_out = flip_flop ? v_out_high_ : 0;
    }
    flip_flop_ = flip_flop;
    return v_out;
}

// --- DSD_555_ASTBL ---

void Astable555::init(const Astable555Info& info, double c) {
    info_ = info;
    c_ = c;
    threshold_ = info.v_pos * 2.0 / 3.0;
    trigger_ = info.v_pos / 3.0;
    last_r1_ = last_r2_ = -1;
    cap_voltage_ = 0;
    flip_flop_ = true;
}

double Astable555::step(bool run, double r1, double r2) {
    if (!run) {
        flip_flop_ = true;
        cap_voltage_ = 0;
        return 0;
    }

    if (r1 != last_r1_ || r2 != last_r2_) {
        t_rc_bleed_ = M(10) * c_;
        t_rc_charge_ = (r1 + r2) * c_;
        t_rc_discharge_ = r2 * c_;
        exp_bleed_ = rc_exp(t_rc_bleed_);
        exp_charge_ = rc_exp(t_rc_charge_);
        exp_discharge_ = rc_exp(t_rc_discharge_);
        last_r1_ = r1;
        last_r2_ = r2;
    }

    double dt = kDt;
    double x_time = 0;
    double v_cap = cap_voltage_;
    double v_cap_next = 0;
    double exponent = 0;
    bool flip_flop = flip_flop_;
    bool update_exponent = false;
    int count_f = 0, count_r = 0;
    const double v_charge = info_.v_charge;

    do {
        if (flip_flop) {
            if (r1 == 0) {
                exponent = update_exponent ? rc_exp_dt(t_rc_bleed_, dt) : exp_bleed_;
                v_cap_next = v_cap - v_cap * exponent;
                dt = 0;
            } else {
                exponent = update_exponent ? rc_exp_dt(t_rc_charge_, dt) : exp_charge_;
                v_cap_next = v_cap + (v_charge - v_cap) * exponent;
                dt = 0;
                if (v_cap_next >= threshold_) {
                    dt = t_rc_charge_ * std::log(1.0 / (1.0 - ((v_cap_next - threshold_) / (v_charge - v_cap))));
                    x_time = dt;
                    v_cap_next = threshold_;
                    flip_flop = false;
                    count_f++;
                    update_exponent = true;
                }
            }
        } else {
            if (r2 != 0) {
                exponent = update_exponent ? rc_exp_dt(t_rc_discharge_, dt) : exp_discharge_;
                v_cap_next = v_cap - v_cap * exponent;
                dt = 0;
            } else {
                v_cap_next = trigger_;
            }
            if (v_cap_next <= trigger_) {
                if (v_cap_next < trigger_) dt = t_rc_discharge_ * std::log(1.0 / (1.0 - ((trigger_ - v_cap_next) / v_cap)));
                x_time = dt;
                v_cap_next = trigger_;
                flip_flop = true;
                count_r++;
                update_exponent = true;
            }
        }
        v_cap = v_cap_next;
    } while (dt);
    cap_voltage_ = v_cap;

    x_time = x_time / kDt;
    double v_out;
    if (info_.energy_out) {
        if (x_time == 0) x_time = 1.0;
        v_out = info_.v_out_high * (flip_flop ? x_time : (1.0 - x_time));
    } else if (count_f + count_r >= 2) {
        v_out = flip_flop_ ? 0 : info_.v_out_high;
    } else {
        v_out = flip_flop * info_.v_out_high;
    }
    flip_flop_ = flip_flop;
    return v_out;
}

// --- DST_RCFILTER, DST_CRFILTER ---

void RcFilter::init(double r, double c) {
    exponent_ = rc_exp(r * c);
    v_out_ = 0;
}

double RcFilter::step(double in) {
    v_out_ += (in - v_out_) * exponent_;
    return v_out_;
}

void CrFilter::init(double r, double c) {
    exponent_ = rc_exp(r * c);
    v_cap_ = 0;
}

double CrFilter::step(double in) {
    double v_out = in - v_cap_;
    v_cap_ += v_out * exponent_;
    return v_out;
}

// --- DSS_SQUAREWFIX ---

void SquareWFix::init(double freq, double amp, double duty, double bias) {
    freq_ = freq;
    amp_ = amp;
    duty_ = duty;
    bias_ = bias;
    flip_flop_ = true;
    t_off_ = 1.0 / freq;
    t_on_ = t_off_ * (duty / 100.0);
    t_off_ -= t_on_;
    t_left_ = 0;
    while (t_left_ <= 0) {
        flip_flop_ = !flip_flop_;
        t_left_ += flip_flop_ ? t_on_ : t_off_;
    }
}

double SquareWFix::step() {
    t_left_ -= kDt;
    while (t_left_ <= 0) {
        flip_flop_ = !flip_flop_;
        t_left_ += flip_flop_ ? t_on_ : t_off_;
    }
    return (flip_flop_ ? amp_ / 2.0 : -(amp_ / 2.0)) + bias_;
}

// --- DSS_LFSR_NOISE ---

namespace {
constexpr double kNoiseClockHz = 7515;  // measured on the breadboard (MAME INVADERS_NOISE_GENERATOR)
constexpr uint32_t kLfsrMask = 0x1ffff;
}  // namespace

void Noise::init() {
    reg_ = kLfsrMask;
    uint32_t fb = ((reg_ >> 4) ^ (reg_ >> 16)) & 1;
    reg_ |= fb << 17;
    t_left_ = 0;
    out_ = ((reg_ >> 12) & 1) ? 12 : 0;
}

double Noise::step() {
    const double t_clock = 1.0 / kNoiseClockHz;
    double cycles = (t_left_ + kDt) / t_clock;
    int inc = static_cast<int>(cycles);
    t_left_ = (cycles - inc) * t_clock;
    for (int n = 0; n < inc; n++) {
        uint32_t fb = (reg_ >> 17) & 1;
        reg_ = ((reg_ << 1) & kLfsrMask) | fb;
        fb = ((reg_ >> 4) ^ (reg_ >> 16)) & 1;
        reg_ |= fb << 17;
    }
    if (inc > 0) out_ = ((reg_ >> 12) & 1) ? 12 : 0;
    return out_;
}

// --- DST_MIXER ---

namespace {
// R78, R134+R133+R132, R136, R59, R86+R85+R84, R28
constexpr double kMixR[6] = {K(200), K(10) + 100 + 100, K(150), K(200), K(2) + K(6.8) + K(5.6), K(150)};
constexpr double kMixRf = K(100);  // R105
}  // namespace

void Mixer::init() {
    exponent_rc_ = rc_exp(kMixR[5] * U(0.001));  // C11 on the missile input
    exponent_amp_ = rc_exp(K(100) * U(0.1));  // C45 into 100k
    v_cap_ = v_cap_amp_ = 0;
}

double Mixer::step(const double (&in)[6]) {
    double i = 0;
    for (int bit = 0; bit < 6; bit++) {
        double v = in[bit];
        if (bit == 5) {
            v_cap_ += (v - v_cap_) * exponent_rc_;
            v -= v_cap_;
        }
        i += (0.0 - v) / kMixR[bit];
    }
    double v = i * kMixRf;
    v_cap_amp_ += (v - v_cap_amp_) * exponent_amp_;
    v -= v_cap_amp_;
    return v;
}

// --- SN76477 ---

namespace {
constexpr double kSlfMin = 0.33, kSlfMax = 2.37, kSlfRange = kSlfMax - kSlfMin;
constexpr double kVcoToSlf = 0.35;
constexpr double kVcoMin = kSlfMin, kVcoMax = kSlfMax + kVcoToSlf, kVcoRange = kVcoMax - kVcoMin;
constexpr double kAdMax = 4.44;
constexpr double kOutCenter = 2.57, kOutHighClip = 3.51, kOutLowClip = 0.715;

constexpr double kOutPosGain[] = {
    0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.01,
    0.03, 0.11, 0.15, 0.19, 0.21, 0.23, 0.26, 0.29, 0.31, 0.33,
    0.36, 0.38, 0.41, 0.43, 0.46, 0.49, 0.52, 0.54, 0.57, 0.60,
    0.62, 0.65, 0.68, 0.70, 0.73, 0.76, 0.80, 0.82, 0.84, 0.87,
    0.90, 0.93, 0.96, 0.98, 1.00};

constexpr double kOutNegGain[] = {
    0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, 0.00, -0.01,
    -0.02, -0.09, -0.13, -0.15, -0.17, -0.19, -0.22, -0.24, -0.26, -0.28,
    -0.30, -0.32, -0.34, -0.37, -0.39, -0.41, -0.44, -0.46, -0.48, -0.51,
    -0.53, -0.56, -0.58, -0.60, -0.62, -0.65, -0.67, -0.69, -0.72, -0.74,
    -0.76, -0.78, -0.81, -0.84, -0.85};

// SLF 1uF / 120k, VCO 0.1uF / 8.2k, amp 56k, feedback 10k (MAME invaders_audio_device)
constexpr double kSlfRes = K(120), kSlfCap = U(1.0);
constexpr double kVcoRes = K(8.2), kVcoCap = U(0.1);
constexpr double kAmpRes = K(56), kFeedbackRes = K(10);
}  // namespace

void Sn76477::init() {
    enable_ = true;
    one_shot_running_ = false;
    slf_cap_voltage_ = kSlfMin;
    vco_cap_voltage_ = kVcoMin;
    slf_out_ = vco_out_ = false;
    slf_charge_ = kSlfRange / (0.5885 * kSlfRes * kSlfCap + 0.001300) / kSampleRate;
    slf_discharge_ = kSlfRange / (0.5413 * kSlfRes * kSlfCap + 0.001343) / kSampleRate;
    vco_step_ = 0.64 * 2 * kVcoRange / (kVcoRes * kVcoCap) / kSampleRate;
}

void Sn76477::enable_w(bool state) {
    if (state == enable_) return;
    enable_ = state;
    if (!enable_) one_shot_running_ = true;
}

double Sn76477::step() {
    if (!slf_out_) slf_cap_voltage_ = std::min(slf_cap_voltage_ + slf_charge_, kSlfMax);
    else slf_cap_voltage_ = std::max(slf_cap_voltage_ - slf_discharge_, kSlfMin);
    if (slf_cap_voltage_ >= kSlfMax) slf_out_ = true;
    else if (slf_cap_voltage_ <= kSlfMin) slf_out_ = false;

    double vco_max = slf_cap_voltage_ + kVcoToSlf;
    if (!vco_out_) vco_cap_voltage_ = std::min(vco_cap_voltage_ + vco_step_, vco_max);
    else vco_cap_voltage_ = std::max(vco_cap_voltage_ - vco_step_, kVcoMin);
    if (vco_cap_voltage_ >= vco_max) vco_out_ = true;
    else if (vco_cap_voltage_ <= kVcoMin) vco_out_ = false;

    // No attack/decay cap: the A/D voltage jumps to its rail with the one-shot state.
    double ad = one_shot_running_ ? kAdMax : 0.0;

    double v_out = kOutCenter;
    if (!enable_ && vco_cap_voltage_ <= kVcoMax) {
        double center_to_peak = 3.818 * (kFeedbackRes / kAmpRes) + 0.03;
        int idx = static_cast<int>(ad * 10);
        if (vco_out_) v_out = std::min(kOutCenter + center_to_peak * kOutPosGain[idx], kOutHighClip);
        else v_out = std::max(kOutCenter + center_to_peak * kOutNegGain[idx], kOutLowClip);
    }
    v_pin_ = v_out;
    return (v_out - kOutLowClip) / (kOutCenter - kOutLowClip) - 1;
}

// --- Taito PV output stage ---

namespace {
// UFO, trigger, flash, target hit, extend play, step, UFO hit series resistors (PVR00001)
constexpr double kTaitoR[7] = {K(47), K(150), K(2), K(47), K(220), K(68), K(47)};
constexpr double kTaitoPot = K(50);   // VR1-VR7, B-50K
constexpr double kTaitoWiper = 0.5;  // trimmers assumed at mid travel
// 100K parallel with VR8 (50K, AC-coupled by 1uF) at mid travel
constexpr double kTaitoFeedback = 1.0 / (1.0 / K(100) + 1.0 / K(25));
}  // namespace

void TaitoMixer::init() {
    for (double& v : v_cap_) v = 0;
    v_cap_out_ = 0;
}

double TaitoMixer::step(const double (&in)[7]) {
    double sum = 0;
    for (int n = 0; n < 7; n++) {
        // trigger also has 0.001uF in series ahead of the 1uF
        const double c = n == 1 ? 1.0 / (1.0 / U(0.001) + 1.0 / U(1)) : U(1);
        const double rs = kTaitoR[n];
        v_cap_[n] += (in[n] - v_cap_[n]) * rc_exp((rs + kTaitoPot) * c);
        // wiper Thevenin source into 47K at the summing amp's virtual ground
        const double lower = kTaitoPot * kTaitoWiper, upper = rs + kTaitoPot - lower;
        const double v_th = (in[n] - v_cap_[n]) * lower / (rs + kTaitoPot);
        const double r_th = lower * upper / (lower + upper);
        sum += v_th / (r_th + K(47));
    }
    sum *= kTaitoFeedback;
    // 0.1uF into the MB3712's 47K
    v_cap_out_ += (sum - v_cap_out_) * rc_exp(K(47) * U(0.1));
    return sum - v_cap_out_;
}

}  // namespace circuit

namespace {

using namespace circuit;

constexpr double kOutputGain = 2500.0 / 32768.0;  // DISCRETE_OUTPUT gain 2500 into a 16-bit stream
constexpr double kTaitoGain = 1.0;  // 1 V at the summing amp output is full scale

// R70, R64, R61, R63, R62, C21
const OscInfo kSaucerOsc = {kOsc1, kOutCap, M(1), K(470), K(100), K(120), M(1), 0, 0, 0, U(0.1), 12};
// R65, R66, R67, R69, R68, C22
const OscInfo kSaucerVco = {kVco1, kOutSqw, M(1), K(470), K(680), M(1), M(1), 0, 0, 0, P(470), 12};
// R72 / R71 / C23 (IC B3 pin 9); R73, R77, R74, R75 (pin 10)
const OpAmpInfo kSaucerB3_9 = {0, K(100), M(1), 0, U(1), 0, 12};
const OpAmpInfo kSaucerB3_10 = {K(680), K(680), M(2.7), K(680), 0, 0, 12};

// R126+R127, R128, R129, R130 (MAME invaders_thump_resistors)
constexpr double kThumpR[4] = {K(20) + K(20), K(68), K(82), K(100)};
const Astable555Info kThump555 = {true, 5, 5.0 - 0.6, kTtlHigh};
const Astable555Info kBonus555 = {false, 5, 5.0, 5.0 - 1.2};

// R49, R51, R48, R50, R52, C18, C20
const OneShotInfo kHit1sht = {M(4.7), K(100), M(1), M(1), M(2.2), U(0.1), P(470), 0, 12};
// R37, R41, R38, R40, R39, C16; Taito PVR00001 draws 0.22uF
const OscInfo kHitOsc = {kOsc1, kOutCap, M(1), K(10), K(100), K(120), M(1), 0, 0, 0, U(0.1), 12};
const OscInfo kHitOscTaito = {kOsc1, kOutCap, M(1), K(10), K(100), K(120), M(1), 0, 0, 0, U(0.22), 12};
// R42, R43, R44, R46, R45, C16
const OscInfo kHitVco = {kVco1, kOutCap, M(1), K(470), K(680), M(1), M(1), 0, 0, 0, P(330), 12};
const OpAmpInfo kHitD3_10 = {0, K(10), M(1), 0, U(0.47), 0, 12};
const OpAmpInfo kHitD3_4 = {K(470), K(680), M(2.7), K(680), 0, 0, 12};

// R90, R88, R91, R89, R92, C24, C25; C24 is not legible on PVR00001
const OneShotInfo kExpl1sht = {M(4.7), K(100), M(1), M(1), M(2.2), U(2.2), P(470), 0, 12};
// R80, R79, R82, R93, R83, C26
const TvcaInfo kExplTvca = {M(2.7), K(680), K(680), K(10), K(680), U(1), 12.0 - kVbe, 12};

// R32, R30, R31, R33, R34, C12, C15; Taito C12 0.22uF (PVR00001, MAME note)
const OneShotInfo kMissile1sht = {M(4.7), K(100), M(1), M(1), M(2.2), U(1), P(470), 0, 12};
const OneShotInfo kMissile1shtTaito = {M(4.7), K(100), M(1), M(1), M(2.2), U(0.22), P(470), 0, 12};
const OpAmpInfo kMissileB3 = {0, K(10), M(1.5), 0, U(0.22), 0, 12};
// (R29||R11)+R12, R16, R17, R19, R16, R14, R13, C58
const OscInfo kMissileOsc = {kVco3, kOutSqw, 1.0 / (1.0 / M(1) + 1.0 / K(330)) + M(1.5), M(1), K(560), M(2.2), M(1), M(4.7), M(3.3), 0, P(330), 12};
// PVR00001 draws C58 as 300pF and R13 as 3.3M
const OscInfo kMissileOscTaito = {kVco3, kOutSqw, 1.0 / (1.0 / M(1) + 1.0 / K(330)) + M(1.5), M(1), K(560), M(2.2), M(1), M(4.7), M(3.3), 0, P(300), 12};
const OpAmpInfo kMissileA3 = {K(560), K(470), M(2.7), K(560), 0, 0, 12};
// R25, R23, R26, R1k, R60, C14
const TvcaInfo kMissileTvca = {M(2.7), K(560), K(560), K(1), K(560), U(0.1), 5, 12};

}  // namespace

void Sound::reset() {
    const bool taito = board_ == SoundBoard::TaitoPv;
    p3_ = p5_ = 0;
    cycle_acc_ = 0;
    ticks_ = 0;
    sum_ = 0;
    count_ = 0;

    noise_.init();
    saucer_b3_9_.init(kSaucerB3_9);
    saucer_b3_10_.init(kSaucerB3_10);
    saucer_osc_.init(kSaucerOsc);
    saucer_vco_.init(kSaucerVco);

    for (int d = 0; d < 16; d++) {
        double g = 0;
        for (int bit = 0; bit < 4; bit++) {
            if (d & (1 << bit)) g += 1.0 / kThumpR[bit];
        }
        thump_r_[d] = g != 0 ? 1.0 / g : 0;
    }
    fleet_555_.init(kThump555, U(0.1));
    fleet_rc1_.init(100, U(4.7));
    fleet_rc2_.init(100 + 100, U(10));
    fleet_555_out_ = fleet_555_.step(true, thump_r_[0], K(75));

    bonus_555_.init(kBonus555, U(1));
    // Taito gates the 960 Hz video-chain tap (PVR00001)
    bonus_sqw_.init(taito ? 960 : 480, 1, 50, 1.0 / 2);

    hit_1sht_.init(kHit1sht);
    hit_d3_10_.init(kHitD3_10);
    hit_d3_4_.init(kHitD3_4);
    hit_osc_.init(taito ? kHitOscTaito : kHitOsc);
    hit_vco_.init(kHitVco);

    expl_1sht_.init(kExpl1sht);
    expl_tvca_.init(kExplTvca);
    // R84 is 560 ohms on PVR00001
    const double r84 = taito ? 560 : K(5.6);
    expl_rc1_.init(r84, U(0.1));
    expl_rc2_.init(r84 + K(6.8), U(0.1));

    missile_1sht_.init(taito ? kMissile1shtTaito : kMissile1sht);
    missile_b3_.init(kMissileB3);
    missile_a3_.init(kMissileA3);
    missile_cr_.init(M(1) + K(330), U(0.1));
    missile_osc_.init(taito ? kMissileOscTaito : kMissileOsc);
    missile_tvca_.init(kMissileTvca);

    mixer_.init();
    taito_mixer_.init();
    sn_.init();
}

// bit0 high opens the SN76477 ENABLE pin (MAME invaders_audio_device::p1_w)
void Sound::port3_w(uint8_t v) {
    p3_ = v;
    sn_.enable_w(!(v & 0x01));
}

void Sound::port5_w(uint8_t v) {
    p5_ = v;
}

void Sound::advance(int cpu_cycles) {
    cycle_acc_ += static_cast<int64_t>(cpu_cycles) * kSampleRate;
    while (cycle_acc_ >= kCpuHz) {
        cycle_acc_ -= kCpuHz;
        tick();
    }
}

float Sound::mix() {
    float out = count_ ? static_cast<float>(sum_ / count_) : 0.0f;
    sum_ = 0;
    count_ = 0;
    return out;
}

void Sound::tick() {
    const double n_noise = noise_.step();

    const double saucer_en = (p5_ & 0x10) ? 12 : 0;
    const double n20 = saucer_b3_9_.step(0, saucer_en);
    const double n21 = saucer_osc_.step(0, 0);
    const double n22 = saucer_vco_.step(n21, 0);
    const double n_saucer = saucer_b3_10_.step(n22, n20);

    fleet_555_out_ = fleet_555_.step(true, thump_r_[p5_ & 0x0f], K(75));
    const double n_fleet = fleet_rc2_.step(fleet_rc1_.step(fleet_555_out_));

    const bool bonus_en = (p3_ & 0x10) != 0;
    const double n40 = bonus_555_.step(bonus_en, K(100), K(47));
    const double n41 = bonus_sqw_.step();
    const double n_bonus = (bonus_en && n41 != 0 && n40 != 0) ? kTtlHigh : 0;

    const double hit_en = (p3_ & 0x08) ? 5 : 0;
    const double n50 = hit_1sht_.step(hit_en);
    const double n51 = hit_d3_10_.step(0, n50);
    const double n52 = hit_osc_.step(0, 0);
    const double n53 = hit_vco_.step(n52, 0);
    const double n_hit = hit_d3_4_.step(n53, n51);

    const double expl_en = (p3_ & 0x04) ? 5 : 0;
    const double n60 = expl_1sht_.step(expl_en);
    const double n61 = expl_tvca_.step(n60, n_noise);
    const double n_expl = expl_rc2_.step(expl_rc1_.step(n61));

    const double missile_en = (p3_ & 0x02) ? 5 : 0;
    const double n70 = missile_1sht_.step(missile_en);
    const double n71 = missile_b3_.step(0, n70);
    // diode drop, then clamp 0..12
    const double n73 = std::min(std::max(n71 - 0.5, 0.0), 12.0);
    const double n75 = missile_cr_.step(n_noise) * (K(330) / (M(1) + K(330)));
    const double n76 = missile_osc_.step(n75, n73);
    const double n77 = missile_a3_.step(n76, n73);
    const double n_missile = missile_tvca_.step(missile_en, n77);

    const double sn = sn_.step();
    double out;
    if (board_ == SoundBoard::TaitoPv) {
        const double in[7] = {sn_.v_pin_, n_missile, n_expl, n_hit, n_bonus, n_fleet, n_saucer};
        out = taito_mixer_.step(in) * kTaitoGain;
    } else {
        const double in[6] = {n_saucer, n_fleet, n_bonus, n_hit, n_expl, n_missile};
        // each source routed to the mono speaker at 0.5
        out = 0.5 * mixer_.step(in) * kOutputGain + 0.5 * sn;
    }
    out = std::min(std::max(out, -1.0), 1.0);
    if (!amp_enabled()) out = 0;

    sum_ += out;
    count_++;
    ticks_++;
}

}  // namespace mw8080
