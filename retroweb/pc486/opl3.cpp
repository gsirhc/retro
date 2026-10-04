#include "opl3.h"

#include <algorithm>
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
// carrier's phase in a normal (non-feedback) FM connection, pinned via the
// decap's feedback path rather than guessed: the die forms feedback as
// (out+prev_out) >> (9-FB), and a full-scale operator output is 4084
// (exp_rom[0]<<1), so the phase adder runs at 1024 units/cycle. Modulator-to-
// carrier coupling adds the modulator's raw output into that same adder
// unshifted, so a full-scale modulator deviates the carrier by 4084/1024 =
// 3.99 cycles = 8pi radians -- exactly double the FB=7 figure kFeedbackRadians
// already carries (4pi). See PC486_REVIEW.md §11.1.
constexpr double kModulationIndexRadians = 8.0 * kPi;

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
// KSN 0-15 directly; KSR=0: KSN>>2) -- YMF715x doc's "Rate Key Scale" /
// "RATE=(RATE Value)x4+Rof" note. A rate register of 0 always yields "the
// operator never moves", signalled here as -1 rather than a numeric RATE,
// since real silicon gates the whole envelope-clock lookup off reg_rate==0
// directly (envelope_shift() below), not off any particular RATE value.
// Deliberately NOT clamped to 63: real hardware clamps only the top nibble
// (rate_hi, inside envelope_shift()) and keeps rate_lo from this same
// unclamped sum, so a caller that capped the sum first would hand
// envelope_shift() the wrong rate_lo whenever RATE exceeds 63 (reachable:
// reg_rate=15, KSR=1, ksn=15 gives 75) -- see "OPLx decapsulated" and
// PC486_REVIEW.md's envelope-generator section.
int effective_rate(uint8_t reg_rate, int ksn, bool ksr) {
    if (reg_rate == 0) return -1;
    int rks = ksr ? ksn : (ksn >> 2);
    return int(reg_rate) * 4 + rks;
}

// Envelope attenuation (env_level, Operator::env_level) is a plain 9-bit
// (0-511) counter, 0.1875 dB/unit, 511 = silent -- matching the real
// chip's own precision exactly. No fractional/fixed-point scale: real
// silicon has none either (the slow rates look continuous only because the
// shared envelope clock gates most samples to a zero step, not because the
// attenuation itself has sub-unit precision).
constexpr int32_t kEnvMax = 511;

// eg_incstep[rate_lo][eg_timer_lo] -- the small additional shift the four
// fastest rate groups (rate_hi 12-15) add on top of rate_hi&3, per "OPLx
// decapsulated"'s documented envelope state machine (cross-checked against
// Nuked-OPL3's eg_incstep table as a fact only, never copied, per this
// file's standing citation rule).
constexpr int kEgIncStep[4][4] = {
    {0, 0, 0, 0},
    {1, 0, 0, 0},
    {1, 0, 1, 0},
    {1, 1, 1, 0},
};

