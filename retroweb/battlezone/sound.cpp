#include "sound.h"

#include <algorithm>
#include <cmath>

namespace battlezone {

namespace {

constexpr double kVcc = 5.0;
constexpr double kRail = 22.0 - 1.5;    // LM324 on +22 V, output high limit
constexpr double kRon = 470;            // CD4066B r_on, VDD = 5 V typical
constexpr double kVoh = 3.4, kVol = 0.2; // LS TTL typical levels
constexpr double kDiode = 0.6;          // 1N914
constexpr double kNoiseHz = 12096000.0 / 1024;  // E12KHZ, N5 LS393 QB

constexpr double kR5 = 1e3, kR6 = 4.7e3, kR7 = 1e3, kR8 = 100e3, kR9 = 22e3;
constexpr double kC13 = 10e-6;
// F3 555: R10 to the discharge pin, R11 set by ear to a moderate idle (mid travel).
constexpr double kR10 = 100e3, kR11 = 125e3, kC11 = 0.015e-6;
constexpr double kR20 = 33e3, kC29 = 0.47e-6;
constexpr double kR25 = 100e3, kR26 = 33e3, kR27 = 330e3, kR28 = 100e3, kR29 = 22e3;
constexpr double kMixGain = 1.0 + 100e3 / 10e3;  // K5: R31 / R30
constexpr double kR23 = 100e3, kR24 = 100e3, kC20 = 0.1e-6;
constexpr double kOutHp = 10.0;         // cabinet amplifier input coupling
constexpr double kFullScale = 20.0;

// C9/R14/R15/R13/R12/R32/C21 and C14/R17/R16/R18/R19/R33/C22.
constexpr Sound::BurstParts kShell{4.7e-6, 22e3, 1e3, 10e3, 33e3, 330e3, 0.0047e-6};
constexpr Sound::BurstParts kExpl{10e-6, 22e3, 1e3, 10e3, 33e3, 330e3, 0.0047e-6};

}  // namespace

void Sound::reset() {
    latch = 0;
    lfsr_ = 0;
    noise_clk_ = 0;
    noise_div_ = false;
    shell_ff_ = expl_ff_ = false;
    qh_ = false;
    nand4_ = true;
    shell_ = Burst{};
    expl_ = Burst{};
    c13_ = 0;
    c11_ = 0;
    f3_high_ = true;
    f4_ = f5_ = 0;
    c29_ = 0;
    c20_ = 0;
    aud_plus_ = 0;
    dc_ = 0;
}

void Sound::set_rate(int hz) {
    hz_ = hz > 0 ? hz : 48000;
    sub_ = std::max(1, int(std::ceil(192000.0 / hz_)));
    dt_ = 1.0 / (double(hz_) * sub_);
}

// H5 then H4 LS164, cleared while SOUND ENABLE is low; J4 LS20 and two LS74 toggles.
void Sound::clock_noise() {
    if (latch & 0x20) {
        bool b14 = (lfsr_ >> 14) & 1, b3 = (lfsr_ >> 3) & 1;
        lfsr_ = uint16_t((lfsr_ << 1) | (b14 == b3 ? 1 : 0));
    } else {
        lfsr_ = 0;
    }
    bool qh = (lfsr_ >> 15) & 1;
    bool nand4 = ((lfsr_ >> 11) & 0xF) != 0xF;
    if (qh && !qh_) shell_ff_ = !shell_ff_;
    if (nand4 && !nand4_) expl_ff_ = !expl_ff_;
    qh_ = qh;
    nand4_ = nand4;
}

double Sound::engine_hz() const {
    double v = std::min(engine_cv(), kVcc - 0.01);
    if (v <= 0.02) v = 0.02;
    double hi = (kR10 + kR11) * kC11 * std::log((kVcc - v / 2) / (kVcc - v));
    double lo = kR11 * kC11 * std::log(2.0);
    return 1.0 / (hi + lo);
}

// 4066 charges the hold cap while enabled; the noise gate dumps it through
// the divider into a non-inverting LM324 whose gain the L/S switch raises.
void Sound::step_burst(Burst& b, const BurstParts& p, bool enable, bool gate, bool loud) {
    double r_dump = kRon + p.r_series + p.r_shunt;
    double i = (enable ? (kVcc - b.cap) / kRon : 0) - (gate ? b.cap / r_dump : 0);
    b.cap += dt_ * i / p.c_hold;
    double vplus = gate ? b.cap * p.r_shunt / r_dump : 0;
    double rg = p.r13 + (loud ? p.r12 * kRon / (p.r12 + kRon) : p.r12);
    b.vf += dt_ * (vplus / rg - b.vf / p.rf) / p.cf;
    b.out = std::clamp(vplus + b.vf, 0.0, kRail);
}

void Sound::step_555() {
    double v = engine_cv();
    if (f3_high_) {
        c11_ += (kVcc - c11_) * (1 - std::exp(-dt_ / ((kR10 + kR11) * kC11)));
        if (c11_ >= v) f3_high_ = false;
    } else {
        c11_ -= c11_ * (1 - std::exp(-dt_ / (kR11 * kC11)));
        if (c11_ <= v / 2) {
            f3_high_ = true;
            clock_counters();
        }
    }
}

// F4 loads 4 and F5 loads 6 after 15: divide by 12 and 10. MOTOR EN clears both.
void Sound::clock_counters() {
    if (!(latch & 0x80)) return;
    f4_ = f4_ == 15 ? 4 : f4_ + 1;
    f5_ = f5_ == 15 ? 6 : f5_ + 1;
}

void Sound::step(double pokey) {
    noise_clk_ += dt_ * kNoiseHz;
    while (noise_clk_ >= 1) {
        noise_clk_ -= 1;
        noise_div_ = !noise_div_;
        if (noise_div_) clock_noise();
    }

    step_burst(shell_, kShell, latch & 0x04, shell_ff_, latch & 0x08);
    step_burst(expl_, kExpl, latch & 0x01, expl_ff_, latch & 0x02);

    double g_gnd = 1 / kR6 + ((latch & 0x10) ? 1 / (kR5 + kRon) : 0);
    double rth = 1 / (1 / kR7 + g_gnd);
    double vth = kVcc / kR7 * rth;
    double va = (vth / rth + c13_ / kR8) / (1 / rth + 1 / kR8);
    if (va - c13_ > kDiode)
        va = (vth / rth + c13_ / kR8 + (c13_ + kDiode) / kR9) / (1 / rth + 1 / kR8 + 1 / kR9);
    double i13 = (va - c13_) / kR8 + std::max(0.0, (va - c13_ - kDiode) / kR9);
    c13_ += dt_ * i13 / kC13;

    if (!(latch & 0x80)) f4_ = f5_ = 0;
    step_555();

    bool on = latch & 0x20;
    double gi = shell_.out / kR25 + expl_.out / kR28 + c29_ / kR26 + pokey / kR27;
    double gs = 1 / kR25 + 1 / kR28 + 1 / kR26 + 1 / kR27;
    double s = gi / (gs + (on ? 1 / (kRon + kR29) : 0));
    double vplus = on ? s * kR29 / (kRon + kR29) : 0;

    auto lvl = [](bool b) { return b ? kVoh : kVol; };
    double i29 = (lvl(f4_ & 8) + lvl(f4_ == 15) + lvl(f5_ & 8) + lvl(f5_ == 15) - 4 * c29_) / kR20 +
                 (s - c29_) / kR26;
    c29_ += dt_ * i29 / kC29;

    aud_plus_ = std::clamp(vplus * kMixGain, 0.0, kRail);
    double u = aud_plus_ - kVcc - c20_;
    c20_ += dt_ * u / (kR24 * kC20);
}

float Sound::sample(float pokey) {
    double acc = 0;
    for (int i = 0; i < sub_; i++) {
        step(pokey);
        double u = aud_plus_ - kVcc - c20_;
        double aud_minus = kVcc - u * kR23 / kR24;
        acc += aud_plus_ - aud_minus;
    }
    double diff = acc / sub_;
    dc_ += (diff - dc_) * (1 - std::exp(-2 * M_PI * kOutHp / hz_));
    return float(std::clamp((diff - dc_) / kFullScale, -1.0, 1.0));
}

}  // namespace battlezone
