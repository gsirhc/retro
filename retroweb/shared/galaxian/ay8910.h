// General Instrument AY-3-8910 PSG — the sound chip on Konami Galaxian-family
// boards (Frogger, Scramble, …).
//
// Clocked at 1.789772 MHz (14.31818 / 8), same as the sound Z80. Three square
// tones, a 17-bit noise LFSR, and a 16-step envelope. I/O ports A/B are
// general-purpose. Volume is the documented 16-level logarithmic DAC.
// Envelope and mixer follow the AY-3-8910 datasheet; MAME ay8910.cpp is a
// cross-check of the envelope shapes, not a behavior source.

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

    void tick();
    void restart_envelope();
    uint8_t env_level() const;
};

}  // namespace galaxian

#endif
