#include "opl3.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pc486 {
namespace {

constexpr double kPi = 3.14159265358979323846;

// MULT field as 2x the multiplier (0.5,1,2,...,10,10,12,12,15,15), Yamaha
// MULT3-0 table; halved after the phase math so 0.5 needs no special case
constexpr uint8_t kMultX2[16] = {
    1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30,
};

// KSL attenuation at KSL=1 (3 dB/oct, BLOCK=7) by top 4 F-number bits, in
// 0.375 dB units. From the YMF715x (OPL3-SA3) register description OCT=7 column;
// matches Niemitalo & Gambrell's decap figures.
constexpr uint8_t kKslTable[16] = {
    0, 24, 32, 37, 40, 43, 45, 47, 48, 50, 51, 52, 53, 54, 55, 56,
};

// Feedback modulation index in radians, FB=0-7 (YMF715x "FB 2-0" table)
constexpr double kFeedbackRadians[8] = {
    0.0, kPi / 16.0, kPi / 8.0, kPi / 4.0, kPi / 2.0, kPi, 2.0 * kPi, 4.0 * kPi,
};

// Phase units per radian: one waveform cycle is 2^20 units (10.10 fixed point)
constexpr double kPhaseUnitsPerRadian = 1048576.0 / (2.0 * kPi);
// Zero-attenuation wave sample; the exp table's implicit leading 1 puts unity at 1024
constexpr double kFullScale = 1024.0;
// Modulation index of a full-scale modulator. The die forms feedback as
// (out+prev_out) >> (9-FB) and full scale is 4084, so the phase adder runs at
// 1024 units/cycle; the modulator adds unshifted: 4084/1024 = 3.99 cycles = 8pi,
// double the FB=7 figure (PC486_REVIEW.md §11.1).
constexpr double kModulationIndexRadians = 8.0 * kPi;

// Log-sin (first quarter) and exp ROM tables from Niemitalo & Gambrell "OPLx
// decapsulated": logsin[x] = round(-log2(sin((x+0.5)*pi/512))*256),
// exp[x] = round((2^(x/256)-1)*1024). Table values only, no code.
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

// Register offset to operator slot 0-17 within a bank ("0-5, 8-13, 16-21"),
// -1 for the gaps
int operator_slot(int r) {
    if (r < 0 || r > 21) return -1;
    int group = r / 8;
    int sub = r % 8;
    if (group > 2 || sub > 5) return -1;
    return group * 6 + sub;
}

// op_[] index for channel-in-bank `cib`'s modulator or carrier (Register
// Setting Manner 1-6): 3 channels share 6 slots, modulators first
int op_index(int bank, int cib, bool carrier) {
    int group = cib / 3, cl = cib % 3;
    return bank * 18 + group * 6 + (carrier ? 3 + cl : cl);
}

// RATE = reg*4 + Rof (KSR=1: KSN 0-15; KSR=0: KSN>>2), YMF715x "Rate Key
// Scale". Rate register 0 returns -1 (envelope never moves), as the silicon gates
// on reg_rate==0. Not clamped to 63: hardware clamps only rate_hi and keeps
// rate_lo from the unclamped sum (reg 15, KSR=1, ksn=15 gives 75).
int effective_rate(uint8_t reg_rate, int ksn, bool ksr) {
    if (reg_rate == 0) return -1;
    int rks = ksr ? ksn : (ksn >> 2);
    return int(reg_rate) * 4 + rks;
}

// Envelope attenuation is a 9-bit counter (0-511), 0.1875 dB/unit, 511 = silent,
// no fractional precision. Slow rates look continuous because the shared clock gates steps.
constexpr int32_t kEnvMax = 511;

// eg_incstep[rate_lo][eg_timer_lo]: extra shift for rate_hi 12-15 on top of
// rate_hi&3 ("OPLx decapsulated"; Nuked-OPL3 table cross-check only)
constexpr int kEgIncStep[4][4] = {
    {0, 0, 0, 0},
    {1, 0, 0, 0},
    {1, 0, 1, 0},
    {1, 1, 1, 0},
};

// Bits (0-3) an operator's attenuation shifts this frame for effective RATE
// `rate` (-1 = never moves). All 36 operators share one envelope clock
// (eg_add_/eg_timer_lo_/eg_state_) and derive their shift from their own RATE.
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

// Total level (0.75 dB/step = 4 envelope units) plus KSL attenuation, in
// 0.1875 dB units. atten = max(0, kKslTable[fnum top4] - 8*(7-oct)), scaled for
// KSL 0/1/2/3 = 0 / 3 / 1.5 / 6 dB per octave.
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

// Phase-modulation offset for a modulator output `out_val` (about +-1024,
// unity 1024), scaled by a modulation index in radians
int32_t phase_offset(int32_t out_val, double radians) {
    return int32_t(std::lround((double(out_val) / kFullScale) * radians * kPhaseUnitsPerRadian));
}

// One of the 8 waveforms at a 20-bit phase: log-sin lookup, add attenuation
// (`atten_env_units`, 0.1875 dB), exponentiate. 0-3 are OPL2 (sine, half, abs,
// quarter/pulse); 4-7 OPL3-only (double-frequency sine, its abs, square, sawtooth).
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
            // The negative half ramps up to full amplitude by cycle end, and the ramp
            // covers ~96 dB per half cycle, hence <<3 not <<2
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
    // The die stores the exp ROM reversed, so decode indexes 255-frac (Niemitalo
    // & Gambrell). The form carries a factor of two, which the extra shift removes.
    int32_t frac = t & 0xFF;
    int32_t mantissa = int32_t(kWave.exp_tab[255 - frac]) + 1024;
    int32_t mag = mantissa >> (shift + 1);
    return sign * mag;
}

}  // namespace

