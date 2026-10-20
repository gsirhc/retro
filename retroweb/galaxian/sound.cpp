#include "sound.h"

#include <algorithm>
#include <cmath>

namespace galaxian {

namespace {

constexpr double kVcc = 5.0;
constexpr int kClocksPerStep = 8;
constexpr double kH = kClocksPerStep / kSoundClock;

// TTL high/low (TI SN74LS00 VOH 3.4 V typ, VOL 0.2 V typ), NE555 high at 5 V
// (TI NE555 VOH 3.3 V typ), LM324 top of swing VCC - 1.5 V (TI LM324).
constexpr double kTtlHi = 3.4;
constexpr double kTtlLo = 0.2;
constexpr double k555Hi = 3.3;
constexpr double kOpHi = kVcc - 1.5;
constexpr double kVbe = 0.6;
constexpr double kVt = 0.02585;
constexpr double kDiode = 0.62;
// CD4066B rON at VDD = 5 V, 25 C typical (TI CD4066B).
constexpr double kRon = 470.0;
// 555 control pin: internal 5k/5k/5k divider (TI LM555 schematic) seen as 3.33 V behind 3.33k.
constexpr double kCvThev = kVcc * 2.0 / 3.0;
constexpr double kCvRth = 5000.0 * 10000.0 / 15000.0;

// Background ramp: 9M Q4-Q7 through R18/R17/R16/R15 into Q1, R19 to ground.
constexpr double kDacR[4] = {1e6, 470e3, 220e3, 100e3};
constexpr double kR19 = 330e3;
constexpr double kR20 = 15e3;
constexpr double kR21 = 100e3;
constexpr double kC15 = 1e-6;
// 7T pins 8-10: non-inverting gain set by R31/R32 47k and R33 10k.
constexpr double kR31 = 47e3, kR32 = 47e3, kR33 = 10e3;

// FS1-3: 8R/8S/8T, Ra 100k, Rb R23/R26/R29, C17-C19 0.01 uF.
constexpr double kFsRa = 100e3;
constexpr double kFsRb[3] = {470e3, 330e3, 220e3};
constexpr double kFsC = 0.01e-6;
constexpr double kR24 = 10e3;  // R24, R27, R30
constexpr double kC20 = 0.1e-6;
constexpr double kR34 = 5.1e3;

// Pitch mix from 6T: QA R51, QC R50, QC via 7R and R49 (VOL1), QD via 7R and R52 (VOL2).
constexpr double kR51 = 33e3, kR50 = 22e3, kR49 = 10e3, kR52 = 15e3;

// HIT: R89, D1, C21 2.2 uF, 7R gated by NOISE, R35 into the 7T pins 1-3 band-pass.
constexpr double kR89 = 1e3;
constexpr double kC21 = 2.2e-6;
constexpr double kR35 = 150e3, kR36 = 22e3, kR37 = 470e3;
constexpr double kC22 = 0.01e-6, kC23 = 0.01e-6;
constexpr double kVref = kVcc * 22e3 / (33e3 + 22e3);  // R38/R39, held by C24
constexpr double kR40 = 2.2e3;

// FIRE: R90, D2, C25 1 uF, 7R gated by 7S, R41 100k, 7T pins 5-7 follower.
constexpr double kR90 = 1e3;
constexpr double kC25 = 1e-6;
constexpr double kR41 = 100e3;
constexpr double kR47 = 2.2e3, kR48 = 2.2e3, kR46 = 10e3;
constexpr double kC28 = 47e-6, kC29 = 0.01e-6;
constexpr double kFireRa = 10e3, kFireRb = 22e3, kFireC = 0.01e-6;  // R44, R45, C27
constexpr double kC26 = 0.01e-6;
constexpr double kR43 = 2.2e3;

// Mix node: R91 10k volume pot to ground, C46 0.1 uF into the LM377 input, R92 100k to its bias.
constexpr double kR91 = 10e3;
constexpr double kC46 = 0.1e-6;
constexpr double kR92 = 100e3;

double ttl(double frac) { return kTtlLo + (kTtlHi - kTtlLo) * frac; }

// 555 astable over one step; returns the fraction of it the output was high.
double astable(DiscreteSound::Astable& a, double vcv, double ra, double rb, double c, bool run) {
    if (!run) {
        a.v *= std::exp(-kH / (rb * c));
        a.charging = false;
        return 0.0;
    }
    vcv = std::clamp(vcv, 0.1, kVcc - 0.05);
    const double lo = vcv / 2;
    double t = kH, high = 0;
    for (int guard = 0; t > 0 && guard < 8; guard++) {
        if (a.charging) {
            const double tau = (ra + rb) * c;
            if (a.v >= vcv) {
                a.charging = false;
                continue;
            }
            const double tc = tau * std::log((kVcc - a.v) / (kVcc - vcv));
            if (tc >= t) {
                a.v = kVcc - (kVcc - a.v) * std::exp(-t / tau);
                high += t;
                t = 0;
            } else {
                a.v = vcv;
                high += tc;
                t -= tc;
                a.charging = false;
            }
        } else {
            const double tau = rb * c;
            if (a.v <= lo) {
                a.charging = true;
                continue;
            }
            const double td = tau * std::log(a.v / lo);
            if (td >= t) {
                a.v *= std::exp(-t / tau);
                t = 0;
            } else {
                a.v = lo;
                t -= td;
                a.charging = true;
            }
        }
    }
    return high / kH;
}

}  // namespace

void DiscreteSound::reset() {
    for (bool& f : fs) f = false;
    hit = fire = false;
    vol[0] = vol[1] = false;
    lfo_bits = 0;
    pitch = 0xFF;
    cpu_rem_ = 0;
    pitch_cnt_ = 0xFF;
    rco_ = true;
    q393_ = 0;
    noise_ = true;
    c15_ = 0;
    bck_cv_ = 0;
    for (Astable& a : fs555_) a = Astable{};
    node_b_ = 0;
    c21_ = 0;
    bp_v1_ = 0;
    bp_vo_ = kVref;
    c25_ = 0;
    c28_ = 0;
    fire_cv_ = 0;
    fire555_ = Astable{};
    c26_ = 0;
    c46_ = 0;
    out_ = 0;
    sum_ = 0;
    n_ = 0;
}

void DiscreteSound::sound_w(int offset, bool on) {
    switch (offset & 7) {
        case 0: fs[0] = on; break;
        case 1: fs[1] = on; break;
        case 2: fs[2] = on; break;
        case 3: hit = on; break;
        case 5: fire = on; break;
        case 6: vol[0] = on; break;
        case 7: vol[1] = on; break;
        default: break;  // 9L Q4 is not connected
    }
}

void DiscreteSound::lfo_freq_w(int bit, bool on) {
    const uint8_t m = uint8_t(1u << (bit & 3));
    lfo_bits = on ? uint8_t(lfo_bits | m) : uint8_t(lfo_bits & ~m);
}

void DiscreteSound::pitch_w(uint8_t data) { pitch = data; }

void DiscreteSound::advance(int cpu_cycles) {
    // 2H is half the CPU clock; eight 2H clocks make one step.
    cpu_rem_ += cpu_cycles;
    while (cpu_rem_ >= 2 * kClocksPerStep) {
        cpu_rem_ -= 2 * kClocksPerStep;
        int qa = 0, qc = 0, qd = 0;
        for (int i = 0; i < kClocksPerStep; i++) {
            // 9K/8K reload the latch on the clock after they reach 255; 6T counts each carry.
            pitch_cnt_ = (pitch_cnt_ == 0xFF) ? pitch : uint8_t(pitch_cnt_ + 1);
            const bool rco = pitch_cnt_ == 0xFF;
            if (rco && !rco_) q393_ = uint8_t((q393_ + 1) & 0x0F);
            rco_ = rco;
            qa += q393_ & 1;
            qc += (q393_ >> 2) & 1;
            qd += (q393_ >> 3) & 1;
        }
        step(qa / double(kClocksPerStep), qc / double(kClocksPerStep), qd / double(kClocksPerStep));
    }
}

void DiscreteSound::step(double qa, double qc, double qd) {
    // --- background ramp ---
    double num = 0, den = 1.0 / kR19;
    for (int b = 0; b < 4; b++) {
        const double v = ((lfo_bits >> b) & 1) ? kTtlHi : kTtlLo;
        num += v / kDacR[b];
        den += 1.0 / kDacR[b];
    }
    double vx = (num + (kVcc - kVbe) / kR20) / (den + 1.0 / kR20);
    double i1 = (kVcc - kVbe - vx) / kR20;
    double i2 = 0;
    if (i1 <= 0) {
        vx = num / den;
    } else {
        // Q2's emitter sits VT ln(I2/I1) below Q1's on the shared base.
        i2 = i1 * kR20 / kR21;
        for (int k = 0; k < 4; k++)
            i2 = std::max(1e-12, (kVcc - kVbe - vx - kVt * std::log(i2 / i1)) / kR21);
    }
    c15_ += i2 * kH / kC15;
    // 9R has DISCH, THRES and TRIG all on C15: it drops straight back to 1/3 VCC.
    if (c15_ >= kVcc * 2.0 / 3.0) c15_ = kVcc / 3.0;
    const double gain = 1.0 + kR33 / kR32 + kR33 / kR31;
    bck_cv_ = std::clamp(c15_ * gain - kVcc * kR33 / kR31, 0.0, kOpHi);

    double fs_sum = 0;
    for (int k = 0; k < 3; k++)
        fs_sum += k555Hi * astable(fs555_[k], bck_cv_, kFsRa, kFsRb[k], kFsC, fs[k]);

    // --- hit ---
    const double vhit = hit ? kTtlHi : kTtlLo;
    const double i_d1 = std::max(0.0, (vhit - kDiode - c21_) / kR89);
    const double gin = noise_ ? 1.0 / (kR35 + kRon) : 0.0;
    const double a = 1.0 / (kR37 * kC23);
    const double k = kC23 / kC22;
    const double g = gin + 1.0 / kR36;
    // Backward Euler on the multiple-feedback band-pass: dv1 = -a (vo - Vref),
    // C22 (dv1 - dvo) = gin (Vc21 - v1) - v1 / R36 - C23 dv1.
    const double lhs = 1.0 + kH * (1.0 + k) * a + kH * kH * g * a / kC22;
    const double rhs = bp_vo_ + kH * (1.0 + k) * a * kVref - kH * gin * c21_ / kC22 +
                       (kH * g / kC22) * (bp_v1_ + kH * a * kVref);
    double vo = rhs / lhs;
    const double v1 = bp_v1_ - kH * a * (vo - kVref);
    bp_vo_ = std::clamp(vo, 0.0, kOpHi);
    bp_v1_ = v1;
    c21_ += kH * (i_d1 - gin * (c21_ - v1)) / kC21;

    // --- fire ---
    const double v8l = fire ? kTtlLo : kTtlHi;
    const double vn = noise_ ? kTtlHi : kTtlLo;
    {
        // Nodes F (C28) and CV (C29) by backward Euler.
        const double a11 = kC28 / kH + 1.0 / kR47 + 1.0 / kR48, a12 = -1.0 / kR48;
        const double b1 = kC28 * c28_ / kH + v8l / kR47;
        const double a21 = -1.0 / kR48, a22 = kC29 / kH + 1.0 / kR48 + 1.0 / kR46 + 1.0 / kCvRth;
        const double b2 = kC29 * fire_cv_ / kH + vn / kR46 + kCvThev / kCvRth;
        const double det = a11 * a22 - a12 * a21;
        c28_ = (b1 * a22 - a12 * b2) / det;
        fire_cv_ = (a11 * b2 - a21 * b1) / det;
    }
    const double sw = astable(fire555_, fire_cv_, kFireRa, kFireRb, kFireC, true);
    const double i_d2 = std::max(0.0, ((fire ? kTtlHi : kTtlLo) - kDiode - c25_) / kR90);
    c25_ += kH * (i_d2 - sw * c25_ / (kR41 + kRon)) / kC25;
    const double vfol = std::min(kOpHi, sw * c25_ * kR41 / (kR41 + kRon));

    // --- mix: node B (C20) and node M by backward Euler ---
    const double r26 = kR43 + kH / kC26;
    const double r46 = kR92 + kH / kC46;
    double gm = 1.0 / kR34 + 1.0 / kR51 + 1.0 / kR50 + 1.0 / kR40 + 1.0 / r26 + 1.0 / kR91 + 1.0 / r46;
    double im = ttl(qa) / kR51 + ttl(qc) / kR50 + bp_vo_ / kR40 + (vfol - c26_) / r26 + c46_ / r46;
    if (vol[0]) {
        gm += 1.0 / (kR49 + kRon);
        im += ttl(qc) / (kR49 + kRon);
    }
    if (vol[1]) {
        gm += 1.0 / (kR52 + kRon);
        im += ttl(qd) / (kR52 + kRon);
    }
    const double b11 = kC20 / kH + 3.0 / kR24 + 1.0 / kR34, b12 = -1.0 / kR34;
    const double c1 = kC20 * node_b_ / kH + fs_sum / kR24;
    const double b21 = -1.0 / kR34, b22 = gm;
    const double det = b11 * b22 - b12 * b21;
    node_b_ = (c1 * b22 - b12 * im) / det;
    const double vm = (b11 * im - b21 * c1) / det;
    c26_ += kH * ((vfol - vm - c26_) / r26) / kC26;
    const double i46 = (vm - c46_) / r46;
    c46_ += kH * i46 / kC46;
    out_ = i46 * kR92;
    sum_ += out_;
    n_++;
}

float DiscreteSound::mix() {
    const double v = n_ ? sum_ / n_ : out_;
    sum_ = 0;
    n_ = 0;
    return float(v);
}

}  // namespace galaxian
