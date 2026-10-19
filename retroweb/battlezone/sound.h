// Battlezone discrete sound board at $1840, ported from MAME bzone_a.cpp.

#ifndef BATTLEZONE_SOUND_H
#define BATTLEZONE_SOUND_H

#include <cstdint>

namespace battlezone {

class Sound {
public:
    void reset();
    void set_rate(int hz);
    void write(uint8_t v) { latch = v; }
    // One output sample; pokey is the POKEY AUDIO pin in volts.
    float sample(float pokey);

    uint8_t latch = 0;
    double engine_hz() const;

private:
    int hz_ = 48000;
    double dt_ = 1.0 / 48000;

    uint32_t lfsr_ = 0;
    double noise_phase_ = 0;
    bool noise_ = false;
    bool div31_ = false;
    bool nand33_ = false;
    bool div34_ = false;

    double shell_cap_ = 0, shell_out_ = 0;
    double expl_cap_ = 0, expl_out_ = 0;

    double eng_cv_ = 0;
    double vco_cap_ = 0;
    bool vco_out_ = true;
    int f4_ = 4, f5_ = 6;
    double eng_mix_ = 0;

    double hp_in_ = 0, hp_out_ = 0;

    void clock_noise();
    double rc_circuit(double& cap, bool charge, bool gate, double r, double c) const;
    struct Parts { double r1, r2, r3, r4, r5, c; };
    double filter(double& out, bool loud, double in, const Parts& p) const;
    void step_vco();
};

}  // namespace battlezone

#endif
