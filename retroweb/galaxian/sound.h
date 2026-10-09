// Namco Galaxian (1979) discrete analog sound, MAME galaxian_a.cpp netlist (Couriersud).
//
// 74LS259 latch at $6800–$6807, 4-bit background DAC at $6004–$6007, 8-bit pitch at $7800.

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
    // Mean output voltage since the previous call (1 V = full scale, MAME DISCRETE_OUTPUT).
    float mix();

    // Probes for tests.
    double background_cv() const { return bck_cv_; }
    double fire_cv() const { return fire_cv_; }
    bool noise() const { return noise_q_; }

private:
    struct Astable {
        double v = 0;
        bool charging = true;
    };

    int cpu_rem_ = 0;
    int clock_rem_ = 0;
    int note_cnt1_ = 0;
    int note_cnt2_ = 0;
    int qa_acc_ = 0, qc_acc_ = 0, qd_acc_ = 0;
    uint64_t lfsr_clock_ = 0;
    int line_phase_ = 0;
    bool noise_q_ = false;

    double bck_cap_ = 0;
    double bck_cv_ = 0;
    Astable fs555_[3];
    double bck_mix_ = 0;

    double hit_cap_ = 0;
    double bp_x1_ = 0, bp_x2_ = 0, bp_y1_ = 0, bp_y2_ = 0;

    double fire_rc_ = 0;
    double fire_cv_ = 0;
    Astable fire555_;
    double fire_cap_ = 0;

    double c26_ = 0;
    double c46_ = 0;
    double out_ = 0;
    double sum_ = 0;
    int n_ = 0;

    void step(double qa, double qc, double qd, bool noise);
};

}  // namespace galaxian

#endif
