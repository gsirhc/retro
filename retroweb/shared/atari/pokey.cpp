#include "pokey.h"

#include <cmath>

namespace atari {

namespace {

// AUDC bits, MAME src/devices/sound/pokey.h (NOTPOLY5/POLY4/PURE).
constexpr uint8_t kVolMask = 0x0F;
constexpr uint8_t kVolOnly = 0x10;
constexpr uint8_t kPure = 0x20;
constexpr uint8_t kPoly4 = 0x40;
constexpr uint8_t kNotPoly5 = 0x80;
// AUDCTL bit 7: 9-bit poly for noise / RANDOM width.
constexpr uint8_t kPoly9 = 0x80;

}  // namespace

void Pokey::reset() {
    audf_.fill(0);
    audc_.fill(0);
    audctl_ = 0;
    skctl_ = 0;
    period_.fill(28);
    count_.fill(28);
    out_.fill(false);
    // Non-zero seeds, an all-zero 17-bit LFSR stays stuck, and Centipede's
    // mushroom field is planted from RANDOM (MAME pokey.cpp starts filled).
    poly17_ = 0x1FFFF;
    poly9_ = 0x1FF;
    poly5_ = 0x1F;
    poly4_ = 0xF;
    sample_acc_ = 0;
    rc_raw_ = 0xFFFF;
    rc_sum_ = 0;
    rc_n_ = 0;
    if (rc_) {
        rc_target(0);
        rc_v_ = rc_v0_;
    }
    recalc_periods();
}

void Pokey::recalc_periods() {
    // Base divider 28 (~64 kHz) or 114 (~15 kHz) from AUDCTL bit 0.
    int base = (audctl_ & 0x01) ? 114 : 28;
    for (int i = 0; i < 4; i++) {
        int p = int(audf_[i]) + 4;
        // 1.79 MHz clock select on ch 1/3 (AUDCTL bits 6/5), rare in Centipede.
        if ((i == 0 && (audctl_ & 0x40)) || (i == 2 && (audctl_ & 0x20)))
            p = int(audf_[i]) + 4;
        else
            p = (int(audf_[i]) + 1) * base;
        if (p < 1) p = 1;
        period_[i] = p;
        wrap_[i] = 0;
    }
    // 16-bit join: ch1+ch2 / ch3+ch4 (AUDCTL bits 4 / 3). +7 only on 1.79 MHz,
    // else AUDF+1 base clocks (MAME pokey.cpp, from the Atari manual).
    // A linked low channel borrows at its AUDF, then every 256 as it wraps (MAME).
    for (int lo : {0, 2}) {
        if (!(audctl_ & (lo == 0 ? 0x10 : 0x08))) continue;
        bool fast = audctl_ & (lo == 0 ? 0x40 : 0x20);
        int unit = fast ? 1 : base;
        int f = (int(audf_[lo + 1]) << 8) | audf_[lo];
        period_[lo] = fast ? audf_[lo] + 7 : (audf_[lo] + 1) * base;
        wrap_[lo] = 256 * unit;
        period_[lo + 1] = fast ? f + 7 : (f + 1) * base;
    }
    for (int i = 0; i < 4; i++) {
        int limit = wrap_[i] ? wrap_[i] : period_[i];
        if (count_[i] > limit) count_[i] = limit;
    }
}

bool Pokey::joined_high(int ch) const {
    return (ch == 1 && (audctl_ & 0x10)) || (ch == 3 && (audctl_ & 0x08));
}

void Pokey::step_polys() {
    // 17-bit: x^17 + x^12 + 1 (MAME / Altirra).
    uint32_t b17 = ((poly17_ >> 16) ^ (poly17_ >> 11)) & 1;
    poly17_ = ((poly17_ << 1) | b17) & 0x1FFFF;
    uint32_t b9 = ((poly9_ >> 8) ^ (poly9_ >> 3)) & 1;
    poly9_ = ((poly9_ << 1) | b9) & 0x1FF;
    uint32_t b5 = ((poly5_ >> 4) ^ (poly5_ >> 2)) & 1;
    poly5_ = ((poly5_ << 1) | b5) & 0x1F;
    uint32_t b4 = ((poly4_ >> 3) ^ (poly4_ >> 2)) & 1;
    poly4_ = ((poly4_ << 1) | b4) & 0xF;
}

void Pokey::tick_channel(int ch) {
    if (--count_[ch] > 0) return;
    count_[ch] = wrap_[ch] ? wrap_[ch] : period_[ch];
    if (joined_high(ch)) count_[ch - 1] = period_[ch - 1];

    uint8_t c = audc_[ch];
    if (c & kVolOnly) {
        out_[ch] = true;
        return;
    }

    // Poly5 gates edges when NOTPOLY5 is clear; miss leaves the latch alone
    // (forcing low here chopped pure tones into scratchy dropouts).
    if (!((c & kNotPoly5) || (poly5_ & 1))) return;

    if (c & kPure)
        out_[ch] = !out_[ch];
    else if (c & kPoly4)
        out_[ch] = (poly4_ & 1) != 0;
    else if (audctl_ & kPoly9)
        out_[ch] = (poly9_ & 1) != 0;
    else
        out_[ch] = (poly17_ & 1) != 0;
}

float Pokey::sample() const {
    float s = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t vol = audc_[i] & kVolMask;
        if (vol == 0) continue;
        if ((audc_[i] & kVolOnly) || out_[i])
            s += float(vol) / 15.0f;
    }
    return s * 0.22f;
}

