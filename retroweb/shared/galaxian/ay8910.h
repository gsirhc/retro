// General Instrument AY-3-8910 PSG, the sound chip on Konami Galaxian-family
// boards. Clocked at 1.789772 MHz (14.31818 / 8). Three square tones, 17-bit
// noise LFSR, 16-step envelope, 16-level logarithmic DAC per the datasheet.

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

    // Advance `ay_cycles` at the chip clock; append mono samples at host_hz.
    void advance(int ay_cycles, int host_hz, std::vector<float>& out);
    float mix() const;
    void tick();
    // DAC level 0-15 on a channel's output this clock; 0 while the mixer gates it off.
    uint8_t output_level(int ch) const;
    // Source-follower output as a Thevenin resistance to 5 V into a 1k load (MAME ay8910.cpp mosfet model).
    static double output_resistance(uint8_t level);

private:
    uint64_t ay_cycle_ = 0;
    double host_acc_ = 0;

    int tone_cnt_[3]{};
    bool tone_out_[3]{};
    int noise_cnt_ = 0;
    uint32_t noise_lfsr_ = 1;
    bool noise_out_ = false;
    int env_cnt_ = 0;
    int env_pos_ = 0;
    int env_step_ = 1;
    bool env_hold_ = false;

    void restart_envelope();
    uint8_t env_level() const;
};

}  // namespace galaxian

#endif
