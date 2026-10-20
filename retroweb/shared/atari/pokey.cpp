#include "pokey.h"

#include <algorithm>
#include <cmath>

namespace atari {

namespace {

// 1.78979 MHz / 63.9210 kHz and / 15.6999 kHz (data sheet sh. 6).
constexpr int kDiv64k = 28;
constexpr int kDiv15k = 114;

// Unit device fitted to the AUD VOL limits with 10K to 4.75 V: W/L 1, 2, 4, 8
// give at most 4.2, 3.4, 2.1, 1.2 V (data sheet sh. 27).
constexpr double kUnitSat = 70e-6;
constexpr double kUnitVdsat = 2.7;

}  // namespace

double Pokey::unit_current(double v) {
    if (v <= 0) return 0;
    if (v >= kUnitVdsat) return kUnitSat;
    double x = v / kUnitVdsat;
    return kUnitSat * (2 * x - x * x);
}

void Pokey::set_output_rc(double r_pullup, double cap, double vpull) {
    load_ = Load::Pullup;
    r_ = r_pullup;
    c_ = cap;
    vsrc_ = vpull;
    v_ = vpull;
}

void Pokey::set_output_transimpedance(double rf, double cf, double vref, double vmax) {
    load_ = Load::Transimpedance;
    r_ = rf;
    c_ = cf;
    vsrc_ = vref;
    vmax_ = vmax;
    v_ = vref;
}

void Pokey::reset() {
    audf_.fill(0);
    audc_.fill(0);
    audctl_ = 0;
    skctl_ = 0;
    out_.fill(false);
    hipass_.fill(false);
    prescale_ = 0;
    poly4_ = 0xF;
    poly5_ = 0x1F;
    poly9_ = 0x1FF;
    poly17_ = 0x1FFFF;
    for (int ch = 0; ch < 4; ch++) reload(ch);
    sample_acc_ = 0;
    sum_ = 0;
    sum_n_ = 0;
    v_ = load_ == Load::None ? 0 : vsrc_;
}

bool Pokey::fast(int ch) const {
    if (ch == 0) return (audctl_ & 0x40) != 0;
    if (ch == 2) return (audctl_ & 0x20) != 0;
    return false;
}

bool Pokey::joined_high(int ch) const {
    if (ch == 1) return (audctl_ & 0x10) != 0;
    if (ch == 3) return (audctl_ & 0x08) != 0;
    return false;
}

bool Pokey::joined_low(int ch) const {
    return (ch == 0 || ch == 2) && joined_high(ch + 1);
}

// N = AUDF+1 input clocks; AUDF+4 at 1.79 MHz, AUDF16+7 for a pair there (sh. 7).
int Pokey::period(int ch) const {
    if (joined_low(ch)) {
        int v16 = (int(audf_[size_t(ch + 1)]) << 8) | audf_[size_t(ch)];
        return v16 + (fast(ch) ? 7 : 1);
    }
    return int(audf_[size_t(ch)]) + (fast(ch) ? 4 : 1);
}

void Pokey::reload(int ch) { count_[size_t(ch)] = period(ch); }

void Pokey::pulse(int ch) {
    uint8_t c = audc_[size_t(ch)];
    if (ch == 2) hipass_[0] = out_[0];
    if (ch == 3) hipass_[1] = out_[1];
    // D7 clear: the 5-bit poly gates the divider (sh. 8).
    if (!(c & 0x80) && !(poly5_ & 1)) return;
    bool& o = out_[size_t(ch)];
    if (c & 0x20) {
        o = !o;
    } else if (c & 0x40) {
        o = (poly4_ & 1) != 0;
    } else {
        o = ((audctl_ & 0x80) ? (poly9_ & 1) : (poly17_ & 1)) != 0;
    }
}

void Pokey::step_polys() {
    // Held at all ones in the initialize state (sh. 17, sh. 20).
    if ((skctl_ & 0x03) == 0) {
        poly4_ = 0xF;
        poly5_ = 0x1F;
        poly9_ = 0x1FF;
        poly17_ = 0x1FFFF;
        return;
    }
    poly4_ = ((poly4_ << 1) | (((poly4_ >> 3) ^ (poly4_ >> 2)) & 1)) & 0xF;
    poly5_ = ((poly5_ << 1) | (((poly5_ >> 4) ^ (poly5_ >> 2)) & 1)) & 0x1F;
    poly9_ = ((poly9_ << 1) | (((poly9_ >> 8) ^ (poly9_ >> 3)) & 1)) & 0x1FF;
    poly17_ = ((poly17_ << 1) | (((poly17_ >> 16) ^ (poly17_ >> 11)) & 1)) & 0x1FFFF;
}

void Pokey::clock() {
    step_polys();
    bool base = false;
    if (++prescale_ >= ((audctl_ & 0x01) ? kDiv15k : kDiv64k)) {
        prescale_ = 0;
        base = true;
    }
    for (int lo = 0; lo < 4; lo += 2) {
        int hi = lo + 1;
        bool in = fast(lo) || base;
        if (joined_low(lo)) {
            if (!in) continue;
            int& n = count_[size_t(lo)];
            n--;
            if (n <= 0) {
                reload(lo);
                pulse(lo);
                pulse(hi);
            } else if ((n & 0xFF) == 0xFF) {
                pulse(lo);
            }
            continue;
        }
        bool lo_pulse = false;
        if (in && --count_[size_t(lo)] <= 0) {
            reload(lo);
            pulse(lo);
            lo_pulse = true;
        }
        bool hi_in = joined_high(hi) ? lo_pulse : base;
        if (hi_in && --count_[size_t(hi)] <= 0) {
            reload(hi);
            pulse(hi);
        }
    }
}

int Pokey::units() const {
    int n = 0;
    for (int ch = 0; ch < 4; ch++) {
        uint8_t c = audc_[size_t(ch)];
        bool on = out_[size_t(ch)];
        if (ch == 0 && (audctl_ & 0x04)) on = on != hipass_[0];
        if (ch == 1 && (audctl_ & 0x02)) on = on != hipass_[1];
        // D4 forces the volume circuit's input true (sh. 4-5).
        if (c & 0x10) on = true;
        if (on) n += c & 0x0F;
    }
    return n;
}

double Pokey::step_output() {
    int n = units();
    double dt = 1.0 / clock_hz_;
    switch (load_) {
        case Load::None:
            return n;
        case Load::Pullup: {
            double i = n * unit_current(v_);
            double f = (vsrc_ - v_) / r_ - i;
            double di = n * (unit_current(v_ + 1e-3) - unit_current(v_)) / 1e-3;
            double fp = -1.0 / r_ - di;
            v_ += dt * f / (c_ - dt * fp);
            return v_;
        }
        case Load::Transimpedance: {
            double target = vsrc_ + n * unit_current(vsrc_) * r_;
            double a = 1.0 - std::exp(-dt / (r_ * c_));
            v_ += (target - v_) * a;
            v_ = std::min(v_, vmax_);
            return v_;
        }
    }
    return 0;
}

void Pokey::mix(int chip_clocks, int out_hz, std::vector<float>& out) {
    for (int i = 0; i < chip_clocks; i++) {
        clock();
        sum_ += step_output();
        sum_n_++;
        sample_acc_ += out_hz;
        if (sample_acc_ >= clock_hz_) {
            sample_acc_ -= clock_hz_;
            double s = sum_ / sum_n_;
            if (load_ == Load::None) s /= 60.0;
            out.push_back(float(s));
            sum_ = 0;
            sum_n_ = 0;
        }
    }
}

// High 8 bits of the 17-bit poly, or of the 9-bit one with AUDCTL D7 (sh. 17).
uint8_t Pokey::random() const {
    if (audctl_ & 0x80) return uint8_t(poly9_ >> 1);
    return uint8_t(poly17_ >> 9);
}

uint8_t Pokey::read(uint8_t reg) const {
    switch (reg & 0x0F) {
        case 0x08: return allpot;
        case 0x0A: return random();
        default: break;
    }
    if ((reg & 0x0F) < 8) return 0;
    return 0xFF;
}

void Pokey::write(uint8_t reg, uint8_t v) {
    reg &= 0x0F;
    if (reg < 8) {
        if (reg & 1) audc_[reg >> 1] = v;
        else audf_[reg >> 1] = v;
        return;
    }
    switch (reg) {
        case 0x08:
            audctl_ = v;
            break;
        case 0x09:
            // STIMER reloads the dividers; outputs 1, 2 high and 3, 4 low (sh. 17).
            for (int ch = 0; ch < 4; ch++) reload(ch);
            out_ = {true, true, false, false};
            break;
        case 0x0F:
            skctl_ = v;
            break;
        default:
            break;
    }
}

}  // namespace atari