uint8_t Pokey::read(uint8_t reg) const {
    reg &= 0x0F;
    if (reg == 0x0A) {
        // RANDOM. SKCTL bits 1:0 == 00 resets polys to 0 → read 0xFF.
        if ((skctl_ & 0x03) == 0) return 0xFF;
        return random();
    }
    if (reg == 0x08) return allpot;
    return 0xFF;
}

void Pokey::write(uint8_t reg, uint8_t v) {
    reg &= 0x0F;
    switch (reg) {
        case 0x00: case 0x02: case 0x04: case 0x06:
            audf_[reg >> 1] = v;
            recalc_periods();
            break;
        case 0x01: case 0x03: case 0x05: case 0x07:
            audc_[reg >> 1] = v;
            break;
        case 0x08:
            audctl_ = v;
            recalc_periods();
            break;
        case 0x0F: {
            uint8_t prev = skctl_;
            skctl_ = v;
            // Init mode (bits 1:0 == 00): RANDOM reads as $FF. Leaving init
            // re-seeds if the LFSR was cleared so it cannot stick at zero.
            if ((v & 0x03) == 0) {
                poly17_ = poly9_ = poly5_ = poly4_ = 0;
            } else if ((prev & 0x03) == 0) {
                if (poly17_ == 0) poly17_ = 0x1FFFF;
                if (poly9_ == 0) poly9_ = 0x1FF;
                if (poly5_ == 0) poly5_ = 0x1F;
                if (poly4_ == 0) poly4_ = 0xF;
            }
            break;
        }
        default:
            break;
    }
}

uint16_t Pokey::out_raw() const {
    uint16_t raw = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t vol = audc_[i] & kVolMask;
        if ((audc_[i] & kVolOnly) || out_[i]) raw = uint16_t(raw | (vol << (4 * i)));
    }
    return raw;
}

void Pokey::set_output_rc(double r_pullup, double cap, double vref) {
    rc_ = true;
    r_pullup_ = r_pullup;
    cap_ = cap;
    vref_ = vref;
    rc_raw_ = 0xFFFF;
    rc_target(0);
    rc_v_ = rc_v0_;
}

void Pokey::rc_target(uint16_t raw) {
    // Volume bits switch in 90k/26.5k/8.05k/3.4k to ground, 8M when off (MAME vol_init).
    static constexpr double kBit[4] = {90000, 26500, 8050, 3400};
    static constexpr double kOff = 8e6;
    if (raw == rc_raw_) return;
    rc_raw_ = raw;
    double g = 0;
    for (int ch = 0; ch < 4; ch++) {
        int nib = (raw >> (4 * ch)) & 0xF;
        for (int b = 0; b < 4; b++) g += (nib & (1 << b)) ? 1.0 / kBit[b] : 1.0 / kOff;
    }
    const double r = 1.0 / g;
    rc_v0_ = r / (r + r_pullup_) * vref_;
    rc_mult_ = 1.0 - std::exp(-(r + r_pullup_) / (cap_ * r_pullup_ * r) / double(clock_hz_));
}

void Pokey::mix(int cpu_cycles, int out_hz, std::vector<float>& out) {
    if (out_hz <= 0 || cpu_cycles <= 0) return;
    const double cycles_per_sample = double(clock_hz_) / double(out_hz);
    for (int i = 0; i < cpu_cycles; i++) {
        step_polys();
        for (int ch = 0; ch < 4; ch++) tick_channel(ch);
        if (rc_) {
            rc_target(out_raw());
            rc_v_ += (rc_v0_ - rc_v_) * rc_mult_;
            rc_sum_ += rc_v_;
            rc_n_++;
        }
        sample_acc_ += 1.0;
        while (sample_acc_ >= cycles_per_sample) {
            sample_acc_ -= cycles_per_sample;
            if (rc_) {
                out.push_back(float(rc_n_ ? rc_sum_ / rc_n_ : rc_v_));
                rc_sum_ = 0;
                rc_n_ = 0;
            } else {
                out.push_back(sample());
            }
        }
    }
}

}  // namespace atari