void Opl3::set_cpu_hz(double hz) {
    if (!(hz > 0.0) || hz == cpu_hz_) return;
    // Scale frame_credit_ so the fractional frame stays the same wall-clock remainder across a Turbo toggle
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
    // Every register clears to 0. A cleared C0h doesn't mute: pan bits are
    // ignored while NEW is clear (see generate_frame).
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
    // Reg 04h mask bits gate only the IRQ line; a masked timer still sets its flag
    // but can't raise bit 7. OPL2 returns 6 in bits 2-1; bits 4-0 stay 0 here.
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
    // A bank-1 write while NEW is clear reaches no register and must not land in
    // regs_. 105h is the exception, or OPL3 mode could never be enabled.
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

// Overflow is counted as boundary crossings on an absolute tick grid, tick t =
// floor(cpu_cycle / period), with no per-timer counter. Preset P reloads every
// N=256-P ticks, so t is an overflow when t % N == 0. Period is exact; the
// first overflow after an off-grid start may slip one tick, which detection tolerates.
void Opl3::step_timers(uint32_t frames) {
    if (frames == 0) return;
    const double cycles_per_frame = cpu_hz_ / kSampleHz;
    // Window is over-extended by a couple of frames so consecutive calls overlap
    // and never skip a boundary (re-checking a tick is idempotent)
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
    // A just-keyed-on channel has all operators in kOff, which only generate_frame
    // leaves, and that doesn't run while active_ is clear. key_on must count here or
    // key-on deadlocks against tick()'s early-out.
    if (!any) {
        for (const auto &ch : ch_) {
            if (ch.key_on) { any = true; break; }
        }
    }
    if (!any && (regs_[0xBD] & 0x20) != 0 && (regs_[0xBD] & 0x1F) != 0) any = true;
    if (!any) {
        // Idle is derived: a released operator sits in kRelease with attenuation pinned at eg_off
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
    // Consume the credit for every due frame before bounding the work. Capping
    // first leaves a remainder that makes the chip's clock fall behind on every big
    // catch-up and the music slows permanently. Dropping audio is recoverable.
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
    // am_phase_ is a 32-bit accumulator, one lap = 2^32; increment = freq/kSampleHz of that. Vibrato is separate.
    constexpr double kFull = 4294967296.0;
    am_phase_ += uint32_t(std::llround((3.7 / kSampleHz) * kFull));
    double am01 = double(am_phase_) / kFull;
    double am_tri = am01 < 0.5 ? am01 * 2.0 : 2.0 - am01 * 2.0;              // 0..1
    bool dam = (regs_[0xBD] & 0x80) != 0;
    bool dvb = (regs_[0xBD] & 0x40) != 0;
    double am_units_this_frame = am_tri * ((dam ? 4.8 : 1.0) / 0.1875);

    // Vibrato steps through an 8-entry pattern once per 1024 frames
    // (49715.9/(1024*8) = 6.07 Hz), not a continuous LFO (Niemitalo & Gambrell;
    // Nuked-OPL3 vibtab cross-check only)
    if (++vib_frame_ >= 1024) { vib_frame_ = 0; vib_pos_ = (vib_pos_ + 1) & 7; }

    // Advances one operator's envelope and phase for this frame, given its key state
    // (channel KON, or a rhythm bit for channels 6-8)
    auto advance_env_phase = [this, dvb](int opidx, uint16_t fnum, uint8_t block, bool key_on) {
        Operator &op = op_[opidx];

        // Attack re-triggers whenever key-on is live while the envelope is in release,
        // not on an edge ("OPLx decapsulated" `reset`)
        bool reset = key_on && op.env == Env::kRelease;
        if (key_on && !op.key_on) {
            // Phase/feedback history resets on the triggering sample (pg_reset). The shift
            // below still takes the release/off path since op.env flips to kAttack at the
            // end of the call, as on silicon.
            op.phase = 0;
            op.out = 0; op.prev_out = 0;
        }
        op.key_on = key_on;

        // NTS=0 selects F-number bit 9 for key scaling, NTS=1 bit 8 (YMF715x note-select table)
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

        // One lap of the 20-bit accumulator is one cycle, indexed as phase>>10, so a
        // frame advances fnum * 2^block at MULT=1: F = fnum * kSampleHz / 2^(20-block)
        // (fnum 159h at block 4 is middle C). kMultX2 is doubled, so halving gives MULT=0 x0.5.
        int32_t vfnum = int32_t(fnum);
        if (op.vib) {
            // Vibrato is a quantised F-number delta per position step, not a cents
            // ratio (Niemitalo & Gambrell). DVB (BDh bit 6) doubles depth by skipping the final halving.
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

    // One operator's signed sample for this frame, given a phase-modulation offset from the preceding operator
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
            // The secondary's A0h/B0h are latched but not live in 4-op mode: the primary's
            // fnum/block drive all four operators (Nuked-OPL3 OPL3_ChannelSync4Op;
            // decap-corroborated, not in the datasheet). They go live when 104h unpairs.
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
        // Bass drum: channel 6 as an ordinary 2-op channel keyed by BDh bit 4
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
            // Each rhythm voice feeds two of the channel's four output buses, so it sums
            // at twice a melodic channel's amplitude (decap-corroborated via Nuked-OPL3,
            // not in a Yamaha document)
            mix_channel(6, 2 * (additive ? (mod_out + car_out) : car_out));
        }

        // Noise LFSR, 23-bit maximal (x^23+x^18+1), once per frame. An unverified
        // stand-in for the real tap positions.
        uint32_t fb = ((noise_ >> 22) ^ (noise_ >> 17)) & 1;
        noise_ = ((noise_ << 1) | fb) & 0x7FFFFF;
        bool noise_bit = (noise_ & 1) != 0;

        // Tom-tom: channel 8's modulator as a plain oscillator
        {
            int tom = op_index(0, 8, false);
            advance_env_phase(tom, ch_[8].fnum, ch_[8].block, tom_on);
            int32_t out = compute_sample(tom, ch_[8].fnum, ch_[8].block, 0);
            op_[tom].prev_out = op_[tom].out; op_[tom].out = int16_t(out);
            mix_channel(8, 2 * out);  // rhythm voice on two output buses
        }

        // Snare, hi-hat, top cymbal: noise-gated square shaped by each slot's envelope
        // and TL. An approximation of the real phase-bit/noise XOR.
        auto noise_percussion = [&](int opidx, uint16_t fnum, uint8_t block, bool key_on, int phase_bit, int c) {
            advance_env_phase(opidx, fnum, block, key_on);
            bool gate = noise_bit ^ (((op_[opidx].phase >> phase_bit) & 1) != 0);
            double atten = double(op_[opidx].env_level) + double(op_[opidx].tl) * 4.0 +
                           double(ksl_env_units(op_[opidx].ksl, fnum, block)) +
                           (op_[opidx].am ? am_units_this_frame : 0.0);
            int32_t mag = wave_sample(6, 0, atten);
            int32_t out = gate ? mag : -mag;
            op_[opidx].prev_out = op_[opidx].out; op_[opidx].out = int16_t(out);
            mix_channel(c, 2 * out);  // rhythm voice on two output buses
        };
        noise_percussion(op_index(0, 7, false), ch_[7].fnum, ch_[7].block, hh_on, 18, 7);
        noise_percussion(op_index(0, 7, true), ch_[7].fnum, ch_[7].block, sd_on, 16, 7);
        noise_percussion(op_index(0, 8, true), ch_[8].fnum, ch_[8].block, tc_on, 17, 8);
    }

    // Scales the summed mix to the 16-bit offset-binary DAC word (datasheet p.5).
    // The format is documented but not the gain: neither Yamaha document describes
    // the internal sum or headroom, and SBPG p.138 defers to the vendor. Empirical
    // scale: one zero-attenuation voice reaches ~18% of full scale, nine peak at 86%
    // (PC486_REVIEW.md §11.1, §29).
    constexpr double kMasterGain = 6.0;
    auto clamp16 = [](double x) -> int16_t {
        if (x > 32767.0) return 32767;
        if (x < -32768.0) return -32768;
        return int16_t(std::lround(x));
    };
    if (samples_.size() >= kMaxSamples) samples_.pop_front();
    samples_.push_back({cycle, clamp16(left * kMasterGain), clamp16(right * kMasterGain)});

    // Operators read this frame's eg_* clock (set by the previous call); step it
    // now so the next frame sees it one tick on, as silicon does (OPL3_Generate4Ch)
    advance_envelope_clock();
}

// Chip-global envelope clock read by every envelope_shift() ("OPLx
// decapsulated"; Nuked-OPL3 eg_timer cross-check only). eg_state_ alternates
// each frame; eg_add_/eg_timer_lo_ change on the true frames, from eg_timer_'s
// trailing zero bits (a ruler sequence), with eg_timerrem_ gating the late carry.
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
