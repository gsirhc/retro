// Namco 3-voice wavetable sound generator.
//
// Sample clock is CPU clock / 32 = 96 kHz (3.072 MHz / 32). Each voice has a
// 20-bit frequency, 4-bit volume, and 3-bit waveform select into a 256×4
// PROM (8 waves × 32 samples). Pac-Man writes the 32 nibbles at $5040;
// Galaga writes the same map at $6800. Register map cross-checked against
// MAME namco.cpp; resistor mixing is a linear 4-bit scale.

#ifndef SHARED_NAMCO_WSG_H
#define SHARED_NAMCO_WSG_H

#include <array>
#include <cstdint>
#include <vector>

namespace namco {

class Wsg {
public:
    std::array<uint8_t, 32> regs{};
    std::array<uint8_t, 256> wave_prom{};
    bool enabled = false;

    void reset();
    void write(int offset, uint8_t nibble);
    // Advance `cpu_cycles` at 3.072 MHz; append mono samples at `host_hz`.
    void advance(int cpu_cycles, int host_hz, std::vector<float>& out);

    float mix_at(uint64_t sample_clock) const;

private:
    uint64_t cpu_cycle_ = 0;
    double sample_hold_ = 0;
    double host_acc_ = 0;
    uint32_t voice_freq(int v) const;
    uint8_t voice_wave(int v) const;
    uint8_t voice_vol(int v) const;
};

}  // namespace namco

#endif
