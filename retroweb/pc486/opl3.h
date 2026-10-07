// Yamaha YMF262 (OPL3) FM synthesizer, the music half of the Sound Blaster 16.
// 18 channels over 36 operators, 4-operator mode, eight waveforms, rhythm section,
// and the two timers AdLib/OPL detection depends on.
// Per the YMF262-M datasheet and SBPG (Creative, 1994) Appendix B. MAME ymf262.cpp
// and Nuked-OPL3 cross-check table values only; no code taken (see ay8910.h).
// --- output rate ---
// Master clock 14.31818 MHz / 288 = 49715.9 Hz stereo frames (kSampleHz). Fixed by
// the silicon; the front end resamples from cpu_cycle stamps. Every operator
// advances once per frame.
// --- port decode ---
//   base+0h / base+1h   bank 0 address (00h-FFh), data     channels 0-8
//   base+2h / base+3h   bank 1 address (100h-1FFh), data   channels 9-17
//   base+8h / base+9h   alias of base+0h/1h (AdLib pair)
// Reading base+0h or base+8h returns status; the others read FFh. The AdLib
// 0x388/0x389 pair maps to bank 0 (see soundblaster.h).
// --- AdLib detection ---
//   1. write 60h to reg 04h (mask/reset both timers)
//   2. write 80h to reg 04h (reset IRQ flags)
//   3. read status, must be 00h
//   4. write FFh to reg 02h (timer 1 preset), 21h to 04h (start it)
//   5. wait 80 us, read status, must be C0h
//   6. write 60h then 80h to reg 04h
// Anything else and the driver concludes there is no OPL. Timer 1 counts every
// 80.8 us (clock/1024), timer 2 every 323.1 us (clock/4096); up from preset to
// 256, set the flag, reload.
// Status: bit 7 any timer expired (IRQ), bit 6 timer 1, bit 5 timer 2, bits 4-0
// zero on OPL3 (OPL2 returns 6 in bits 2-1, which software uses to tell them apart).
//
// --- register map ---
// "per operator" registers index operators 0-5, 8-13, 16-21 within each bank
// (low 5 bits; 6, 7, 14, 15, 22-31 decode to nothing).
//
// 01h        bit 5 = WSE, OPL2 waveform-select enable. Ignored while NEW is set.
//   02h / 03h  timer 1 / timer 2 preset
//   04h        bit 7 = IRQ reset (resets both flags, no other bit acts),
//              bits 6/5 = mask timer 1/2, bits 1/0 = start timer 2/1
// 08h        bit 7 = CSM (OPL2 legacy), bit 6 = NTS (note-select, F-number
//             bit feeding key scaling)
//   20h-35h    per operator: bit 7 AM, 6 VIB, 5 EGT (sustain hold),
//              4 KSR, 3-0 MULT (frequency multiple)
//   40h-55h    per operator: bits 7-6 KSL (key scale level), 5-0 TL
//              (total level, 0.75 dB per step, 63 = silence)
//   60h-75h    per operator: bits 7-4 AR (attack rate), 3-0 DR (decay)
//   80h-95h    per operator: bits 7-4 SL (sustain level), 3-0 RR (release)
//   A0h-A8h    per channel: F-number low 8 bits
//   B0h-B8h    per channel: bit 5 KON (key on), 4-2 BLOCK (octave),
//              1-0 F-number high 2 bits
//   BDh        bit 7 = deep tremolo, 6 = deep vibrato, 5 = rhythm enable,
//              4-0 = key-on for bass drum, snare, tom-tom, top cymbal,
//              hi-hat. Rhythm mode takes over channels 6, 7 and 8.
//   C0h-C8h    per channel: bit 7 CHD, 6 CHC, 5 CHB, 4 CHA, 3-1 FB
// C0h-C8h    per channel: bit 7 CHD, 6 CHC, 5 CHB, 4 CHA, 3-1 FB, 0 CNT.
//             CHA routes LEFT and CHB RIGHT per the pin mapping (CHA/CHB drive
//             the DO2 pair, CHC/CHD DO0). Mono-era drivers never touch them,
//             so panning is forced on both sides while NEW is clear (105h).
//   E0h-F5h    per operator: bits 2-0 = waveform select (0-7; an OPL2 only
//              had 0-3)
//   104h       bits 5-0 = four-operator enable for channel pairs
//              0+3, 1+4, 2+5, 9+12, 10+13, 11+14
// 105h       bit 0 = NEW, the OPL3 enable. Clear at reset: OPL2-compatible,
//             mono, waveforms 0-3, bank 1 and 104h/105h inert.
//
// --- scope ---
// The IRQ is reported in status but not wired to the PIC: a real SB16's OPL3
// interrupt pin isn't connected either. Drivers poll status.
#ifndef PC486_OPL3_H
#define PC486_OPL3_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace pc486 {

class Opl3 {
public:
    // 14.31818 MHz / 288, the real YMF262 output frame rate
    static constexpr double kSampleHz = 14318180.0 / 288.0;
    // Turbo-on CPU clock (machine.h kCpuHz); set_cpu_hz() tracks the live rate
    static constexpr double kCpuHz = 66000000.0;

    Opl3() { reset(); }

    void set_cpu_hz(double hz);

    // Cold power-on: registers 0, timers stopped, NEW clear, operators released
    void reset();

    // --- register access ---
    // `bank` 0 is base+0h/1h (and 8h/9h), 1 is base+2h/3h. A bank-1 write while
    // NEW is clear reaches no register.
    void write_address(int bank, uint8_t v);
    void write_data(int bank, uint8_t v);

