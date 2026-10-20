#include "sound.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace mw8080 {

namespace {

constexpr double kDt = 1.0 / kSampleRate;
constexpr double kV12 = 12.0;
constexpr double kV5 = 5.0;
// LM3900: inputs one VBE up, output 90 mV to V+ less one VBE (AN-72).
constexpr double kVbe = 0.5;
constexpr double kVoh = kV12 - kVbe;
constexpr double kVol = 0.09;
constexpr double kVd = 0.6;
// 7411 typical output levels
constexpr double kTtlHigh = 3.4;
constexpr double kTtlLow = 0.2;

double iin(double v, double r) { return (v - kVbe) / r; }

double norton_bias(double r) { return (kV12 - kVbe) / r; }

// Inverting stage settles where the mirrored + current equals the - current.
double norton_linear(double i_plus, double i_minus, double r_fb) {
    return std::clamp(kVbe + (std::max(i_plus, 0.0) - i_minus) * r_fb, kVol, kVoh);
}

double norton_out(bool high) { return high ? kVoh : kVol; }

struct Parts {
    double logic_high = 0;  // trigger pull-ups: 1K to +5 V (Midway F4), 1K to +12 V (Taito)
    double sn_series = 0;
    analog::NortonOneShot missile_os{};
    double missile_osc_c = 0;
    double missile_out_r = 0;
    analog::NortonOneShot expl_os{};
    double expl_r84 = 0;
    double expl_noise_r = 0;
    double expl_out_r = 0;
    double saw_c = 0;
    double hit3_out_r = 0;
    double hit5_out_r = 0;
    double fleet_out_r = 0;
    double tone_hz = 0;
    double bonus_out_r = 0;
    bool trimmers = false;
};

// Designators are the Midway sheet's; Taito values read off PVR00001.
constexpr Parts kMidway{
    kV5, 47e3,
    {4.7e6, 1e6, 1e6, 1e-6, 2.2e6, 100e3, 470e-12},   // R32 R31 R33 C12 R34 R30 C15
    330e-12,                                          // C10
    150e3,                                            // R28
    {4.7e6, 1e6, 1e6, 2.2e-6, 2.2e6, 100e3, 470e-12}, // R90 R91 R89 C24 R92 R88 C25
    5.6e3, 680e3, 2e3,                                // R84 R79 R86
    0.1e-6,                                           // C16
    200e3,                                            // R59
    200e3,                                            // R78, reads 200K or 280K on the scan
    10e3,                                             // R134
    480,                                              // "480 HZ" into H3
    150e3,                                            // R136
    false,
};

constexpr Parts kTaito{
    kV12, 47e3,
    {4.7e6, 1e6, 1e6, 0.22e-6, 2.2e6, 100e3, 470e-12},
    300e-12,
    150e3,
    // C24 illegible and R79 not drawn on PVR00001: Midway values
    {4.7e6, 1e6, 1e6, 2.2e-6, 2.2e6, 100e3, 470e-12},
    560, 680e3, 2e3,
    0.22e-6,
    47e3,
    47e3,
    68e3,
    960,                                              // "960Hz" into IC12
    220e3,
    true,
};

const Parts& parts(SoundBoard b) { return b == SoundBoard::TaitoPv ? kTaito : kMidway; }

// Taito VR1-VR7: value not on the sheet. 50K like VR8, wiper at mid travel.
constexpr double kTrimPot = 50e3;
constexpr double kTrimWiper = 0.5;
constexpr double kTrimR = 47e3;
constexpr double kTrimTop = (1 - kTrimWiper) * kTrimPot;
constexpr double kTrimBottom = kTrimWiper * kTrimPot;
constexpr double kTrimIn = kTrimTop + kTrimBottom * kTrimR / (kTrimBottom + kTrimR);
constexpr double kTrimRatio = kTrimBottom / (kTrimBottom + kTrimR);

// Summing amp: R104 bias, R105 feedback, R106 volume (mid travel) with C42.
constexpr double kAmpBias = 220e3;
constexpr double kAmpFb = 100e3;
constexpr double kAmpPot = 25e3;
constexpr double kAmpPotC = 1e-6;
constexpr double kAmpDc = kVbe + kAmpFb * (kV12 - kVbe) / kAmpBias;
constexpr double kAmpSwing = kVoh - kAmpDc;

// Two-gate CMOS astable, threshold at VDD/2: T = 2 ln3 R113 C46.
const double kNoiseHz = 1.0 / (2.0 * std::log(3.0) * 56e3 * 0.001e-6);

}  // namespace

