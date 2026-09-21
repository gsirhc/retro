// Namco Galaxian (1979) discrete analog sound.
//
// 74LS259 at $6800–$6807 (FS1/FS2/FS3, HIT, FIRE, VOL1/VOL2), 4-bit LFO
// DAC at $6004–$6007, 8-bit pitch at $7800. Pitch is two LS164s clocked
// at SOUND_CLOCK/(256−pitch) into a 74393 (QA/QC/QD mixed always, not
// gated by FIRE). FIRE is a decaying 555 shot (R41·C25). HIT is LFSR
// noise through RCDISC (R35+R36)·C21. Labelled RC-time-constant model of
// the Midway/Namco schematic, not a SPICE netlist. MAME galaxian_a.cpp is
// a cross-check of the latch map and the RC values.

#ifndef GALAXIAN_SOUND_H
#define GALAXIAN_SOUND_H

#include <cstdint>

namespace galaxian {

constexpr double kSoundClock = 1536000.0;  // 18.432 MHz / 6 / 2

class DiscreteSound {
public:
    bool fs[3]{};
    bool hit = false;
    bool fire = false;
    bool vol[2]{};
    uint8_t lfo_bits = 0;
    uint8_t pitch = 0xFF;

    void reset();
    void sound_w(int offset, bool on);
    void lfo_freq_w(int bit, bool on);
    void pitch_w(uint8_t data);
    void advance(int cpu_cycles);
    float mix() const;

private:
    bool fire_prev_ = false;
    double lfo_phase_ = 0;
    double fs_phase_[3]{};
    double qa_phase_ = 0;
    double qc_phase_ = 0;
    double qd_phase_ = 0;
    double fire_phase_ = 0;
    double fire_env_ = 0;
    double hit_env_ = 0;
    uint32_t lfsr_ = 1;
    int lfsr_div_ = 0;
};

}  // namespace galaxian

#endif
