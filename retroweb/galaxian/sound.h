// Namco/Midway Galaxian (1979) discrete sound (Midway logic schematic A082-91352-A000, page 22).
//
// 74259 9L at $6800-$6807 (FS1-3, HIT, FIRE, VOL1/2), 9M Q4-Q7 at $6004-$6007
// (background ramp DAC), 74273 9J pitch latch at $7800.

#ifndef GALAXIAN_SOUND_H
#define GALAXIAN_SOUND_H

#include <cstdint>

namespace galaxian {

constexpr double kSoundClock = 1536000.0;  // 2H, 18.432 MHz / 12

class DiscreteSound {
public:
    struct Astable {
        double v = 0;
        bool charging = true;
    };

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
    // NOISE: 7474 2D Q. Clock pin is 2V (A082-91352-A000 p.22).
    void set_noise(bool on) { noise_ = on; }
    void advance(int cpu_cycles);
    // Mean LM377 input voltage (across R92) since the previous call.
    float mix();

    // Probes for tests.
    double background_cv() const { return bck_cv_; }
    double ramp_volts() const { return c15_; }
    double fire_cv() const { return fire_cv_; }
    double hit_out() const { return bp_vo_; }
    bool noise() const { return noise_; }

private:
    int cpu_rem_ = 0;
    uint8_t pitch_cnt_ = 0xFF;
    bool rco_ = true;
    uint8_t q393_ = 0;
    bool noise_ = true;

    double c15_ = 0;
    double bck_cv_ = 0;
    Astable fs555_[3];
    double node_b_ = 0;

    double c21_ = 0;
    double bp_v1_ = 0;
    double bp_vo_ = 2.0;

    double c25_ = 0;
    double c28_ = 0;
    double fire_cv_ = 0;
    Astable fire555_;
    double c26_ = 0;

    double c46_ = 0;
    double out_ = 0;
    double sum_ = 0;
    int n_ = 0;

    void step(double qa, double qc, double qd);
};

}  // namespace galaxian

#endif
