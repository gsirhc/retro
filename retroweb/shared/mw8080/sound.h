// Space Invaders sound: Midway 739 logic board (A084-90700-B000) and Taito PV game board (PVR00001).

#ifndef SHARED_MW8080_SOUND_H
#define SHARED_MW8080_SOUND_H

#include "board.h"

#include <cstdint>

namespace mw8080 {

enum class SoundBoard { Midway, TaitoPv };

constexpr int kSampleRate = 48000;

namespace analog {

// One-shot on an LM3900: timing cap charges from the output through r_out and
// bleeds into the inverting input through r_in, a diode across r_out dumps it.
struct NortonOneShot {
    double r_bias = 0, r_in = 0, r_out = 0, c = 0, r_fb = 0, r_trig = 0, c_trig = 0;

    void reset();
    void step(double v_line, double dt);
    double out() const;

    bool high = false;
    double v_cap = 0;
    double v_trig = 0;
};

// Integrator plus Schmitt on two LM3900 sections: the square drives the
// integrator's + input, the ramp drives the Schmitt's - input.
struct NortonRamp {
    double c = 0, r_bias = 0, r_sq = 0, r_ref = 0, r_hys = 0, r_cmp = 0;

    void reset();
    void step(double i_minus_extra, double dt);
    double square() const;

    double v_ramp = 0;
    bool high = false;
};

// Linear VCO: v_in feeds both integrator inputs, a switch on the + input
// flips the slope, a non-inverting Schmitt drives the switch.
struct NortonVco {
    double c = 0, r_minus = 0, r_plus = 0, r_in = 0, r_hys = 0, r_ref = 0;

    void reset();
    void step(double v_in, double dt);

    double v_ramp = 0;
    bool high = false;
};

struct Astable555 {
    double v_cc = 0, c = 0;

    void reset();
    // r_a = 0 with v_src = 0 stops charging; reset low holds the discharge on.
    void step(double v_src, double r_a, double r_b, bool run, double dt);

    double v_cap = 0;
    bool out = false;
};

// Two-section RC ladder into a resistive load, implicit Euler.
struct RcLadder {
    double r1 = 0, c1 = 0, r2 = 0, c2 = 0, r_load = 0;

    void reset();
    double step(double v_in, double dt);

    double v1 = 0, v2 = 0;
};

// TI SN76477 wired as on both boards: VCO swept by the SLF, mixer VCO only,
// one-shot envelope with no one-shot resistor, pin 9 driven from bit 0.
struct Sn76477 {
    double r_slf = 0, c_slf = 0, r_vco = 0, c_vco = 0, r_amp = 0, r_fb = 0;

    void reset();
    void inhibit_w(bool high);
    double step(double dt);

    double v_slf = 0.35;
    bool slf_rising = true;
    double vco_phase = 0;
    bool vco_out = false;
    bool inhibit = true;
    bool latch = false;
};

}  // namespace analog

class Sound {
public:
    explicit Sound(SoundBoard board = SoundBoard::Midway);

    void reset();
    void port3_w(uint8_t v);
    void port5_w(uint8_t v);
    void advance(int cpu_cycles);
    // Mean output since the previous call, -1..1.
    float mix();

    bool amp_enabled() const { return (p3_ & 0x20) != 0; }
    bool flip_screen() const { return (p5_ & 0x20) != 0; }

    // Probes for tests.
    uint64_t ticks() const { return ticks_; }
    double fleet_555() const { return fleet_out_; }
    double fleet_cap() const { return fleet_.v_cap; }
    bool ufo_vco() const { return sn_.vco_out; }
    double ufo_slf_voltage() const { return sn_.v_slf; }
    bool noise() const { return (lfsr_ >> 12) & 1; }
    double missile_one_shot() const { return missile_os_.out(); }
    double missile_pitch() const { return missile_ramp_; }
    double explosion_env() const { return expl_env_; }
    double hit3_env() const { return hit3_env_; }
    double hit5_env() const { return hit5_env_; }
    double hit3_saw() const { return hit3_saw_.v_ramp; }
    double hit3_vco() const { return hit3_vco_.v_ramp; }
    double hit5_tri() const { return hit5_tri_.v_ramp; }
    double hit5_vco() const { return hit5_vco_.v_ramp; }
    double bonus_out() const { return bonus_out_; }
    // Bus current per source before the summing amp, amps.
    double source_current(int i) const { return src_i_[i]; }
    // Summing amp output relative to its bias point, volts.
    double amp_out() const { return amp_v_; }

    enum Source { kUfo, kMissile, kExplosion, kHit3, kBonus, kFleet, kHit5, kSources };

private:
    void tick();

    SoundBoard board_;
    uint8_t p3_ = 0;
    uint8_t p5_ = 0;

    int64_t cycle_acc_ = 0;
    uint64_t ticks_ = 0;
    double sum_ = 0;
    int count_ = 0;
    float last_ = 0;

    double noise_phase_ = 0;
    uint32_t lfsr_ = 1;
    double noise_c_ = 0;

    analog::Sn76477 sn_;

    analog::NortonOneShot missile_os_;
    double missile_ramp_ = 0;
    analog::NortonRamp missile_osc_;
    double missile_gate_ = 0;
    double missile_c11_ = 0;

    analog::NortonOneShot expl_os_;
    double expl_env_ = 0;
    analog::RcLadder expl_rc_;

    analog::NortonRamp hit3_saw_;
    analog::NortonVco hit3_vco_;
    analog::NortonOneShot hit3_os_;
    double hit3_env_ = 0;

    analog::NortonRamp hit5_tri_;
    analog::NortonVco hit5_vco_;
    double hit5_env_ = 0;

    analog::Astable555 fleet_;
    double fleet_out_ = 0;
    analog::RcLadder fleet_rc_;

    analog::Astable555 bonus_;
    double tone_phase_ = 0;
    double bonus_out_ = 0;

    double src_i_[kSources]{};
    double hpf_i_ = 0;
    double hpf_prev_ = 0;
    double c42_ = 0;
    double amp_v_ = 0;
};

}  // namespace mw8080

#endif
