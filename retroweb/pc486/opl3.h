// Yamaha YMF262 (OPL3) FM synthesizer -- the music half of the Sound Blaster
// 16. Two-operator and four-operator FM, 18 channels over 36 operators, eight
// selectable waveforms, a five-instrument rhythm section, and the two timers
// whose status bits are the whole basis of AdLib/OPL detection.
//
// Programmed per the Yamaha YMF262-M datasheet (application manual, register
// tables and the AM/VIB/EG/KSR operator fields) together with the OPL3
// register map as the Sound Blaster Series Hardware Programming Guide
// (Creative, 1994) Appendix B presents it for the card's FM ports. MAME's
// ymf262.cpp and Nuked-OPL3 are cross-checks on table values only, never a
// behavior source, and no code is taken from either -- the same standing
// rule ay8910.h records for MAME.
//
// --- output rate ------------------------------------------------------
// The real chip divides its 14.31818 MHz master clock by 288, so it emits
// one stereo frame every 288 clocks: 49715.9 Hz. That is a property of the
// silicon, not a tunable, so it is fixed here (kSampleHz) and the front end
// resamples from the cpu_cycle stamps, exactly as the digitized-sound path
// already does. Every operator advances once per output frame; there is no
// faster internal rate to model.
//
// --- port decode ------------------------------------------------------
// The card presents two register banks through two address/data pairs:
//
//   base+0h / base+1h   bank 0 address (00h-FFh), data     -- channels 0-8
//   base+2h / base+3h   bank 1 address (100h-1FFh), data   -- channels 9-17
//   base+8h / base+9h   alias of base+0h/1h (AdLib-compatible pair)
//
// Reading base+0h (or base+8h) returns the status byte; the other three
// read as FFh on a real card. The card also answers the original AdLib
// card's own 0x388/0x389 pair as bank 0, which is where an AdLib-era music
// driver actually writes -- see soundblaster.h.
//
// --- what AdLib detection actually requires ----------------------------
// The canonical sequence every period driver runs, and the reason the
// timers are not optional:
//   1. write 60h to register 04h  (mask/reset both timers)
//   2. write 80h to register 04h  (reset the IRQ flags)
//   3. read the status port -- must be 00h
//   4. write FFh to register 02h  (timer 1 preset) and 21h to 04h (start it)
//   5. wait at least 80 us, read the status port -- must be C0h
//   6. write 60h then 80h to register 04h again
// Step 3 reading anything but 00h, or step 5 anything but C0h, and the
// driver concludes there is no OPL -- which is exactly what this machine
// did before this file existed. Timer 1 counts every 80.8 us (clock/1024)
// and timer 2 every 323.1 us (clock/4096); both count up from their preset
// to 256, set their status flag, and reload.
//
// Status byte: bit 7 = either timer expired (the IRQ line), bit 6 = timer 1,
// bit 5 = timer 2, bits 4-0 read 0 on OPL3 (an OPL2 returns 6 in bits 2-1,
// which is one way software tells the two apart -- and a reason not to
// return stray bits here).
//
// --- register map -----------------------------------------------------
// Offsets marked "per operator" use the standard OPL operator layout: the
// low 5 bits of the register index select the operator as
// 0-5, 8-13, 16-21 within each bank (indices 6, 7, 14, 15, 22-31 decode to
// no operator and are ignored).
//
//   01h        bit 5 = WSE, the waveform-select enable an OPL2 needed
//              before registers E0h-F5h did anything. Ignored while the
//              OPL3 NEW bit is set, which is how OPL3 mode always behaves.
//   02h / 03h  timer 1 / timer 2 preset
//   04h        bit 7 = IRQ reset (resets both flags, no other bit acts),
//              bits 6/5 = mask timer 1/2, bits 1/0 = start timer 2/1
//   08h        bit 7 = CSM (composite sine, OPL2 legacy), bit 6 = NTS
//              (note-select, picks which F-number bit feeds key scaling)
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
//              (feedback), 0 CNT (connection). CHA (bit 4) routes the
//              channel to the LEFT output and CHB (bit 5) to the RIGHT --
//              that orientation is the register's documented pin mapping
//              (CHA/CHB drive the mixed DO2 pair, CHC/CHD the DO0 pair),
//              not a convention chosen here. A mono-era driver never
//              touches any of the four, which is why a channel with all
//              four clear must not be silent while NEW is clear -- see the
//              105h note below.
//   E0h-F5h    per operator: bits 2-0 = waveform select (0-7; an OPL2 only
//              had 0-3)
//   104h       bits 5-0 = four-operator enable for channel pairs
//              0+3, 1+4, 2+5, 9+12, 10+13, 11+14
//   105h       bit 0 = NEW, the OPL3 enable. Clear at reset: the chip
//              powers up OPL2-compatible, mono, with only waveforms 0-3
//              and registers 104h/105h and bank 1 inert. Software that
//              never sets it must still hear sound out of both speakers,
//              which is why panning defaults to both channels enabled
//              while NEW is clear rather than reading C0h's bits 5-4.
//
// --- scope ------------------------------------------------------------
// The IRQ line is generated and reported in the status byte but is NOT
// wired to the PIC: a real SB16's OPL3 interrupt pin is not connected to
// the bus either -- drivers poll the status port, which is what the
// detection sequence above does.
#ifndef PC486_OPL3_H
#define PC486_OPL3_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace pc486 {