namespace analog {

// --- NortonOneShot ---

void NortonOneShot::reset() {
    high = false;
    v_cap = 0;
    v_trig = 0;
}

double NortonOneShot::out() const { return norton_out(high); }

void NortonOneShot::step(double v_line, double dt) {
    const double i_trig = (v_line - kVbe - v_trig) / (r_trig + dt / c_trig);
    v_trig += i_trig * dt / c_trig;
    const double v_out = out();
    v_cap += ((v_out - v_cap) / r_out - iin(v_cap, r_in)) * dt / c;
    v_cap = std::min(v_cap, v_out + kVd);
    const double i_plus = std::max(i_trig, 0.0) + iin(v_out, r_fb);
    const double i_minus = norton_bias(r_bias) + iin(v_cap, r_in);
    high = i_plus > i_minus;
}

// --- NortonRamp ---

void NortonRamp::reset() {
    v_ramp = kVol;
    high = false;
}

double NortonRamp::square() const { return norton_out(high); }

void NortonRamp::step(double i_minus_extra, double dt) {
    const double i_bias = norton_bias(r_bias) + i_minus_extra;
    const double i_ref = norton_bias(r_ref);
    // Ramp slopes are constant within a step, so switch at the exact crossing.
    while (dt > 0) {
        const double v_sq = square();
        const double slope = (std::max(iin(v_sq, r_sq), 0.0) - i_bias) / c;
        const double v_switch = kVbe + r_cmp * (i_ref + iin(v_sq, r_hys));
        const bool toward = high ? slope > 0 : slope < 0;
        const double t = toward ? (v_switch - v_ramp) / slope : dt;
        if (t >= dt) {
            v_ramp = std::clamp(v_ramp + slope * dt, kVol, kVoh);
            break;
        }
        v_ramp += slope * std::max(t, 0.0);
        dt -= std::max(t, 0.0);
        high = !high;
    }
}

// --- NortonVco ---

void NortonVco::reset() {
    v_ramp = kVol;
    high = false;
}

void NortonVco::step(double v_in, double dt) {
    const double v = std::max(v_in - kVbe, 0.0);
    const double i_ref = norton_bias(r_ref);
    while (dt > 0) {
        const double slope = ((high ? 0.0 : v / r_plus) - v / r_minus) / c;
        const double v_switch = kVbe + r_in * (i_ref - iin(norton_out(high), r_hys));
        const bool toward = high ? slope < 0 : slope > 0;
        const double t = toward ? (v_switch - v_ramp) / slope : dt;
        if (t >= dt) {
            v_ramp = std::clamp(v_ramp + slope * dt, kVol, kVoh);
            break;
        }
        v_ramp += slope * std::max(t, 0.0);
        dt -= std::max(t, 0.0);
        high = !high;
    }
}

// --- Astable555 ---

void Astable555::reset() {
    v_cap = 0;
    out = true;
}

void Astable555::step(double v_src, double r_a, double r_b, bool run, double dt) {
    if (!run) {
        out = false;
        v_cap *= std::exp(-dt / (r_b * c));
        return;
    }
    // 1/3 and 2/3 VCC comparators (NE555 datasheet).
    if (out) {
        if (r_a > 0) v_cap = v_src + (v_cap - v_src) * std::exp(-dt / ((r_a + r_b) * c));
        if (v_cap >= v_cc * 2 / 3) out = false;
    } else {
        v_cap *= std::exp(-dt / (r_b * c));
        if (v_cap <= v_cc / 3) out = true;
    }
}

// --- RcLadder ---

void RcLadder::reset() { v1 = v2 = 0; }

double RcLadder::step(double v_in, double dt) {
    const double a = dt / c1, b = dt / c2;
    const double m11 = 1 + a / r1 + a / r2, m12 = -a / r2;
    const double m21 = -b / r2, m22 = 1 + b / r2 + b / r_load;
    const double y1 = v1 + a * v_in / r1, y2 = v2;
    const double det = m11 * m22 - m12 * m21;
    v1 = (y1 * m22 - m12 * y2) / det;
    v2 = (m11 * y2 - m21 * y1) / det;
    return v2 / r_load;
}

// --- Sn76477 ---

// SLF cap swings 0.35 V to 2.5 V on I/2I sources (SN76477 app guide, fig. 2).
constexpr double kSlfLow = 0.35;
constexpr double kSlfHigh = 2.5;
// VCO spans 10:1 over 0 V to 2.5 V of control (app guide, VCO section);
// f ~ 1/(V + V0) puts both ends there.
constexpr double kVcoV0 = kSlfHigh / 9;

void Sn76477::reset() {
    v_slf = kSlfLow;
    slf_rising = true;
    vco_phase = 0;
    vco_out = false;
    inhibit = true;
    latch = false;
}

// Falling pin 9 sets the one-shot latch; pin 24 open never times it out
// (app guide, fig. 7 and 16).
void Sn76477::inhibit_w(bool high) {
    if (inhibit && !high) latch = true;
    inhibit = high;
}

double Sn76477::step(double dt) {
    // SLF frequency 0.64/(R C), equal-sided triangle (SN76477 datasheet eq. 1).
    const double slope = 2 * (kSlfHigh - kSlfLow) * 0.64 / (r_slf * c_slf);
    v_slf += (slf_rising ? slope : -slope) * dt;
    if (v_slf >= kSlfHigh) {
        v_slf = 2 * kSlfHigh - v_slf;
        slf_rising = false;
    } else if (v_slf <= kSlfLow) {
        v_slf = 2 * kSlfLow - v_slf;
        slf_rising = true;
    }
    // f_min 0.64/(R C) at 2.5 V (datasheet eq. 2); pin 19 high gives 50% duty.
    const double f_min = 0.64 / (r_vco * c_vco);
    const double f = f_min * (kSlfHigh + kVcoV0) / (std::min(v_slf, kSlfHigh) + kVcoV0);
    vco_phase += f * dt;
    vco_phase -= std::floor(vco_phase);
    vco_out = vco_phase < 0.5;
    // Peak output 3.4 RF/RG (datasheet eq. 8); pulses fall from the resting level.
    if (inhibit || !latch || !vco_out) return 0;
    return -3.4 * r_fb / r_amp;
}

}  // namespace analog