// How many bits (0-3) an operator's attenuation counter should shift by
// this output frame, for an operator whose effective RATE is `rate`
// (effective_rate()'s return, or -1 for "rate register 0, never moves").
// The chip has one shared envelope clock, not a per-operator timer: all 36
// operators read the SAME eg_add/eg_timer_lo/eg_state this frame
// (Opl3::eg_add_ etc., advanced once per frame by advance_envelope_clock()
// below) and each derives its own shift from its own RATE against that
// shared clock -- "OPLx decapsulated"'s documented mechanism. A shift of 0
// means this operator's attenuation does not move this frame; most
// frames, for most rates, it doesn't -- that's what makes the slow rates
// slow.
int envelope_shift(int rate, int eg_add, int eg_timer_lo, bool eg_state) {
    if (rate < 0) return 0;
    int rate_hi = rate >> 2;
    int rate_lo = rate & 3;
    if (rate_hi > 15) rate_hi = 15;
    int eg_shift = rate_hi + eg_add;
    if (rate_hi < 12) {
        if (!eg_state) return 0;
        switch (eg_shift) {
            case 12: return 1;
            case 13: return (rate_lo >> 1) & 1;
            case 14: return rate_lo & 1;
            default: return 0;
        }
    }
    int shift = (rate_hi & 3) + kEgIncStep[rate_lo][eg_timer_lo];
    if (shift & 4) shift = 3;
    if (shift == 0) shift = eg_state ? 1 : 0;
    return shift;
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
            // The negative half's phase is mirrored (real hardware ramps
            // that half up to full amplitude by the end of the cycle, not
            // down from it) and the ramp covers the full ~96 dB range per
            // half-cycle, not ~48 dB -- hence <<3, not <<2.
            if (quadrant >= 2) li = 511 - li;
            logsin_units = int32_t(li) * 8;
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
    for (auto &op : op_) op = Operator();
    for (auto &ch : ch_) ch = Channel();
    am_phase_ = 0;
    vib_pos_ = 0;
    vib_frame_ = 0;
    noise_ = 1;
    eg_timer_ = 0;
    eg_timerrem_ = false;
    eg_state_ = false;
    eg_add_ = 0;
    eg_timer_lo_ = 0;
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
        // "Idle" is derived, not stored: a fully released operator sits in
        // kRelease forever with its attenuation pinned at the eg_off
        // threshold (generate_frame's shift gate keeps it from moving) --
        // see the Env enum's comment in opl3.h.
        for (const auto &op : op_) {
            bool pinned = op.env == Env::kRelease && (op.env_level & 0x1F8) == 0x1F8;
            if (!pinned) { any = true; break; }
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
    // am_phase_ is a 32-bit fixed-point accumulator, one full lap = 2^32, so
    // the increment for a target frequency is freq/kSampleHz of that span.
    // Vibrato is handled separately below -- it is quantised, not a phase LFO.
    constexpr double kFull = 4294967296.0;
    am_phase_ += uint32_t(std::llround((3.7 / kSampleHz) * kFull));
    double am01 = double(am_phase_) / kFull;
    double am_tri = am01 < 0.5 ? am01 * 2.0 : 2.0 - am01 * 2.0;              // 0..1
    bool dam = (regs_[0xBD] & 0x80) != 0;
    bool dvb = (regs_[0xBD] & 0x40) != 0;
    double am_units_this_frame = am_tri * ((dam ? 4.8 : 1.0) / 0.1875);

    // Vibrato position counter: real hardware steps through an 8-entry
    // pattern once every 1024 output frames (49715.9/(1024*8) = 6.07 Hz),
    // not a continuous LFO -- Niemitalo & Gambrell's decap notes (cross-
    // checked against Nuked-OPL3's vibtab as a fact only, per this repo's
    // standing rule).
    if (++vib_frame_ >= 1024) { vib_frame_ = 0; vib_pos_ = (vib_pos_ + 1) & 7; }

    // Advances one operator's envelope and phase generator for this frame,
    // given the key state it should currently see (channel KON, or a
    // rhythm-instrument bit for channels 6-8 in rhythm mode).
    auto advance_env_phase = [this, dvb](int opidx, uint16_t fnum, uint8_t block, bool key_on) {
        Operator &op = op_[opidx];

        // Real hardware re-triggers attack whenever key-on is live while the
        // envelope is (still, or again) in release -- not on an edge against
        // a separately tracked flag. "OPLx decapsulated"'s `reset`. A fully
        // released operator sits in kRelease indefinitely (see the Env enum
        // comment in opl3.h), so this also covers a fresh key-on.
        bool reset = key_on && op.env == Env::kRelease;
        if (key_on && !op.key_on) {
            // Phase/feedback history reset exactly on the triggering sample
            // -- "OPLx decapsulated"'s pg_reset. The envelope shift below is
            // keyed off op.env, which (by design, see reset above) does not
            // flip to kAttack until the end of this same call -- so on the
            // very sample a note starts, the shift computation below still
            // dispatches through the release/off case, exactly as real
            // silicon does (it changes eg_gen only after this calculation).
            op.phase = 0;
            op.out = 0; op.prev_out = 0;
        }
        op.key_on = key_on;

        // NTS=0 selects F-number bit 9 for key scaling, NTS=1 selects bit 8 --
        // Yamaha YMF715x Register Description Document's note-select table.
        bool nts = (regs_[0x08] & 0x40) != 0;
        int ksn = (int(block) << 1) | int(nts ? ((fnum >> 8) & 1) : ((fnum >> 9) & 1));

        uint8_t reg_rate = 0;
        switch (op.env) {
            case Env::kAttack: reg_rate = op.ar; break;
            case Env::kDecay: reg_rate = op.dr; break;
            case Env::kSustain: if (op.egt == 0) reg_rate = op.rr; break;  // percussive: falls to release
            case Env::kRelease: reg_rate = op.rr; break;
        }
        if (reset) reg_rate = op.ar;

        int rate = effective_rate(reg_rate, ksn, op.ksr != 0);
        int rate_hi = rate < 0 ? 0 : std::min(rate >> 2, 15);
        int shift = envelope_shift(rate, eg_add_, eg_timer_lo_, eg_state_);

        bool eg_off = (op.env_level & 0x1F8) == 0x1F8;
        int32_t rout = op.env_level;
        if (reset && rate_hi == 15) rout = 0;  // instant attack
        if (op.env != Env::kAttack && !reset && eg_off) rout = kEnvMax;
        int32_t inc = 0;

        switch (op.env) {
            case Env::kAttack:
                if (op.env_level == 0) {
                    op.env = Env::kDecay;
                } else if (key_on && shift > 0 && rate_hi != 15) {
                    inc = (~rout) >> (4 - shift);
                }
                break;
            case Env::kDecay: {
                int sl = (op.sl == 15) ? 31 : int(op.sl);
                if ((op.env_level >> 4) == sl) {
                    op.env = Env::kSustain;
                } else if (!eg_off && !reset && shift > 0) {
                    inc = 1 << (shift - 1);
                }
                break;
            }
            case Env::kSustain:
            case Env::kRelease:
                if (!eg_off && !reset && shift > 0) inc = 1 << (shift - 1);
                break;
        }
        op.env_level = (rout + inc) & 0x1FF;
        if (reset) op.env = Env::kAttack;
        if (!key_on) op.env = Env::kRelease;

        // One lap of the 20-bit accumulator is one cycle and wave_sample()
        // indexes it as phase>>10, so a frame advances fnum * 2^block for
        // MULT=1 -- i.e. F = fnum * kSampleHz / 2^(20-block), the figure the
        // classic AdLib note table is built on (fnum 159h at block 4 is
        // middle C, 261.6 Hz). kMultX2 holds twice the multiplier, so the
        // halving here is what makes MULT=0 the documented x0.5.
        int32_t vfnum = int32_t(fnum);
        if (op.vib) {
            // Real hardware's vibrato is a quantised F-number delta keyed off
            // the position step, not a continuous cents ratio -- Niemitalo &
            // Gambrell's decap notes. DVB (register BDh bit 6) doubles the
            // depth by skipping the final halving.
            int range = (fnum >> 7) & 7;
            if ((vib_pos_ & 3) == 0) range = 0;
            else if (vib_pos_ & 1) range >>= 1;
            range >>= dvb ? 0 : 1;
            if (vib_pos_ & 4) range = -range;
            vfnum += range;
            if (vfnum < 0) vfnum = 0;
        }
        double base = double(uint64_t(uint32_t(vfnum)) << block) * double(kMultX2[op.mult]) / 2.0;
        op.phase = (op.phase + uint32_t(std::llround(base))) & 0xFFFFF;
    };

    // Computes one operator's fully-formed (signed) sample for this frame,
    // given a phase-modulation offset from a preceding operator (0 if none).
    auto compute_sample = [this, am_units_this_frame](int opidx, uint16_t fnum, uint8_t block, int32_t mod_offset) -> int32_t {
        Operator &op = op_[opidx];
        double atten = double(op.env_level) + double(op.tl) * 4.0 +
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
            // The secondary's own A0h/B0h are latched into ch_[s] but not
            // live while the pair is in 4-op mode: the primary's fnum/block
            // drive all four operators (Nuked-OPL3's OPL3_ChannelSync4Op,
            // called on every A0h/B0h write and on a 104h change -- decap-
            // corroborated behavior, not stated in the Yamaha datasheet).
            // ch_[s].fnum/block stay as written and go live again once 104h
            // clears that pairing, which is itself real behavior.
            uint16_t sfnum = fnum;
            uint8_t sblock = block;
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
            // Each rhythm voice is wired into two of the channel's four
            // output buses on real silicon, so it sums at twice a melodic
            // channel's amplitude -- decap-corroborated (Nuked-OPL3 wires
            // out[0]/out[1] and out[2]/out[3] to the same slot in rhythm
            // mode), not stated in a Yamaha document.
            mix_channel(6, 2 * (additive ? (mod_out + car_out) : car_out));
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
            mix_channel(8, 2 * out);  // rhythm voice on two output buses -- see the bass drum note above
        }

        // Snare, hi-hat and top cymbal: noise-gated square shaped by each
        // slot's own envelope/TL -- a reasoned approximation of the real
        // phase-bit/noise XOR the silicon uses, not a verified reproduction
        // (see the change notes).
        auto noise_percussion = [&](int opidx, uint16_t fnum, uint8_t block, bool key_on, int phase_bit, int c) {
            advance_env_phase(opidx, fnum, block, key_on);
            bool gate = noise_bit ^ (((op_[opidx].phase >> phase_bit) & 1) != 0);
            double atten = double(op_[opidx].env_level) + double(op_[opidx].tl) * 4.0 +
                           double(ksl_env_units(op_[opidx].ksl, fnum, block)) +
                           (op_[opidx].am ? am_units_this_frame : 0.0);
            int32_t mag = wave_sample(6, 0, atten);
            int32_t out = gate ? mag : -mag;
            op_[opidx].prev_out = op_[opidx].out; op_[opidx].out = int16_t(out);
            mix_channel(c, 2 * out);  // rhythm voice on two output buses -- see the bass drum note above
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

    // Every operator above read this frame's eg_add_/eg_timer_lo_/eg_state_
    // (set by the PREVIOUS call); advance them now so the NEXT frame sees
    // the clock one tick on -- the same order real silicon updates them in
    // (OPL3_Generate4Ch processes all 36 slots, then steps the clock).
    advance_envelope_clock();
}

// The chip-global envelope clock every operator's envelope_shift() call
// reads (opl3.h's eg_timer_ etc.) -- "OPLx decapsulated"'s documented
// mechanism, cross-checked against Nuked-OPL3's chip->eg_timer update as a
// fact only. eg_state_ alternates every output frame; eg_add_ and
// eg_timer_lo_ only change on the frames eg_state_ is true, derived from
// counting eg_timer_'s trailing zero bits (a "ruler sequence") -- which is
// also why the clock only advances itself on those same frames (gated by
// eg_timerrem_ for the one-sample-late carry once eg_timer_ wraps).
void Opl3::advance_envelope_clock() {
    if (eg_state_) {
        int shift = 0;
        while (shift < 13 && ((eg_timer_ >> shift) & 1) == 0) shift++;
        eg_add_ = shift > 12 ? 0 : shift + 1;
        eg_timer_lo_ = uint8_t(eg_timer_ & 3);
    }
    if (eg_timerrem_ || eg_state_) {
        if (eg_timer_ == 0xFFFFFFFFFULL) {  // 36-bit counter, per the decap notes
            eg_timer_ = 0;
            eg_timerrem_ = true;
        } else {
            eg_timer_++;
            eg_timerrem_ = false;
        }
    }
    eg_state_ = !eg_state_;
}

}  // namespace pc486