class Opl3 {
public:
    // 14.31818 MHz / 288, the real YMF262 output frame rate. Not tunable.
    static constexpr double kSampleHz = 14318180.0 / 288.0;
    // This machine's 66 MHz CPU clock (machine.h's kCpuHz) as the Turbo-on
    // default; set_cpu_hz() updates the live rate so Turbo-off (33 MHz)
    // keeps FM wall-clock timing correct.
    static constexpr double kCpuHz = 66000000.0;

    Opl3() { reset(); }

    void set_cpu_hz(double hz);

    // Cold power-on: every register 0, both timers stopped and their flags
    // clear, NEW clear (so the chip starts OPL2-compatible), all 36 operators
    // released and silent.
    void reset();

    // --- register access -------------------------------------------------
    // `bank` is 0 for the base+0h/1h (and base+8h/9h) pair and 1 for
    // base+2h/3h. A bank-1 write while NEW is clear reaches no register, as
    // on real silicon.
    void write_address(int bank, uint8_t v);
    void write_data(int bank, uint8_t v);

    // The status byte both status ports return: bit 7 IRQ, 6 timer 1,
    // 5 timer 2, bits 4-0 zero. Reading it has no side effect -- the flags
    // are cleared only by register 04h bit 7.
    uint8_t status() const;

    // What register 04h bit 7 resets, exposed for tests.
    bool timer1_expired() const { return timer1_flag_; }
    bool timer2_expired() const { return timer2_flag_; }
    bool irq_pending() const { return (status() & 0x80) != 0; }

    // The register file as written, for tests and for the front end's
    // "what is the FM chip doing" view. Bank 1 occupies 0x100-0x1FF.
    uint8_t reg(uint16_t index) const { return index < 0x200 ? regs_[index] : 0; }
    bool opl3_mode() const { return new_; }

    // --- pacing ----------------------------------------------------------
    // Advances the chip against the CPU's running cycle count, generating
    // output frames and stepping both timers. Inline with an early-out for
    // the same reason SoundBlaster::tick has one: Machine::run_cycles()
    // calls it constantly and a chip with nothing keyed on must cost
    // almost nothing (PC486_REVIEW.md §8).
    void tick(uint64_t cpu_cycles) {
        uint64_t delta = cpu_cycles - prev_cycles_;
        prev_cycles_ = cpu_cycles;
        if (!active_) return;
        advance(cpu_cycles, delta);
    }

