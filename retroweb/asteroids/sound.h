// Asteroids discrete audio (Atari DP-143 sheet 2B, Outputs and Audio Output).

#ifndef ASTEROIDS_SOUND_H
#define ASTEROIDS_SOUND_H

#include <array>
#include <cstdint>

namespace asteroids {

class Sound {
public:
    void reset();
    void set_rate(int hz);

    void write_explode(uint8_t v) { explode_ = v; }  // P7 LS174, $3600
    void write_thump(uint8_t v) { thump_ = v; }      // N7 LS174, $3A00
    void write_latch(int q, bool d);                 // M10 LS259 Q0-Q5, $3C00-$3C05
    void noise_reset() { lfsr_ = 0; }                // $3E00

    float sample();

    uint8_t explode() const { return explode_; }
    uint8_t thump() const { return thump_; }
    bool latch(int q) const { return (latch_ >> q) & 1; }

    // P11 mixer output (AUDIO 1) in volts, for tests.
    double mix_out() const { return mix_; }
    double thump_hz() const;
    double saucer_hz() const;
    double ship_fire_hz() const { return fire_hz(ship_); }
    double saucer_fire_hz() const { return fire_hz(saucer_fire_); }
    bool noise() const;
    bool explosion_ff() const { return exp_ff_; }
    double ship_fire_out() const { return ship_.node; }

private:
    struct Fire {
        double rq = 0, cq = 0, r_out = 0;  // R54 C38 R58 / R52 C47 R66
        double ctrl = 5.0;  // C38 / C47
        double cap = 0;     // C35 / C50
        bool out = false;
        double amp = 5.0;   // C39 / C48
        double node = 0;
    };

    int hz_ = 48000;
    int sub_ = 4;
    double dt_ = 1.0 / 192000;

    uint8_t explode_ = 0, thump_ = 0, latch_ = 0;

    uint16_t lfsr_ = 0;
    double clk12_ = 0, clk3_ = 0;
    bool c12_ = false, c3_ = false;
    int p8_ = 0;
    bool exp_ff_ = false;
    double c24_ = 0;

    double c33_ = 0;
    bool n8_high_ = false;
    double c64_ = 0;

    double c45_ = 0;
    bool r10_high_ = false;
    double tri_phase_ = 0;

    Fire saucer_fire_{10e3, 10e-6, 10e3};
    Fire ship_{33e3, 1e-6, 2.7e3};

    double n1_ = 0, n3_ = 0, c67_ = 0, vo2_ = 0;

    double mix_ = 5.0;
    double hp_ = 0;

    void step();
    void clock12_rise();
    double thump_current() const;
    double saucer_ic() const;
    void step_fire(Fire& f, bool en);
    static double fire_current(const Fire& f, double vcap);
    static double fire_hz(const Fire& f);
};

}  // namespace asteroids

#endif
