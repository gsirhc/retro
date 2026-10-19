// Atari POKEY (Centipede, Missile Command and other Atari boards).
// Register map / poly RNG: Atari Hardware Manual; Altirra Hardware Reference;
// MAME pokey.cpp for AUDF/AUDC packing and RANDOM polarity.

#ifndef SHARED_ATARI_POKEY_H
#define SHARED_ATARI_POKEY_H

#include <array>
#include <cstdint>
#include <vector>

namespace atari {

class Pokey {
public:
    static constexpr int kClockHz = 1512000;  // 12.096 MHz / 8 on Centipede

    void set_clock(int hz) { clock_hz_ = hz; }
    // AUDIO pin into a pull-up and cap; mix() then yields pin volts (MAME pokey.cpp RC_LOWPASS).
    void set_output_rc(double r_pullup, double cap, double vref);

    // P0-P7 as read back through ALLPOT ($08); Battlezone wires switches here.
    uint8_t allpot = 0xFF;

    void reset();
    uint8_t read(uint8_t reg) const;
    void write(uint8_t reg, uint8_t v);

    // Advance the chip by `cpu_cycles` at the chip clock and append samples.
    void mix(int cpu_cycles, int out_hz, std::vector<float>& out);

    // 17-bit poly RANDOM (reads as ^0xFF; SKCTL reset yields 0xFF).
    uint8_t random() const { return uint8_t((poly17_ ^ 0xFF) & 0xFF); }

private:
    std::array<uint8_t, 4> audf_{};
    std::array<uint8_t, 4> audc_{};
    uint8_t audctl_ = 0;
    uint8_t skctl_ = 0;

    std::array<int, 4> period_{};
    std::array<int, 4> wrap_{};  // linked low channel: clocks between borrows after the first
    std::array<int, 4> count_{};
    std::array<bool, 4> out_{};

    uint32_t poly17_ = 0x1FFFF;
    uint32_t poly9_ = 0x1FF;
    uint32_t poly5_ = 0x1F;
    uint32_t poly4_ = 0xF;

    double sample_acc_ = 0;
    int clock_hz_ = kClockHz;

    bool rc_ = false;
    double r_pullup_ = 0, cap_ = 0, vref_ = 0;
    double rc_v_ = 0, rc_sum_ = 0;
    int rc_n_ = 0;
    uint16_t rc_raw_ = 0xFFFF;
    double rc_v0_ = 0, rc_mult_ = 0;

    void recalc_periods();
    bool joined_high(int ch) const;
    void tick_channel(int ch);
    void step_polys();
    float sample() const;
    uint16_t out_raw() const;
    void rc_target(uint16_t raw);
};

}  // namespace atari

#endif
