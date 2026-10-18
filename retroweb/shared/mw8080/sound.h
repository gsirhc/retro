// Space Invaders sound board: discrete analog + SN76477. Midway parts from MAME mw8080bw_a.cpp
// invaders_discrete, Taito PV game board parts from Taito schematic PVR00001.

#ifndef SHARED_MW8080_SOUND_H
#define SHARED_MW8080_SOUND_H

#include "board.h"

#include <cstdint>

namespace mw8080 {

enum class SoundBoard { Midway, TaitoPv };

constexpr int kSampleRate = 48000;  // discrete and SN76477 step rate (MAME stream rate)

namespace circuit {

struct OpAmpInfo {
    double r1, r2, r3, r4, c, vn, vp;
};

// DST_OP_AMP, Norton.
struct OpAmp {
    void init(const OpAmpInfo& info);
    double step(double in0, double in1);

    OpAmpInfo info_{};
    bool has_r1_ = false, has_r4_ = false, has_cap_ = false;
    double v_max_ = 0, i_fixed_ = 0, exponent_ = 0, v_cap_ = 0;
};

struct OneShotInfo {
    double r1, r2, r3, r4, r5, c1, c2, vn, vp;
};

// DST_OP_AMP_1SHT, DISC_OP_AMP_1SHT_1 | Norton.
struct OneShot {
    void init(const OneShotInfo& info);
    double step(double trigger);

    OneShotInfo info_{};
    double exponent1c_ = 0, exponent1d_ = 0, exponent2_ = 0, i_fixed_ = 0;
    double v_max_ = 0, r34ratio_ = 0;
    double v_cap1_ = 0, v_cap2_ = 0, v_out_ = 0;
};

// DST_TVCA_OP_AMP with f0, f1, f3 open, f2 = TRG0, and no r3, r6, r8 to r11.
struct TvcaInfo {
    double r1, r2, r4, r5, r7, c1, v1, vp;
};

struct Tvca {
    void init(const TvcaInfo& info);
    double step(double trig0, double in0);

    TvcaInfo info_{};
    double v_out_max_ = 0, v_trig_ = 0, i_fixed_ = 0, exponent_c_ = 0, exponent_d_ = 0;
    double v_cap1_ = 0;
};

enum OscType { kOsc1, kVco1, kVco3 };
enum OscOut { kOutCap, kOutSqw };

struct OscInfo {
    OscType type;
    OscOut out;
    double r1, r2, r3, r4, r5, r6, r7, r8, c, vp;
};

// DSS_OP_AMP_OSC, Norton, linear charge.
struct OpAmpOsc {
    void init(const OscInfo& info);
    double step(double vmod1, double vmod2);

    OscInfo info_{};
    double v_out_high_ = 0, threshold_low_ = 0, threshold_high_ = 0, i_fixed_ = 0, temp1_ = 0;
    double v_cap_ = 0;
    bool flip_flop_ = false, flip_flop_xor_ = false;
};

struct Astable555Info {
    bool energy_out;  // DISC_555_OUT_ENERGY, else DISC_555_OUT_SQW
    double v_pos, v_charge, v_out_high;
};

// DSD_555_ASTBL, no control voltage node.
struct Astable555 {
    void init(const Astable555Info& info, double c);
    double step(bool run, double r1, double r2);

    Astable555Info info_{};
    double c_ = 0, threshold_ = 0, trigger_ = 0;
    double last_r1_ = -1, last_r2_ = -1;
    double t_rc_bleed_ = 0, t_rc_charge_ = 0, t_rc_discharge_ = 0;
    double exp_bleed_ = 0, exp_charge_ = 0, exp_discharge_ = 0;
    double cap_voltage_ = 0;
    bool flip_flop_ = true;
};

struct RcFilter {
    void init(double r, double c);
    double step(double in);

    double exponent_ = 0, v_out_ = 0;
};

struct CrFilter {
    void init(double r, double c);
    double step(double in);

    double exponent_ = 0, v_cap_ = 0;
};

// DSS_SQUAREWFIX
struct SquareWFix {
    void init(double freq, double amp, double duty, double bias);
    double step();

    double amp_ = 0, bias_ = 0, freq_ = 0, duty_ = 0;
    double t_on_ = 0, t_off_ = 0, t_left_ = 0;
    bool flip_flop_ = true;
};

// DSS_LFSR_NOISE with midway_lfsr.
struct Noise {
    void init();
    double step();

    uint32_t reg_ = 0;
    double t_left_ = 0, out_ = 0;
};

// DST_MIXER, DISC_MIXER_IS_OP_AMP, rI = 0, cF = 0.
struct Mixer {
    void init();
    double step(const double (&in)[6]);

    double exponent_rc_ = 0, exponent_amp_ = 0;
    double v_cap_ = 0, v_cap_amp_ = 0;
};

// TI SN76477 as configured for the saucer, MAME sn76477.cpp.
struct Sn76477 {
    void init();
    void enable_w(bool state);
    double step();

    double v_pin_ = 0;
    bool enable_ = true;
    bool one_shot_running_ = false;
    double slf_cap_voltage_ = 0, vco_cap_voltage_ = 0;
    bool slf_out_ = false, vco_out_ = false;
    double slf_charge_ = 0, slf_discharge_ = 0, vco_step_ = 0;
};

// PVR00001 output stage: each AMP line through 1uF, a 50K trimmer and 47K into the summing amp.
struct TaitoMixer {
    void init();
    double step(const double (&in)[7]);

    double v_cap_[7]{};
    double v_cap_out_ = 0;
};

}  // namespace circuit

class Sound {
public:
    explicit Sound(SoundBoard board = SoundBoard::Midway) : board_(board) { reset(); }

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
    double fleet_555() const { return fleet_555_out_; }
    bool ufo_vco() const { return sn_.vco_out_; }
    double ufo_slf_voltage() const { return sn_.slf_cap_voltage_; }

private:
    void tick();

    SoundBoard board_;
    uint8_t p3_ = 0;
    uint8_t p5_ = 0;

    int64_t cycle_acc_ = 0;
    uint64_t ticks_ = 0;
    double sum_ = 0;
    int count_ = 0;

    circuit::Noise noise_;
    circuit::OpAmp saucer_b3_9_, saucer_b3_10_;
    circuit::OpAmpOsc saucer_osc_, saucer_vco_;

    double thump_r_[16]{};
    circuit::Astable555 fleet_555_;
    circuit::RcFilter fleet_rc1_, fleet_rc2_;
    double fleet_555_out_ = 0;

    circuit::Astable555 bonus_555_;
    circuit::SquareWFix bonus_sqw_;

    circuit::OneShot hit_1sht_;
    circuit::OpAmp hit_d3_10_, hit_d3_4_;
    circuit::OpAmpOsc hit_osc_, hit_vco_;

    circuit::OneShot expl_1sht_;
    circuit::Tvca expl_tvca_;
    circuit::RcFilter expl_rc1_, expl_rc2_;

    circuit::OneShot missile_1sht_;
    circuit::OpAmp missile_b3_, missile_a3_;
    circuit::CrFilter missile_cr_;
    circuit::OpAmpOsc missile_osc_;
    circuit::Tvca missile_tvca_;

    circuit::Mixer mixer_;
    circuit::TaitoMixer taito_mixer_;
    circuit::Sn76477 sn_;
};

}  // namespace mw8080

#endif