// --- Sound ---

Sound::Sound(SoundBoard board) : board_(board) {
    const Parts& p = parts(board);
    // R120 C35 R121 C36 R123 R124. PVR00001 pins 11, 17, 18, 20 illegible.
    sn_ = {120e3, 1e-6, 39e3, 0.047e-6, 150e3, 10e3};
    missile_os_ = p.missile_os;
    // C10, R13, R16, R18, R19, R17
    missile_osc_ = {p.missile_osc_c, 3.3e6, 1e6, 1e6, 2.2e6, 560e3};
    expl_os_ = p.expl_os;
    // R84 C27 R85 C28, R86 into the bus or the trimmer
    expl_rc_ = {p.expl_r84, 0.1e-6, 6.8e3, 0.1e-6, p.expl_out_r + (p.trimmers ? kTrimIn : 0)};
    // C16, R37, R41, R39, R40, R38
    hit3_saw_ = {p.saw_c, 1e6, 10e3, 1e6, 120e3, 100e3};
    // C17, R42, R43, R44, R46, R45
    hit3_vco_ = {330e-12, 1e6, 470e3, 680e3, 1e6, 1e6};
    // R49 R48 R50 C18 R52 R51 C20
    hit3_os_ = {4.7e6, 1e6, 1e6, 0.1e-6, 2.2e6, 100e3, 470e-12};
    // C21, R70, R64, R62, R63, R61
    hit5_tri_ = {0.1e-6, 1e6, 470e3, 1e6, 120e3, 100e3};
    // C22, R65, R66, R67, R69, R68
    hit5_vco_ = {470e-12, 1e6, 470e3, 680e3, 1e6, 1e6};
    fleet_ = {kV5, 0.1e-6};  // C29
    // R132 C31 R133 C32, R134 into the bus or the trimmer
    fleet_rc_ = {100, 4.7e-6, 100, 10e-6, p.fleet_out_r + (p.trimmers ? kTrimIn : 0)};
    bonus_ = {kV5, 1e-6};  // C34
    reset();
}

