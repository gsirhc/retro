#include "opl3.h"

#include <cmath>
#include <cstring>

namespace pc486 {
namespace {

constexpr double kPi = 3.14159265358979323846;

// MULT field -> 2x the real (possibly half-integer) frequency multiplier
// (0.5,1,2,3,4,5,6,7,8,9,10,10,12,12,15,15), per the Yamaha register
// description's MULT3-0 table. Kept as 2x and divided out after the phase
// math so the 0.5 entry needs no separate case.
constexpr uint8_t kMultX2[16] = {
    1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30,
};

// Key-scale-level attenuation at KSL=1 (3 dB/octave, BLOCK=7), indexed by
// the top 4 bits of the channel's F-number, in 0.375 dB units. Derived from
// the Yamaha YMF715x (OPL3-SA3) Register Description Document's KSL
// octave/F-number table (the OCT=7 column), which independently cross-checks
// against the "0,24,32,37,40,43,45,47,48,50,51,52,53,54,55,56" figures
// reported from Niemitalo & Gambrell's YM3812/YMF262 ROM decap notes.
constexpr uint8_t kKslTable[16] = {
    0, 24, 32, 37, 40, 43, 45, 47, 48, 50, 51, 52, 53, 54, 55, 56,
};

// Attack rate (Time 10-90%, ms) and decay/release rate (Time 0-100%, ms) at
// the effective rate index 4-7 -- the Yamaha YMF715x Register Description
// Document's "Rate Value - Actual Time Table" (section 1-5). Every other
// rate index halves the time for each +4, a relationship the printed table
// itself follows exactly (e.g. attack rate 8 is precisely half of rate 4),
// so the rest of the 0-63 range is derived rather than hand-transcribed.
constexpr double kAttack1090Base[4] = {1482.75, 1155.07, 991.23, 868.35};
constexpr double kDecay0to100Base[4] = {39280.64, 31416.32, 26173.44, 22446.08};

// Feedback modulation index in radians, FB=0-7 -- Yamaha YMF715x Register
// Description Document, "FB 2-0" table (0, pi/16, pi/8, pi/4, pi/2, pi, 2pi,
// 4pi).
constexpr double kFeedbackRadians[8] = {
    0.0, kPi / 16.0, kPi / 8.0, kPi / 4.0, kPi / 2.0, kPi, 2.0 * kPi, 4.0 * kPi,
};

// Modulator-to-carrier phase units per radian: the phase accumulator's
// 10.10 fixed-point space covers one full waveform cycle in 2^20 units.
constexpr double kPhaseUnitsPerRadian = 1048576.0 / (2.0 * kPi);
// Full-scale (zero-attenuation) reference for a wave sample -- see WaveTables
// below: the exponential table's implicit leading 1 puts unity at 1024.
constexpr double kFullScale = 1024.0;
// Modulation index (radians) a fully unattenuated modulator applies to its
// carrier's phase in a normal (non-feedback) FM connection. Still not a
// verified figure: the YMF262 datasheet (p.12) and the YMF715x Register
// Description Document (p.9) both tabulate only the feedback case
// (kFeedbackRadians above), and the OPLx decap notes give the algebra without
// any phase-domain scale. One further reading, recorded but deliberately not
// applied: YMF715x §1-1's two equations are "A sin(wc t + B sin wm t)" for the
// normal connection against "A sin(wt + beta FM(t))" for feedback, so B
// carries an implicit coefficient of 1 where feedback carries beta (up to
// 4pi). If A and B share one normalised scale -- which neither document
// states -- an unattenuated modulator would deviate the carrier by ~1 radian
// rather than pi. Changing it on that inference alone is exactly how pi got
// here, so it stands until a real figure turns up. See PC486_REVIEW.md §11.1.
constexpr double kModulationIndexRadians = kPi;

// Log-sin (first quarter-cycle) and exponential ROM tables, regenerated from
// the exact formulas in Niemitalo & Gambrell's "OPLx decapsulated" analysis
// of the YM3812/YMF262 die: logsin[x] = round(-log2(sin((x+0.5)*pi/512))*256),
// exp[x] = round((2^(x/256)-1)*1024). Table entries, not code, are what's
// reproduced from that source.
struct WaveTables {
    uint16_t logsin[256];
    uint16_t exp_tab[256];
    WaveTables() {
        for (int x = 0; x < 256; x++) {
            double angle = (double(x) + 0.5) * kPi / 512.0;
            logsin[x] = uint16_t(std::lround(-std::log2(std::sin(angle)) * 256.0));
            exp_tab[x] = uint16_t(std::lround((std::pow(2.0, double(x) / 256.0) - 1.0) * 1024.0));
        }
    }
};
const WaveTables kWave;

// Maps a register offset (relative to a per-operator range's base, e.g.
// r-0x20 for 20h-35h) to an operator slot 0-17 within a bank, per the
// header's "0-5, 8-13, 16-21" layout. Returns -1 for the gaps (6,7,14,15 and
// anything at or past 22).
int operator_slot(int r) {
    if (r < 0 || r > 21) return -1;
    int group = r / 8;
    int sub = r % 8;
    if (group > 2 || sub > 5) return -1;
    return group * 6 + sub;
}

// Absolute op_[] index for channel-in-bank `cib` (0-8)'s modulator or
// carrier, per the "channel N = slots {a,a+3}" table (Register Setting
// Manner, section 1-6): groups of 3 channels share a group of 6 slots, the
// first 3 modulators and the last 3 carriers.
int op_index(int bank, int cib, bool carrier) {
    int group = cib / 3, cl = cib % 3;
    return bank * 18 + group * 6 + (carrier ? 3 + cl : cl);
}

// RATE = (rate register)*4 + Rof, Rof the key-scale-adjusted offset (KSR=1:
// KSN 0-15 directly; KSR=0: KSN>>2). A rate register of 0 always yields
// RATE 0 (the operator never moves), regardless of key scaling -- both
// exactly per the YMF715x doc's "Rate Key Scale" / "RATE=(RATE Value)x4+Rof"
// note.
int effective_rate(uint8_t reg_rate, int ksn, bool ksr) {
    if (reg_rate == 0) return 0;
    int rks = ksr ? ksn : (ksn >> 2);
    int r = int(reg_rate) * 4 + rks;
    return r > 63 ? 63 : r;
}

// Envelope attenuation is stored in env_level at kEnvScale fixed-point
// precision rather than as whole 0.1875 dB units: the slowest documented
// decay/release rate (~39 s across the 96 dB range) advances by a fraction
// of a unit per output sample, and the header's Operator struct has no
// separate fractional-accumulator field, so env_level itself carries the
// extra bits.
constexpr int32_t kEnvScale = 65536;
constexpr int32_t kEnvMax = 511 * kEnvScale;

// Linear envelope step per output sample for decay/release at effective
// rate `rof`, in kEnvScale units, derived from the datasheet's Decay/Release
// Time(0-100%) table (512 envelope units = the full 96 dB range) via the
// confirmed halve-every-4-steps relationship.
double decay_units_per_sample(int rof) {
    if (rof < 4) return 0.0;  // rates 0-3: held indefinitely
    int group = (rof - 4) / 4;
    int sub = (rof - 4) % 4;
    double t_ms = kDecay0to100Base[sub] / double(1u << group);
    if (t_ms <= 1000.0 / Opl3::kSampleHz) return double(kEnvMax);
    double units_per_ms = 512.0 / t_ms;
    return units_per_ms * (1000.0 / Opl3::kSampleHz) * double(kEnvScale);
}

// Time constant for the exponential attack at effective rate `rof`,
// calibrated from the datasheet's Attack Time(10-90%) table: for
// V(t)=V0*exp(-t/tau), the 10%-of-scale to 90%-of-scale interval is
// tau*(ln(0.1)-ln(0.9))^-1... i.e. tau = T(10-90) / 2.197225.
double attack_tau_seconds(int rof) {
    if (rof < 4) return -1.0;  // never attacks (rate register 0 case)
    int group = (rof - 4) / 4;
    int sub = (rof - 4) % 4;
    double t_ms = kAttack1090Base[sub] / double(1u << group);
    if (t_ms <= 0.05) return 0.0;  // effectively instantaneous
    double tau_ms = t_ms / 2.197225;
    return tau_ms / 1000.0;
}

// Total-level (0.75 dB/step -> 4 envelope units/step) plus key-scale-level
// attenuation, combined in envelope units (0.1875 dB each). KSL formula:
// atten(oct,fnum) = max(0, kKslTable[fnum top4] - 8*(7-oct)) dB-per-octave-
// linear at the KSL=1 (3 dB/oct) setting, scaled per the register's 0/1/2/3
// = 0 / 3 / 1.5 / 6 dB-per-octave per the datasheet's KSL table.
int ksl_env_units(uint8_t ksl_reg, uint16_t fnum, uint8_t block) {
    if (ksl_reg == 0) return 0;
    int fnum4 = (fnum >> 6) & 0x0F;
    int base_0375 = int(kKslTable[fnum4]) - 8 * (7 - int(block));
    if (base_0375 < 0) base_0375 = 0;
    int env_r1 = base_0375 * 2;  // 0.375 dB units -> 0.1875 dB units
    switch (ksl_reg) {
        case 1: return env_r1;
        case 2: return env_r1 / 2;
        default: return env_r1 * 2;  // 3
    }
}

// A phase-modulation offset (in the 20-bit 10.10 phase-accumulator's own
// units) for a modulator whose fully-formed output is `out_val` (roughly
// +-1024, unity at 1024 -- see kFullScale), scaled by a modulation index in
// radians.
int32_t phase_offset(int32_t out_val, double radians) {
    return int32_t(std::lround((double(out_val) / kFullScale) * radians * kPhaseUnitsPerRadian));
}

// Evaluates one of the 8 OPL3 waveforms at a 20-bit phase, combined with
// `atten_env_units` (envelope + TL + KSL + AM, in 0.1875 dB units) via the
// log-sin/exp table pair: look up log-sin, add the (unit-converted)
// attenuation, exponentiate. Waveforms 0-3 are the OPL2 set (full sine, half
// sine, absolute sine, quarter/pulse sine); 4-7 are OPL3-only (double-
// frequency sine and its absolute value, square, exponential-decay
// sawtooth).
int32_t wave_sample(uint8_t waveform, uint32_t phase20, double atten_env_units) {
    uint32_t idx1024 = (phase20 >> 10) & 0x3FF;
    uint32_t quadrant = idx1024 >> 8;
    uint32_t sub = idx1024 & 0xFF;
    uint32_t qidx = (quadrant & 1) ? (255 - sub) : sub;
    int sign = (quadrant >= 2) ? -1 : 1;
    bool silent = false;
    int32_t logsin_units = 0;
    bool use_table = true;

    switch (waveform) {
        case 0:  // full sine
            break;
        case 1:  // half sine: negative half clamped to zero
            if (quadrant >= 2) silent = true;
            break;
        case 2:  // absolute sine: full-wave rectified, always positive
            sign = 1;
            break;
        case 3:  // quarter/pulse sine: rising quarter, repeated twice, silent between
            if (quadrant == 1 || quadrant == 3) silent = true;
            else { qidx = sub; sign = 1; }
            break;
        case 4: {  // even-period-only sine at double frequency
            if (idx1024 >= 512) { silent = true; break; }
            uint32_t d = (idx1024 << 1) & 0x3FF;
            uint32_t dq = d >> 8, dsub = d & 0xFF;
            qidx = (dq & 1) ? (255 - dsub) : dsub;
            sign = (dq >= 2) ? -1 : 1;
            break;
        }
        case 5: {  // absolute value of waveform 4
            if (idx1024 >= 512) { silent = true; break; }
            uint32_t d = (idx1024 << 1) & 0x3FF;
            uint32_t dq = d >> 8, dsub = d & 0xFF;
            qidx = (dq & 1) ? (255 - dsub) : dsub;
            sign = 1;
            break;
        }
        case 6:  // square: full amplitude both halves, envelope still shapes it
            use_table = false;
            logsin_units = 0;
            break;
        default: {  // 7: exponential-decay sawtooth (derived linear-attenuation ramp)
            use_table = false;
            uint32_t li = idx1024 & 511;
            logsin_units = int32_t(li) * 4;
            break;
        }
    }
    if (silent) return 0;
    if (use_table) logsin_units = int32_t(kWave.logsin[qidx]);

    double total = double(logsin_units) + atten_env_units * 8.0;  // 0.1875dB -> logsin's ~0.0235dB units
    int32_t t = int32_t(std::lround(total));
    if (t < 0) t = 0;
    int32_t shift = t >> 8;
    if (shift >= 16) return 0;
    // The exp ROM is read backwards -- the die stores it reversed, so the
    // decode indexes 255-frac (Niemitalo & Gambrell, "OPLx decapsulated");
    // exp_tab rises with its index while `t` is an attenuation. That form
    // carries an inherent factor of two, which the extra shift takes back
    // out to land on kFullScale's unity-at-1024 convention.
    int32_t frac = t & 0xFF;
    int32_t mantissa = int32_t(kWave.exp_tab[255 - frac]) + 1024;
    int32_t mag = mantissa >> (shift + 1);
    return sign * mag;
}

}  // namespace

void Opl3::set_cpu_hz(double hz) {
    if (!(hz > 0.0) || hz == cpu_hz_) return;
    // frame_credit_ is in CPU cycles; scale so the pending fractional frame
    // stays the same wall-clock remainder across a Turbo toggle.
    frame_credit_ *= hz / cpu_hz_;
    cpu_hz_ = hz;
}

void Opl3::reset() {
    std::memset(regs_, 0, sizeof(regs_));
    addr_[0] = addr_[1] = 0;
    new_ = false;
    timer1_preset_ = timer2_preset_ = 0;
    timer1_run_ = timer2_run_ = false;
    timer1_mask_ = timer2_mask_ = false;
    timer1_flag_ = timer2_flag_ = false;
    active_ = false;
    prev_cycles_ = 0;
    frame_credit_ = 0.0;
    samples_.clear();
    trace_.clear();
    trace_max_ = 0;
    for (auto &op : op_) {
        op = Operator();
        op.env_level = kEnvMax;  // env_level is kEnvScale fixed-point -- see kEnvScale
    }
    for (auto &ch : ch_) ch = Channel();
    am_phase_ = vib_phase_ = 0;
    noise_ = 1;
    // Every register clears to 0. A cleared C0h does not mute the channel:
    // the pan bits are ignored entirely while NEW is clear, which is what
    // keeps an OPL2-era driver audible on both outputs (see generate_frame).
}

void Opl3::write_address(int bank, uint8_t v) {
    if (bank < 0 || bank > 1) return;
    addr_[bank] = v;
}

void Opl3::write_data(int bank, uint8_t v) {
    if (bank < 0 || bank > 1) return;
    const uint16_t index = uint16_t(bank * 0x100 + addr_[bank]);
    if (trace_max_ != 0 && trace_.size() < trace_max_) {
        trace_.push_back({prev_cycles_, index, v});
    }
    write_reg(index, v);
}

uint8_t Opl3::status() const {
    // Register 04h's mask bits gate the IRQ line only: a masked timer still
    // sets its own flag here, it just cannot raise bit 7. Bits 4-0 stay 0 --
    // an OPL2 returns 6 in bits 2-1, which is how software tells them apart.
    uint8_t s = 0;
    if (timer1_flag_) s |= 0x40;
    if (timer2_flag_) s |= 0x20;
    if ((timer1_flag_ && !timer1_mask_) || (timer2_flag_ && !timer2_mask_)) s |= 0x80;
    return s;
}

std::vector<Opl3::Sample> Opl3::drain_samples() {
    std::vector<Sample> out(samples_.begin(), samples_.end());
    samples_.clear();
    return out;
}

void Opl3::start_trace(std::size_t max_events) {
    trace_max_ = max_events;
    trace_.clear();
    trace_.reserve(max_events);
}

std::vector<Opl3::TraceEvent> Opl3::drain_trace() {
    std::vector<TraceEvent> out(trace_.begin(), trace_.end());
    trace_.clear();
    return out;
}

void Opl3::write_reg(uint16_t index, uint8_t v) {
    if (index >= 0x200) return;
    const int bank = index >> 8;
    const int r = index & 0xFF;
    // A bank-1 write while NEW is clear reaches no register at all, so it must
    // not land in regs_ either -- 105h itself is the one exception, since
    // otherwise OPL3 mode could never be switched on.
    if (bank == 1 && !new_ && index != 0x105) return;
    regs_[index] = v;

    if (bank == 0 && r == 0x02) { timer1_preset_ = v; return; }
    if (bank == 0 && r == 0x03) { timer2_preset_ = v; return; }
    if (bank == 0 && r == 0x04) {
        if (v & 0x80) {
            timer1_flag_ = false;
            timer2_flag_ = false;
        } else {
            timer1_mask_ = (v & 0x40) != 0;
            timer2_mask_ = (v & 0x20) != 0;
            timer1_run_ = (v & 0x01) != 0;
            timer2_run_ = (v & 0x02) != 0;
        }
        recompute_active();
        return;
    }
    if (index == 0x105) { new_ = (v & 0x01) != 0; return; }

    if (r >= 0x20 && r <= 0x35) {
        int slot = operator_slot(r - 0x20);
        if (slot < 0) return;
        Operator &op = op_[bank * 18 + slot];
        op.am = (v >> 7) & 1; op.vib = (v >> 6) & 1; op.egt = (v >> 5) & 1;
        op.ksr = (v >> 4) & 1; op.mult = v & 0x0F;
        return;
    }
    if (r >= 0x40 && r <= 0x55) {
        int slot = operator_slot(r - 0x40);
        if (slot < 0) return;
        Operator &op = op_[bank * 18 + slot];
        op.ksl = (v >> 6) & 0x03; op.tl = v & 0x3F;
        return;
    }
    if (r >= 0x60 && r <= 0x75) {
        int slot = operator_slot(r - 0x60);
        if (slot < 0) return;
        Operator &op = op_[bank * 18 + slot];
        op.ar = (v >> 4) & 0x0F; op.dr = v & 0x0F;
        return;
    }
    if (r >= 0x80 && r <= 0x95) {
        int slot = operator_slot(r - 0x80);
        if (slot < 0) return;
        Operator &op = op_[bank * 18 + slot];
        op.sl = (v >> 4) & 0x0F; op.rr = v & 0x0F;
        return;
    }
    if (r >= 0xE0 && r <= 0xF5) {
        int slot = operator_slot(r - 0xE0);
        if (slot < 0) return;
        Operator &op = op_[bank * 18 + slot];
        op.waveform = v & (new_ ? 0x07 : 0x03);
        return;
    }
    if (r >= 0xA0 && r <= 0xA8) {
        Channel &ch = ch_[bank * 9 + (r - 0xA0)];
        ch.fnum = uint16_t((ch.fnum & 0x300) | v);
        return;
    }
    if (r >= 0xB0 && r <= 0xB8) {
        int cib = r - 0xB0;
        Channel &ch = ch_[bank * 9 + cib];
        ch.fnum = uint16_t((ch.fnum & 0xFF) | ((v & 0x03) << 8));
        ch.block = (v >> 2) & 0x07;
        bool kon = (v >> 5) & 1;
        bool rhythm_channel = (bank == 0) && (cib == 6 || cib == 7 || cib == 8);
        bool rhythm_active = (regs_[0xBD] & 0x20) != 0;
        if (!(rhythm_channel && rhythm_active)) ch.key_on = kon;
        recompute_active();
        return;
    }
    if (bank == 0 && r == 0xBD) {
        recompute_active();
        return;
    }
    if (r >= 0xC0 && r <= 0xC8) {
        Channel &ch = ch_[bank * 9 + (r - 0xC0)];
        ch.out_right = (v >> 5) & 1;
        ch.out_left = (v >> 4) & 1;
        ch.feedback = (v >> 1) & 0x07;
        ch.connection = v & 0x01;
        return;
    }
    if (bank == 1 && r == 0x04) {
        static constexpr int kPrimary[6] = {0, 1, 2, 9, 10, 11};
        for (int i = 0; i < 6; i++) {
            bool en = (v >> i) & 1;
            ch_[kPrimary[i]].four_op_primary = en;
            ch_[kPrimary[i] + 3].four_op_secondary = en;
        }
        return;
    }
}

// The header gives each timer only a preset/run/mask/flag, no running
// counter -- so rather than accumulating a per-call fractional count (which
// would need a persistent field this class doesn't have), overflow is
// detected as a boundary-crossing count against an absolute, cycle-0-
// anchored tick grid: tick number t = floor(cpu_cycle / period). A timer
// with preset P reloads every N=256-P ticks, so under a grid that always
// starts a hypothetical countdown at tick 0, tick t is an overflow exactly
// when t % N == 0. This gets the steady-state period exactly right; the
// only inexactness is up to one tick's worth of phase slip in when the
// *first* overflow lands after a timer is (re)started off-grid, which the
// AdLib detection sequence's own "wait at least 80us" already tolerates.
void Opl3::step_timers(uint32_t frames) {
    if (frames == 0) return;
    const double cycles_per_frame = cpu_hz_ / kSampleHz;
    // frame_credit_'s per-call remainder means this call's span doesn't
    // land exactly where the previous call's left off; over-extend the
    // window by a couple of frames so consecutive calls' windows always
    // overlap rather than risk a gap that permanently skips a boundary --
    // re-checking already-seen ticks is harmless (t % n == 0 is idempotent).
    const double span = double(frames) * cycles_per_frame + 2.0 * cycles_per_frame;
    const double end_cycle = double(prev_cycles_);  // tick() already set this to "now"
    const double start_cycle = end_cycle > span ? end_cycle - span : 0.0;

    if (timer1_run_) {
        const double period = cpu_hz_ * 80.8e-6;
        const long long n = 256 - (long long)(timer1_preset_);
        long long t0 = (long long)std::floor(start_cycle / period);
        long long t1 = (long long)std::floor(end_cycle / period);
        if (t1 > t0 && (t1 - t0) < 100000) {  // defensive bound on a pathological span
            for (long long t = t0 + 1; t <= t1; ++t) {
                if (n <= 0 || (t % n) == 0) timer1_flag_ = true;
            }
        }
    }
    if (timer2_run_) {
        const double period = cpu_hz_ * 323.1e-6;
        const long long n = 256 - (long long)(timer2_preset_);
        long long t0 = (long long)std::floor(start_cycle / period);
        long long t1 = (long long)std::floor(end_cycle / period);
        if (t1 > t0 && (t1 - t0) < 100000) {
            for (long long t = t0 + 1; t <= t1; ++t) {
                if (n <= 0 || (t % n) == 0) timer2_flag_ = true;
            }
        }
    }
}

void Opl3::recompute_active() {
    bool any = timer1_run_ || timer2_run_;
    // A just-keyed-on channel still has every operator in kOff: envelopes only
    // leave kOff inside generate_frame, which does not run while active_ is
    // clear. So key_on has to count here, or key-on would deadlock against
    // tick()'s early-out and the channel would never sound.
    if (!any) {
        for (const auto &ch : ch_) {
            if (ch.key_on) { any = true; break; }
        }
    }
    if (!any && (regs_[0xBD] & 0x20) != 0 && (regs_[0xBD] & 0x1F) != 0) any = true;
    if (!any) {
        for (const auto &op : op_) {
            if (op.env != Env::kOff) { any = true; break; }
        }
    }
    active_ = any;
}

void Opl3::advance(uint64_t cpu_cycles, uint64_t delta) {
    frame_credit_ += double(delta);
    const double cycles_per_frame = cpu_hz_ / kSampleHz;
    uint64_t due = uint64_t(frame_credit_ / cycles_per_frame);
    if (due == 0) { recompute_active(); return; }
    // Consume the credit for every frame that came due BEFORE bounding the
    // work below. Capping first and leaving the remainder in frame_credit_
    // makes the chip's clock fall permanently behind whenever one call asks
    // for a big catch-up -- the shortfall accumulates call after call and the
    // music audibly slows down and keeps slowing. Dropping a slice of audio
    // is recoverable; running the chip slow is not.
    frame_credit_ -= double(due) * cycles_per_frame;
    if (due > kMaxCatchUpFrames) due = kMaxCatchUpFrames;
    const double span = double(due) * cycles_per_frame;
    uint64_t start = double(cpu_cycles) > span ? cpu_cycles - uint64_t(span) : 0;
    for (uint64_t i = 0; i < due; i++) {
        generate_frame(start + uint64_t(double(i) * cycles_per_frame));
    }
    step_timers(uint32_t(due));
    recompute_active();
}

void Opl3::generate_frame(uint64_t cycle) {
    // LFOs: am_phase_/vib_phase_ are 32-bit fixed-point accumulators, one
    // full lap = 2^32, so the increment for a target frequency is
    // freq/kSampleHz of that span.
    constexpr double kFull = 4294967296.0;
    am_phase_ += uint32_t(std::llround((3.7 / kSampleHz) * kFull));
    vib_phase_ += uint32_t(std::llround((6.1 / kSampleHz) * kFull));
    double am01 = double(am_phase_) / kFull;
    double vib01 = double(vib_phase_) / kFull;
    double am_tri = am01 < 0.5 ? am01 * 2.0 : 2.0 - am01 * 2.0;              // 0..1
    double vib_tri = (vib01 < 0.5 ? vib01 * 2.0 : 2.0 - vib01 * 2.0) * 2.0 - 1.0;  // -1..1
    bool dam = (regs_[0xBD] & 0x80) != 0;
    bool dvb = (regs_[0xBD] & 0x40) != 0;
    double am_units_this_frame = am_tri * ((dam ? 4.8 : 1.0) / 0.1875);
    double vib_cents = dvb ? 14.0 : 7.0;
    double vib_ratio_this_frame = 1.0 + vib_tri * (std::pow(2.0, vib_cents / 1200.0) - 1.0);

    // Advances one operator's envelope and phase generator for this frame,
    // given the key state it should currently see (channel KON, or a
    // rhythm-instrument bit for channels 6-8 in rhythm mode).
    auto advance_env_phase = [this, vib_ratio_this_frame](int opidx, uint16_t fnum, uint8_t block, bool key_on) {
        Operator &op = op_[opidx];
        if (key_on && !op.key_on) {
            op.env = Env::kAttack;
            op.phase = 0;
            op.out = 0; op.prev_out = 0;
        } else if (!key_on && op.key_on) {
            op.env = Env::kRelease;
        }
        op.key_on = key_on;

        bool nts = (regs_[0x08] & 0x40) != 0;
        int ksn = (int(block) << 1) | int(nts ? ((fnum >> 9) & 1) : ((fnum >> 8) & 1));
        switch (op.env) {
            case Env::kAttack: {
                if (op.ar != 0) {
                    int rof = effective_rate(op.ar, ksn, op.ksr != 0);
                    double tau = attack_tau_seconds(rof);
                    if (tau <= 0.0) op.env_level = 0;
                    else op.env_level = int32_t(std::lround(double(op.env_level) * std::exp(-1.0 / (tau * kSampleHz))));
                    if (op.env_level <= kEnvScale / 2) { op.env_level = 0; op.env = Env::kDecay; }
                }
                break;
            }
            case Env::kDecay: {
                int rof = effective_rate(op.dr, ksn, op.ksr != 0);
                op.env_level += int32_t(std::lround(decay_units_per_sample(rof)));
                int32_t sl_target = (op.sl == 15) ? kEnvMax : int32_t(op.sl) * 16 * kEnvScale;
                if (op.env_level >= sl_target) { op.env_level = sl_target; op.env = Env::kSustain; }
                break;
            }
            case Env::kSustain: {
                if (op.egt == 0) {  // percussive: fall straight through to release
                    int rof = effective_rate(op.rr, ksn, op.ksr != 0);
                    op.env_level += int32_t(std::lround(decay_units_per_sample(rof)));
                    if (op.env_level >= kEnvMax) { op.env_level = kEnvMax; op.env = Env::kOff; }
                }
                break;
            }
            case Env::kRelease: {
                int rof = effective_rate(op.rr, ksn, op.ksr != 0);
                op.env_level += int32_t(std::lround(decay_units_per_sample(rof)));
                if (op.env_level >= kEnvMax) { op.env_level = kEnvMax; op.env = Env::kOff; }
                break;
            }
            case Env::kOff: break;
        }
        if (op.env_level < 0) op.env_level = 0;
        if (op.env_level > kEnvMax) op.env_level = kEnvMax;

        // One lap of the 20-bit accumulator is one cycle and wave_sample()
        // indexes it as phase>>10, so a frame advances fnum * 2^block for
        // MULT=1 -- i.e. F = fnum * kSampleHz / 2^(20-block), the figure the
        // classic AdLib note table is built on (fnum 159h at block 4 is
        // middle C, 261.6 Hz). kMultX2 holds twice the multiplier, so the
        // halving here is what makes MULT=0 the documented x0.5.
        double base = double(uint64_t(fnum) << block) * double(kMultX2[op.mult]) / 2.0;
        double increment = base * (op.vib ? vib_ratio_this_frame : 1.0);
        op.phase = (op.phase + uint32_t(std::llround(increment))) & 0xFFFFF;
    };

    // Computes one operator's fully-formed (signed) sample for this frame,
    // given a phase-modulation offset from a preceding operator (0 if none).
    auto compute_sample = [this, am_units_this_frame](int opidx, uint16_t fnum, uint8_t block, int32_t mod_offset) -> int32_t {
        Operator &op = op_[opidx];
        double atten = double(op.env_level) / double(kEnvScale) + double(op.tl) * 4.0 +
                       double(ksl_env_units(op.ksl, fnum, block)) +
                       (op.am ? am_units_this_frame : 0.0);
        uint32_t phase20 = uint32_t((uint64_t(op.phase) + uint64_t(uint32_t(mod_offset))) & 0xFFFFF);
        return wave_sample(op.waveform, phase20, atten);
    };

    auto feedback_offset = [this](int opidx, uint8_t fb) -> int32_t {
        if (fb == 0) return 0;
        double avg = (double(op_[opidx].out) + double(op_[opidx].prev_out)) / 2.0;
        return phase_offset(int32_t(avg), kFeedbackRadians[fb]);
    };

    bool rhythm_active = (regs_[0xBD] & 0x20) != 0;
    double left = 0.0, right = 0.0;

    auto mix_channel = [this, &left, &right](int c, int32_t out_val) {
        bool out_l = new_ ? ch_[c].out_left : true;
        bool out_r = new_ ? ch_[c].out_right : true;
        if (out_l) left += out_val;
        if (out_r) right += out_val;
    };

    for (int c = 0; c < 18; c++) {
        if (ch_[c].four_op_secondary) continue;  // folded into its primary below
        if (rhythm_active && (c == 6 || c == 7 || c == 8)) continue;  // handled after this loop

        uint16_t fnum = ch_[c].fnum;
        uint8_t block = ch_[c].block;
        int bank = c / 9, cib = c % 9;
        int mod = op_index(bank, cib, false);
        int car = op_index(bank, cib, true);

        if (ch_[c].four_op_primary) {
            int s = c + 3;
            uint16_t sfnum = ch_[s].fnum;
            uint8_t sblock = ch_[s].block;
            int op3 = op_index(bank, cib + 3, false);
            int op4 = op_index(bank, cib + 3, true);
            advance_env_phase(mod, fnum, block, ch_[c].key_on);
            advance_env_phase(car, fnum, block, ch_[c].key_on);
            advance_env_phase(op3, sfnum, sblock, ch_[c].key_on);
            advance_env_phase(op4, sfnum, sblock, ch_[c].key_on);

            int32_t out1 = compute_sample(mod, fnum, block, feedback_offset(mod, ch_[c].feedback));
            op_[mod].prev_out = op_[mod].out; op_[mod].out = int16_t(out1);
            bool cp = ch_[c].connection != 0, cs = ch_[s].connection != 0;
            int32_t out2, out3, out4, channel_out;
            if (!cp && !cs) {
                out2 = compute_sample(car, fnum, block, phase_offset(out1, kModulationIndexRadians));
                op_[car].prev_out = op_[car].out; op_[car].out = int16_t(out2);
                out3 = compute_sample(op3, sfnum, sblock, phase_offset(out2, kModulationIndexRadians));
                op_[op3].prev_out = op_[op3].out; op_[op3].out = int16_t(out3);
                out4 = compute_sample(op4, sfnum, sblock, phase_offset(out3, kModulationIndexRadians));
                op_[op4].prev_out = op_[op4].out; op_[op4].out = int16_t(out4);
                channel_out = out4;
            } else if (!cp && cs) {
                out2 = compute_sample(car, fnum, block, phase_offset(out1, kModulationIndexRadians));
                op_[car].prev_out = op_[car].out; op_[car].out = int16_t(out2);
                out3 = compute_sample(op3, sfnum, sblock, 0);
                op_[op3].prev_out = op_[op3].out; op_[op3].out = int16_t(out3);
                out4 = compute_sample(op4, sfnum, sblock, phase_offset(out3, kModulationIndexRadians));
                op_[op4].prev_out = op_[op4].out; op_[op4].out = int16_t(out4);
                channel_out = out2 + out4;
            } else if (cp && !cs) {
                out2 = compute_sample(car, fnum, block, 0);
                op_[car].prev_out = op_[car].out; op_[car].out = int16_t(out2);
                out3 = compute_sample(op3, sfnum, sblock, phase_offset(out2, kModulationIndexRadians));
                op_[op3].prev_out = op_[op3].out; op_[op3].out = int16_t(out3);
                out4 = compute_sample(op4, sfnum, sblock, phase_offset(out3, kModulationIndexRadians));
                op_[op4].prev_out = op_[op4].out; op_[op4].out = int16_t(out4);
                channel_out = out1 + out4;
            } else {
                out2 = compute_sample(car, fnum, block, 0);
                op_[car].prev_out = op_[car].out; op_[car].out = int16_t(out2);
                out3 = compute_sample(op3, sfnum, sblock, phase_offset(out2, kModulationIndexRadians));
                op_[op3].prev_out = op_[op3].out; op_[op3].out = int16_t(out3);
                out4 = compute_sample(op4, sfnum, sblock, 0);
                op_[op4].prev_out = op_[op4].out; op_[op4].out = int16_t(out4);
                channel_out = out1 + out3 + out4;
            }
            mix_channel(c, channel_out);
            continue;
        }

        advance_env_phase(mod, fnum, block, ch_[c].key_on);
        advance_env_phase(car, fnum, block, ch_[c].key_on);
        int32_t mod_out = compute_sample(mod, fnum, block, feedback_offset(mod, ch_[c].feedback));
        op_[mod].prev_out = op_[mod].out; op_[mod].out = int16_t(mod_out);
        bool additive = ch_[c].connection != 0;
        int32_t car_offset = additive ? 0 : phase_offset(mod_out, kModulationIndexRadians);
        int32_t car_out = compute_sample(car, fnum, block, car_offset);
        op_[car].prev_out = op_[car].out; op_[car].out = int16_t(car_out);
        mix_channel(c, additive ? (mod_out + car_out) : car_out);
    }

    if (rhythm_active) {
        // Bass drum: channel 6 runs as an ordinary 2-op FM/additive channel,
        // keyed by BDh bit 4 instead of its own B6h KON.
        bool bd_on = (regs_[0xBD] >> 4) & 1;
        bool sd_on = (regs_[0xBD] >> 3) & 1;
        bool tom_on = (regs_[0xBD] >> 2) & 1;
        bool tc_on = (regs_[0xBD] >> 1) & 1;
        bool hh_on = (regs_[0xBD] >> 0) & 1;

        {
            int mod = op_index(0, 6, false), car = op_index(0, 6, true);
            advance_env_phase(mod, ch_[6].fnum, ch_[6].block, bd_on);
            advance_env_phase(car, ch_[6].fnum, ch_[6].block, bd_on);
            int32_t mod_out = compute_sample(mod, ch_[6].fnum, ch_[6].block, feedback_offset(mod, ch_[6].feedback));
            op_[mod].prev_out = op_[mod].out; op_[mod].out = int16_t(mod_out);
            bool additive = ch_[6].connection != 0;
            int32_t off = additive ? 0 : phase_offset(mod_out, kModulationIndexRadians);
            int32_t car_out = compute_sample(car, ch_[6].fnum, ch_[6].block, off);
            op_[car].prev_out = op_[car].out; op_[car].out = int16_t(car_out);
            mix_channel(6, additive ? (mod_out + car_out) : car_out);
        }

        // Noise LFSR, maximal-length 23-bit (polynomial x^23+x^18+1),
        // advanced once per output frame -- a generic, unverified stand-in
        // for the rhythm section's actual tap positions, see the change
        // notes.
        uint32_t fb = ((noise_ >> 22) ^ (noise_ >> 17)) & 1;
        noise_ = ((noise_ << 1) | fb) & 0x7FFFFF;
        bool noise_bit = (noise_ & 1) != 0;

        // Tom-tom: channel 8's modulator run as a plain, unmodulated
        // oscillator -- its own AR/DR/SL/RR/TL/waveform shape the sound.
        {
            int tom = op_index(0, 8, false);
            advance_env_phase(tom, ch_[8].fnum, ch_[8].block, tom_on);
            int32_t out = compute_sample(tom, ch_[8].fnum, ch_[8].block, 0);
            op_[tom].prev_out = op_[tom].out; op_[tom].out = int16_t(out);
            mix_channel(8, out);
        }

        // Snare, hi-hat and top cymbal: noise-gated square shaped by each
        // slot's own envelope/TL -- a reasoned approximation of the real
        // phase-bit/noise XOR the silicon uses, not a verified reproduction
        // (see the change notes).
        auto noise_percussion = [&](int opidx, uint16_t fnum, uint8_t block, bool key_on, int phase_bit, int c) {
            advance_env_phase(opidx, fnum, block, key_on);
            bool gate = noise_bit ^ (((op_[opidx].phase >> phase_bit) & 1) != 0);
            double atten = double(op_[opidx].env_level) / double(kEnvScale) + double(op_[opidx].tl) * 4.0 +
                           double(ksl_env_units(op_[opidx].ksl, fnum, block)) +
                           (op_[opidx].am ? am_units_this_frame : 0.0);
            int32_t mag = wave_sample(6, 0, atten);
            int32_t out = gate ? mag : -mag;
            op_[opidx].prev_out = op_[opidx].out; op_[opidx].out = int16_t(out);
            mix_channel(c, out);
        };
        noise_percussion(op_index(0, 7, false), ch_[7].fnum, ch_[7].block, hh_on, 18, 7);
        noise_percussion(op_index(0, 7, true), ch_[7].fnum, ch_[7].block, sd_on, 16, 7);
        noise_percussion(op_index(0, 8, true), ch_[8].fnum, ch_[8].block, tc_on, 17, 8);
    }

    // Scales the summed channel mix into the 16-bit offset-binary word the
    // YMF262 hands its DAC (datasheet p.5: 4 buses, 49.7 kHz). That format is
    // documented; the gain reaching it is not -- neither Yamaha document
    // describes how up to 18 channels are summed internally or what headroom
    // that leaves, and Creative's own SB16 programming guide (p.138) refers
    // OPL3 internals back to the vendor. So this is an empirical scale with no
    // hardware citation, chosen for headroom at the polyphony that actually
    // binds: one zero-attenuation voice reaches ~18% of full scale, and nine
    // of them (the case that clips first -- not the many-quiet-voices case)
    // peak at 86%. See PC486_REVIEW.md §11.1 and §29.
    constexpr double kMasterGain = 6.0;
    auto clamp16 = [](double x) -> int16_t {
        if (x > 32767.0) return 32767;
        if (x < -32768.0) return -32768;
        return int16_t(std::lround(x));
    };
    if (samples_.size() >= kMaxSamples) samples_.pop_front();
    samples_.push_back({cycle, clamp16(left * kMasterGain), clamp16(right * kMasterGain)});
}

}  // namespace pc486