    // Status byte: bit 7 IRQ, 6 timer 1, 5 timer 2. No side effect; flags clear
    // only via reg 04h bit 7.
    uint8_t status() const;

    bool timer1_expired() const { return timer1_flag_; }
    bool timer2_expired() const { return timer2_flag_; }
    bool irq_pending() const { return (status() & 0x80) != 0; }

    // Register file as written, for tests and the front end's FM view. Bank 1 is 0x100-0x1FF.
    uint8_t reg(uint16_t index) const { return index < 0x200 ? regs_[index] : 0; }
    bool opl3_mode() const { return new_; }

    // --- pacing ---
    // Advances against the CPU's cycle count: output frames and timers. Inline
    // with an early-out so an idle chip is nearly free (PC486_REVIEW.md §8).
    void tick(uint64_t cpu_cycles) {
        uint64_t delta = cpu_cycles - prev_cycles_;
        prev_cycles_ = cpu_cycles;
        if (!active_) return;
        advance(cpu_cycles, delta);
    }

    // --- audio output ---
    // Same contract as SoundBlaster::Sample: each frame carries its CPU cycle.
    // Signed 16-bit stereo, before the CT1745 FM volume (mixer 34h/35h).
    struct Sample {
        uint64_t cpu_cycle;
        int16_t left, right;
    };
    std::vector<Sample> drain_samples();

    // True while any operator isn't fully released or either timer runs. The
    // timer half matters: detection probes timers with nothing keyed on.
    bool active() const { return active_; }

    // --- register write trace ---
    // Opt-in diagnostic for offline FM analysis, one check when disarmed (web/app.js window.__fm).
    struct TraceEvent {
        uint64_t cycle;
        uint16_t reg;
        uint8_t value;
    };
    void start_trace(std::size_t max_events);
    std::vector<TraceEvent> drain_trace();
    bool tracing() const { return trace_max_ != 0; }

private:
    // Same bound as SoundBlaster::kMaxSamples; caps memory if nobody drains
    static constexpr std::size_t kMaxSamples = 1u << 16;
    // Bounds frames per advance(). Excess audio is dropped rather than letting the clock slip.
    static constexpr uint64_t kMaxCatchUpFrames = 8192;

    void advance(uint64_t cpu_cycles, uint64_t delta);

    uint8_t regs_[0x200] = {};
    uint8_t addr_[2] = {0, 0};
    bool new_ = false;

    uint8_t timer1_preset_ = 0, timer2_preset_ = 0;
    bool timer1_run_ = false, timer2_run_ = false;
    bool timer1_mask_ = false, timer2_mask_ = false;
    bool timer1_flag_ = false, timer2_flag_ = false;

    bool active_ = false;
    uint64_t prev_cycles_ = 0;
    double frame_credit_ = 0.0;
    double cpu_hz_ = kCpuHz;

    std::deque<Sample> samples_;

    std::vector<TraceEvent> trace_;
    std::size_t trace_max_ = 0;

    // --- operator and channel state ---
    // 36 operators, 18 channels, indexed by register slot. The chip has only these four
    // envelope states (Niemitalo & Gambrell, "OPLx decapsulated"); a released
    // operator pins attenuation at maximum (eg_off in generate_frame) instead of a fifth state.
    enum class Env { kAttack, kDecay, kSustain, kRelease };

    struct Operator {
        uint8_t am = 0, vib = 0, egt = 0, ksr = 0, mult = 0;
        uint8_t ksl = 0, tl = 0;
        uint8_t ar = 0, dr = 0, sl = 0, rr = 0;
        uint8_t waveform = 0;
        // Attenuation is a 9-bit counter (0-511), 0.1875 dB/unit, 511 = silent, no
        // fractional precision (envelope_shift() in opl3.cpp). Defaults to released.
        Env env = Env::kRelease;
        uint32_t phase = 0;         // 10.10 fixed point into the 1024-entry table
        int32_t env_level = 511;
        int16_t out = 0, prev_out = 0;  // feedback history (two samples)
        bool key_on = false;
    };

    struct Channel {
        uint16_t fnum = 0;
        uint8_t block = 0;
        uint8_t feedback = 0, connection = 0;
        bool key_on = false;
        // C0h bits 7-4: output to D, C, B, A. Both sides forced on while NEW is clear.
        bool out_left = true, out_right = true;
        // First channel of a four-operator pair, and the second (no output of its own)
        bool four_op_primary = false, four_op_secondary = false;
    };

    Operator op_[36];
    Channel ch_[18];

    uint32_t am_phase_ = 0;   // tremolo LFO, 3.7 Hz
    // Vibrato's 8-step position and 1024-frame-per-step counter
    uint8_t vib_pos_ = 0;
    uint32_t vib_frame_ = 0;
    uint32_t noise_ = 1;      // rhythm-section noise LFSR

    // Chip-global envelope clock shared by all operators (Niemitalo & Gambrell),
    // advanced per frame by advance_envelope_clock(). eg_state_ alternates each
    // frame; eg_add_/eg_timer_lo_ derive from eg_timer_ on the active frames.
    uint64_t eg_timer_ = 0;
    bool eg_timerrem_ = false;
    bool eg_state_ = false;
    int eg_add_ = 0;
    uint8_t eg_timer_lo_ = 0;
    void advance_envelope_clock();

    void write_reg(uint16_t index, uint8_t v);
    void generate_frame(uint64_t cycle);
    void step_timers(uint32_t frames);
    // Clears active_ only when every operator is released and both timers stopped
    void recompute_active();
};

}  // namespace pc486

#endif  // PC486_OPL3_H
