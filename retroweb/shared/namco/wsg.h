// Namco 3-voice wavetable sound generator. Sample clock is CPU/32 = 96 kHz
// (3.072 MHz / 32). Each voice: 20-bit frequency, 4-bit volume, 3-bit waveform
// select into a 256x4 PROM (8 waves x 32 samples). Pac-Man loads it at $5040,
// Galaga at $6800. Register map per MAME namco.cpp.

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

private:
    uint64_t cpu_cycle_ = 0;
    double sample_hold_ = 0;
    double host_acc_ = 0;
    // 20-bit phase accumulators, as in MAME namco_wsg_device
    uint32_t counter_[3] = {};
    uint32_t voice_freq(int v) const;
    uint8_t voice_wave(int v) const;
    uint8_t voice_vol(int v) const;
    float mix_counters() const;
};

}  // namespace namco

#endif
