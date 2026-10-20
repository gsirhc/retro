// Atari POKEY C012294 (Atari POKEY data sheet, drawing C012294 rev B).

#ifndef SHARED_ATARI_POKEY_H
#define SHARED_ATARI_POKEY_H

#include <array>
#include <cstdint>
#include <vector>

namespace atari {

class Pokey {
public:
    static constexpr int kClockHz = 1512000;  // 12.096 MHz / 8 on these boards

    void set_clock(int hz) { clock_hz_ = hz; }

    // AUD pin into a pull-up to `vpull` and a cap to ground; mix() yields pin volts.
    void set_output_rc(double r_pullup, double cap, double vpull);
    // AUD pin into an op-amp held at `vref` with rf || cf feedback; mix() yields op-amp volts.
    void set_output_transimpedance(double rf, double cf, double vref, double vmax);

    // P0-P7 line states as read back through ALLPOT ($08).
    uint8_t allpot = 0xFF;

    void reset();
    uint8_t read(uint8_t reg) const;
    void write(uint8_t reg, uint8_t v);

    // Advance by `chip_clocks` and append output samples at out_hz. With no
    // load set, a sample is the mean count of on devices over 60.
    void mix(int chip_clocks, int out_hz, std::vector<float>& out);

    uint8_t random() const;
    // Sum of the volume units sinking current right now (0..60).
    int units() const;
    bool channel_out(int ch) const { return out_[size_t(ch)]; }

    // Sink current of one unit device (W/L 1) at pin voltage v.
    static double unit_current(double v);

private:
    enum class Load { None, Pullup, Transimpedance };

    std::array<uint8_t, 4> audf_{};
    std::array<uint8_t, 4> audc_{};
    uint8_t audctl_ = 0;
    uint8_t skctl_ = 0;

    std::array<int, 4> count_{};
    std::array<bool, 4> out_{};
    std::array<bool, 2> hipass_{};
    int prescale_ = 0;

    uint32_t poly4_ = 0xF, poly5_ = 0x1F, poly9_ = 0x1FF, poly17_ = 0x1FFFF;

    int clock_hz_ = kClockHz;
    double sample_acc_ = 0;
    double sum_ = 0;
    int sum_n_ = 0;

    Load load_ = Load::None;
    double r_ = 0, c_ = 0, vsrc_ = 0, vmax_ = 0;
    double v_ = 0;

    void clock();
    void step_polys();
    bool fast(int ch) const;
    bool joined_high(int ch) const;
    bool joined_low(int ch) const;
    int period(int ch) const;
    void reload(int ch);
    void pulse(int ch);
    double step_output();
};

}  // namespace atari

#endif
