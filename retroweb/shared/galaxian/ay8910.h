// General Instrument AY-3-8910 PSG (GI AY-3-8910/8912 data sheet), clocked
// at 14.31818 / 8 = 1.789772 MHz on the Konami sound boards.

#ifndef GALAXIAN_AY8910_H
#define GALAXIAN_AY8910_H

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace galaxian {

class Ay8910 {
public:
    std::array<uint8_t, 16> regs{};
    uint8_t addr = 0;
    std::function<uint8_t()> port_a_r;
    std::function<uint8_t()> port_b_r;
    bool mute = false;

    void reset();
    void write_addr(uint8_t v);
    void write_data(uint8_t v);
    uint8_t read_data() const;

    // Advance `ay_cycles` input clocks; append the mean of the three channel
    // voltages at host_hz (0 = no output).
    void advance(int ay_cycles, int host_hz, std::vector<float>& out);
    float mix() const;
    void tick();
    // DAC level 0-15 on a channel's output this clock; 0 while the mixer gates it off.
    uint8_t output_level(int ch) const;
    // Channel output in volts for a DAC level: 1 V at 15, 3 dB per step, 0 V at 0.
    static double dac_volts(uint8_t level);

private:
    double host_acc_ = 0;
    double host_sum_ = 0;
    int host_n_ = 0;

    int tone_cnt_[3]{};
    bool tone_out_[3]{};
    int noise_cnt_ = 0;
    uint32_t noise_sr_ = 1;
    int env_cnt_ = 0;
    int env_step_ = 0;
    bool env_attack_ = false;
    bool env_done_ = false;

    void restart_envelope();
    uint8_t env_level() const;
};

}  // namespace galaxian

#endif