void Sound::reset() {
    p3_ = p5_ = 0;
    cycle_acc_ = 0;
    ticks_ = 0;
    sum_ = 0;
    count_ = 0;
    last_ = 0;
    noise_phase_ = 0;
    // C47/D11 kick a one into the register at power-up.
    lfsr_ = 1;
    noise_c_ = 0;
    sn_.reset();
    missile_os_.reset();
    missile_ramp_ = kVol;
    missile_osc_.reset();
    missile_gate_ = 0;
    missile_c11_ = 0;
    expl_os_.reset();
    expl_env_ = 0;
    expl_rc_.reset();
    hit3_saw_.reset();
    hit3_vco_.reset();
    hit3_os_.reset();
    hit3_env_ = kVol;
    hit5_tri_.reset();
    hit5_vco_.reset();
    hit5_env_ = kVol;
    fleet_.reset();
    fleet_out_ = kTtlHigh;
    fleet_rc_.reset();
    bonus_.reset();
    tone_phase_ = 0;
    bonus_out_ = kTtlLow;
    std::fill(std::begin(src_i_), std::end(src_i_), 0.0);
    hpf_i_ = hpf_prev_ = 0;
    c42_ = 0;
    amp_v_ = 0;
}

// OUT 3: 0 UFO (inverted onto SN76477 pin 9), 1 missile, 2 explosion, 3 hit,
// 4 bonus, 5 amp enable.
void Sound::port3_w(uint8_t v) {
    p3_ = v;
    sn_.inhibit_w((v & 1) == 0);
}

// OUT 5: 0-3 fleet ladder, 4 hit, 5 flip screen.
void Sound::port5_w(uint8_t v) { p5_ = v; }

void Sound::advance(int cpu_cycles) {
    cycle_acc_ += int64_t(cpu_cycles) * kSampleRate;
    while (cycle_acc_ >= kCpuHz) {
        cycle_acc_ -= kCpuHz;
        tick();
    }
}

float Sound::mix() {
    if (count_ > 0) {
        last_ = float(sum_ / count_);
        sum_ = 0;
        count_ = 0;
    }
    return last_;
}

