// Battlezone auxiliary PCB audio at $1840 (Atari DP-156 sheet 3B, Audio Output).

#ifndef BATTLEZONE_SOUND_H
#define BATTLEZONE_SOUND_H

#include <cstdint>

namespace battlezone {

class Sound {
public:
    struct BurstParts { double c_hold, r_series, r_shunt, r13, r12, rf, cf; };

    void reset();
    void set_rate(int hz);
    // M2 LS273 output latch.
    void write(uint8_t v) { latch = v; }
    // One output sample; pokey is the POKEY AUD pin in volts.
    float sample(float pokey);

    uint8_t latch = 0;
    // F3 555 rate for the current control voltage.
    double engine_hz() const;
    // C13, buffered by K5 onto the 555 control pin.
    double engine_cv() const { return c13_; }
    bool shell_gate() const { return shell_ff_; }
    bool explosion_gate() const { return expl_ff_; }
    // K5 mixer output (AUD+) in volts, for tests.
    double aud_plus() const { return aud_plus_; }
    double shell_out() const { return shell_.out; }
    double explosion_out() const { return expl_.out; }

private:
    struct Burst {
        double cap = 0;   // C9 / C14
        double vf = 0;    // voltage across the R32/R33 || C21/C22 feedback
        double out = 0;
    };

    int hz_ = 48000;
    int sub_ = 4;
    double dt_ = 1.0 / 192000;

    uint16_t lfsr_ = 0;
    double noise_clk_ = 0;
    bool noise_div_ = false;
    bool shell_ff_ = false;
    bool expl_ff_ = false;
    bool qh_ = false;
    bool nand4_ = true;

    Burst shell_, expl_;

    double c13_ = 0;
    double c11_ = 0;
    bool f3_high_ = true;
    int f4_ = 0, f5_ = 0;
    double c29_ = 0;

    double c20_ = 0;
    double aud_plus_ = 0;
    double dc_ = 0;

    void step(double pokey);
    void clock_noise();
    void step_burst(Burst& b, const BurstParts& p, bool enable, bool gate, bool loud);
    void step_555();
    void clock_counters();
};

}  // namespace battlezone

#endif