    // --- audio output ----------------------------------------------------
    // Same contract as SoundBlaster::Sample: each frame carries the CPU
    // cycle it was clocked out at, so the front end resamples from real
    // time rather than this core committing to an output rate. Values are
    // signed 16-bit stereo -- the chip's own 16-bit DAC range, before the
    // CT1745's FM volume (mixer 34h/35h) is applied.
    struct Sample {
        uint64_t cpu_cycle;
        int16_t left, right;
    };
    std::vector<Sample> drain_samples();

    // True while the chip still has work to do: any operator not fully
    // released, OR either timer running. What tick()'s early-out keys off --
    // so the timer half is load-bearing, not incidental. A driver probes the
    // timers with nothing keyed on (the detection sequence above does
    // exactly that), and keying off active_ on envelope state alone would
    // stop advancing them and hang detection forever.
    bool active() const { return active_; }

    // --- register write trace (opt-in diagnostic) -------------------------
    // Off the hot path entirely when disarmed: write_data's capture is a
    // single trace_max_ != 0 check. For offline FM analysis of a real DOS
    // game's music, not part of the emulated machine -- see web/app.js's
    // window.__fm.
    struct TraceEvent {
        uint64_t cycle;
        uint16_t reg;
        uint8_t value;
    };
    void start_trace(std::size_t max_events);
    std::vector<TraceEvent> drain_trace();
    bool tracing() const { return trace_max_ != 0; }

private:
    // Same bound and reason as SoundBlaster::kMaxSamples: real hardware has
    // no such limit, this only caps memory if the front end stops draining.
    static constexpr std::size_t kMaxSamples = 1u << 16;
    // Bounds the frames one advance() call will generate. The credit for
    // every due frame is consumed regardless, so this drops audio rather
    // than letting the chip's clock slip -- see advance().
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

    // --- operator and channel state --------------------------------------
    // 36 operators and 18 channels, laid out so a register index's operator
    // slot maps straight onto op_[] -- see the register map above.
    enum class Env { kOff, kAttack, kDecay, kSustain, kRelease };

    struct Operator {
        // Register fields, decoded on write.
        uint8_t am = 0, vib = 0, egt = 0, ksr = 0, mult = 0;
        uint8_t ksl = 0, tl = 0;
        uint8_t ar = 0, dr = 0, sl = 0, rr = 0;
        uint8_t waveform = 0;
        // Running state.
        Env env = Env::kOff;
        uint32_t phase = 0;         // 10.10 fixed point into the 1024-entry table
        int32_t env_level = 511;    // attenuation in 1/8 dB units; 511 = silent
        int16_t out = 0, prev_out = 0;  // feedback history (two samples)
        bool key_on = false;
    };

    struct Channel {
        uint16_t fnum = 0;
        uint8_t block = 0;
        uint8_t feedback = 0, connection = 0;
        bool key_on = false;
        // C0h bits 7-4: output to D, C, B, A. While NEW is clear both
        // left and right are forced on -- see the 105h note above.
        bool out_left = true, out_right = true;
        // Set for the first channel of a four-operator pair, and for the
        // second (which stops producing output of its own).
        bool four_op_primary = false, four_op_secondary = false;
    };

    Operator op_[36];
    Channel ch_[18];

    uint32_t am_phase_ = 0;   // tremolo LFO, 3.7 Hz
    // Vibrato's quantised 8-step position and its 1024-frame-per-step
    // counter -- see generate_frame.
    uint8_t vib_pos_ = 0;
    uint32_t vib_frame_ = 0;
    uint32_t noise_ = 1;      // rhythm-section noise LFSR

    void write_reg(uint16_t index, uint8_t v);
    void generate_frame(uint64_t cycle);
    void step_timers(uint32_t frames);
    // Sets active_ false only when every operator is released AND both
    // timers are stopped -- see active().
    void recompute_active();
};

}  // namespace pc486

#endif  // PC486_OPL3_H