void Sound::tick() {
    const Parts& p = parts(board_);
    const double dt = kDt;
    ticks_++;

    // 4006 wired as 17 stages, stages 5 XOR 17 fed back, noise from stage 13.
    noise_phase_ += kNoiseHz * dt;
    if (noise_phase_ >= 1) {
        noise_phase_ -= 1;
        const uint32_t fb = ((lfsr_ >> 4) ^ (lfsr_ >> 16)) & 1;
        lfsr_ = ((lfsr_ << 1) | fb) & 0x1FFFF;
    }
    const double v_noise = noise() ? kV12 : 0.0;

    // Into the bus (virtual ground) or into a Taito trimmer.
    const auto into_bus = [&](double i) { return p.trimmers ? i * kTrimRatio : i; };
    const double r_trim = p.trimmers ? kTrimIn : 0.0;

    // --- UFO: SN76477 ---
    src_i_[kUfo] = into_bus(sn_.step(dt) / (p.sn_series + r_trim));

    // --- Missile ---
    const double v_shot = (p3_ & 0x02) ? p.logic_high : 0.0;
    missile_os_.step(v_shot, dt);
    // R35 into the + input, R36 bias, C13
    missile_ramp_ += (std::max(iin(missile_os_.out(), 10e3), 0.0) - norton_bias(1.5e6)) * dt / 0.22e-6;
    missile_ramp_ = std::clamp(missile_ramp_, kVol, kVoh);
    const double v_pitch = std::max(missile_ramp_ - kVd, kVbe);  // D1
    // Noise through C9 into the R10/R11 divider, R12 into the oscillator.
    const double v_c9 = v_noise - noise_c_;
    const double v_div = (v_c9 / 1e6 + kVbe / 1.5e6) / (1 / 1e6 + 1 / 330e3 + 1 / 1.5e6);
    noise_c_ += (v_c9 - v_div) / 1e6 * dt / 0.1e-6;
    missile_osc_.step(iin(v_pitch, 4.7e6) + iin(v_div, 1.5e6), dt);  // R14, R12
    // R15 pitch, R22 oscillator, R20 bias, R21 feedback
    const double v_mod = norton_linear(iin(v_pitch, 470e3), iin(missile_osc_.square(), 560e3) + norton_bias(2.7e6), 560e3);
    // D3 charges C14 from the shot line, R60 bleeds it into the gate
    missile_gate_ = std::max(missile_gate_, v_shot - kVd);
    const double i_gate = std::max(iin(missile_gate_, 560e3), 0.0);
    missile_gate_ -= i_gate * dt / 0.1e-6;
    // R23, R25 bias, R26 feedback
    const double v_missile = norton_linear(i_gate, iin(v_mod, 560e3) + norton_bias(2.7e6), 560e3);
    // C11 in series with R28
    const double i_missile = (v_missile - missile_c11_) / (p.missile_out_r + r_trim + dt / 0.001e-6);
    missile_c11_ += i_missile * dt / 0.001e-6;
    src_i_[kMissile] = into_bus(i_missile);

    // --- Explosion / flash ---
    expl_os_.step((p3_ & 0x04) ? p.logic_high : 0.0, dt);
    // R93 and D6 charge C26, R83 bleeds it into the + input
    const double i_env_in = std::max((expl_os_.out() - kVd - expl_env_) / 10e3, 0.0);
    const double i_env_out = std::max(iin(expl_env_, 680e3), 0.0);
    expl_env_ += (i_env_in - i_env_out) * dt / 1e-6;
    // R80 bias, noise resistor, R82 feedback
    const double v_expl = norton_linear(i_env_out, norton_bias(2.7e6) + iin(v_noise, p.expl_noise_r), 680e3);
    src_i_[kExplosion] = into_bus(expl_rc_.step(v_expl, dt));

    // --- OUT 3 bit 3 hit: saw-swept VCO, one-shot envelope ---
    hit3_saw_.step(0, dt);
    hit3_vco_.step(hit3_saw_.v_ramp, dt);
    hit3_os_.step((p3_ & 0x08) ? p.logic_high : 0.0, dt);
    // R53 into the + input, R137 bias, C19
    hit3_env_ += (std::max(iin(hit3_os_.out(), 10e3), 0.0) - norton_bias(1e6)) * dt / 0.47e-6;
    hit3_env_ = std::clamp(hit3_env_, kVol, kVoh);
    // R54, R56 bias, R55 VCO, R57 feedback
    const double v_hit3 = norton_linear(iin(hit3_env_, 680e3), norton_bias(2.7e6) + iin(hit3_vco_.v_ramp, 470e3), 680e3);
    src_i_[kHit3] = into_bus(v_hit3 / (p.hit3_out_r + r_trim));

    // --- OUT 5 bit 4 hit: triangle-swept VCO, attack/decay while the bit is held ---
    hit5_tri_.step(0, dt);
    hit5_vco_.step(hit5_tri_.v_ramp, dt);
    // 1K pull-up to +12 V and R72 into the + input, R71 bias, C23
    const double i_hit5 = (p5_ & 0x10) ? (kV12 - kVbe) / (1e3 + 100e3) : 0.0;
    hit5_env_ += (i_hit5 - norton_bias(1e6)) * dt / 1e-6;
    hit5_env_ = std::clamp(hit5_env_, kVol, kVoh);
    // R77, R74 bias, R73 VCO, R75 feedback
    const double v_hit5 = norton_linear(iin(hit5_env_, 680e3), norton_bias(2.7e6) + iin(hit5_vco_.v_ramp, 680e3), 680e3);
    src_i_[kHit5] = into_bus(v_hit5 / (p.hit5_out_r + r_trim));

    // --- Fleet / step: 556 charged through whichever ladder lines are high ---
    // 1K pull-ups to +5 V, then R126+R127, R128, R129, R130 and a diode each.
    static constexpr double kLadder[4] = {1e3 + 40e3, 1e3 + 68e3, 1e3 + 82e3, 1e3 + 100e3};
    double g = 0;
    for (int b = 0; b < 4; b++) {
        if (p5_ & (1 << b)) g += 1 / kLadder[b];
    }
    fleet_.step(g > 0 ? kV5 - kVd : 0.0, g > 0 ? 1 / g : 0.0, 75e3, true, dt);  // R131
    fleet_out_ = fleet_.out ? kTtlHigh : kTtlLow;
    src_i_[kFleet] = into_bus(fleet_rc_.step(fleet_out_, dt));

    // --- Bonus / extend play: 556 (R94, R95) gates the tone, reset by the bit ---
    const bool bonus_on = (p3_ & 0x10) != 0;
    bonus_.step(kV5, 100e3, 47e3, bonus_on, dt);
    tone_phase_ += p.tone_hz * dt;
    tone_phase_ -= std::floor(tone_phase_);
    bonus_out_ = (bonus_on && bonus_.out && tone_phase_ < 0.5) ? kTtlHigh : kTtlLow;
    src_i_[kBonus] = into_bus(bonus_out_ / (p.bonus_out_r + r_trim));

    // --- Summing amp ---
    double i_bus = 0;
    for (double i : src_i_) i_bus += i;
    // Coupling into the - input (C44 10 uF Midway, 1 uF Taito) against the bus's own resistance.
    double g_bus = 1 / (p.sn_series + r_trim) + 1 / (p.missile_out_r + r_trim) + 1 / (p.expl_out_r + r_trim)
        + 1 / (p.hit3_out_r + r_trim) + 1 / (p.hit5_out_r + r_trim) + 1 / (p.fleet_out_r + r_trim) + 1 / (p.bonus_out_r + r_trim);
    const double rc = (p.trimmers ? 1e-6 : 10e-6) / g_bus;
    hpf_i_ = rc / (rc + dt) * (hpf_i_ + i_bus - hpf_prev_);
    hpf_prev_ = i_bus;

    double v = 0;
    if (amp_enabled()) {
        v = (-hpf_i_ + c42_ / kAmpPot) / (1 / kAmpFb + 1 / kAmpPot);
        v = std::clamp(v, kVol - kAmpDc, kVoh - kAmpDc);
    }
    c42_ += (v - c42_) / kAmpPot * dt / kAmpPotC;
    amp_v_ = v;

    sum_ += std::clamp(v / kAmpSwing, -1.0, 1.0);
    count_++;
}

}  // namespace mw8080
