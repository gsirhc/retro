#include "wsg.h"

namespace pacman {

void Wsg::reset() {
    regs.fill(0);
    enabled = false;
    cpu_cycle_ = 0;
    sample_hold_ = 0;
    host_acc_ = 0;
}

void Wsg::write(int offset, uint8_t nibble) {
    regs[unsigned(offset) & 31] = uint8_t(nibble & 0x0F);
}

// Real register map (MAME src/devices/sound/namco.cpp,
// namco_wsg_device::pacman_sound_w's "pacman register map" comment, cross-
// checked against the Midway pacman disassembly's waveform writes to
// $5045/$504a/$504f and its 16-byte $4e8c->$5050 LDIR):
//   0x05/0x0a/0x0f:   ch0/1/2 waveform select
//   0x10:             ch0's low frequency nibble only -- voices 1 and 2 have
//                     no wire to this bit position, so their bottom nibble
//                     is hardwired 0 (coarser pitch resolution, genuine
//                     hardware quirk, not an emulation shortcut).
//   0x11-0x14/0x15:   ch0 frequency (4 more nibbles) / ch0 volume
//   0x16-0x19/0x1a:   ch1 frequency / ch1 volume
//   0x1b-0x1e/0x1f:   ch2 frequency / ch2 volume
uint32_t Wsg::voice_freq(int v) const {
    uint32_t f = (v == 0) ? uint32_t(regs[0x10]) : 0;
    f += uint32_t(regs[unsigned(v * 5 + 0x11)]) << 4;
    f += uint32_t(regs[unsigned(v * 5 + 0x12)]) << 8;
    f += uint32_t(regs[unsigned(v * 5 + 0x13)]) << 12;
    f += uint32_t(regs[unsigned(v * 5 + 0x14)]) << 16;
    return f;
}

uint8_t Wsg::voice_wave(int v) const { return uint8_t(regs[unsigned(v * 5 + 0x05)] & 7); }

uint8_t Wsg::voice_vol(int v) const { return regs[unsigned(v * 5 + 0x15)]; }

float Wsg::mix_at(uint64_t sample_clock) const {
    if (!enabled) return 0;
    float acc = 0;
    for (int v = 0; v < 3; v++) {
        uint32_t f = voice_freq(v);
        uint8_t vol = voice_vol(v);
        if (f == 0 || vol == 0) continue;
        // 20-bit phase accumulator at 96 kHz, waveform 32 samples.
        uint32_t phase = uint32_t((sample_clock * uint64_t(f)) >> (20 - 5));
        uint8_t idx = uint8_t((voice_wave(v) << 5) | (phase & 31));
        // Real WSG PROM samples are unsigned 4-bit values biased by 8 (MAME's
        // waveform_r: `(nibble & 0xf) - 8`), not a symmetric -7.5..7.5 range.
        int s = int(wave_prom[idx] & 0x0F) - 8;
        acc += float(s) * float(vol);
    }
    // Normalize the resistor-summed 3-voice mix (each voice contributes at
    // most |sample|*vol = 8*15) to a unit float range.
    return acc / (8.0f * 15.0f * 3.0f);
}

void Wsg::advance(int cpu_cycles, int host_hz, std::vector<float>& out) {
    // 96 kHz WSG clock = cpu/32. Resample to host_hz with a zero-order hold.
    const double wsg_hz = 3072000.0 / 32.0;
    for (int i = 0; i < cpu_cycles; i++) {
        cpu_cycle_++;
        sample_hold_ += wsg_hz / 3072000.0;
        while (sample_hold_ >= 1.0) {
            sample_hold_ -= 1.0;
            uint64_t sc = cpu_cycle_ / 32;
            // Emit host samples proportionally.
            host_acc_ += double(host_hz) / wsg_hz;
            float s = mix_at(sc);
            while (host_acc_ >= 1.0) {
                host_acc_ -= 1.0;
                out.push_back(s);
            }
        }
    }
}

}  // namespace pacman
