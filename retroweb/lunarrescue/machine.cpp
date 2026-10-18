#include "machine.h"

#include <algorithm>

namespace lunarrescue {

Machine::Machine() {
    // port 6 goes nowhere on lrescue (MAME lrescue_io_map)
    watchdog_enabled = false;
    reset();
}

void Machine::reset() {
    Board::reset();
    // IN0 tied high, IN1 COIN active-low with bits 3 and 7 high (MAME sicv_base)
    inputs = {0xFF, 0x89, 0x00};
    sound.reset();
    audio.clear();
    audio_acc_ = 0;
}

void Machine::load_roms(const RomSet& set) {
    program = set.program;
    program_hi.fill(0);
    std::copy(set.program_hi.begin(), set.program_hi.end(), program_hi.begin());
    prom = set.prom;
}

void Machine::sound_out(uint8_t port, uint8_t v) {
    if (port == 3) {
        sound.port3_w(v);
        // SX2 also forces every foreground pixel red (MAME m_screen_red)
        beam_tag = (v >> 2) & 1;
    } else {
        sound.port5_w(v);
    }
}

void Machine::elapsed(int t) {
    sound.advance(t);
    if (audio_hz <= 0) return;
    audio_acc_ += t;
    const double step = double(kCpuHz) / double(audio_hz);
    while (audio_acc_ >= step) {
        audio_acc_ -= step;
        audio.push_back(sound.mix());
    }
}

// PROM address (V >> 3) * 32 + byte column, V = line + 0x20 (MAME screen_update_invadpt2).
uint8_t Machine::pen(int ux, int uy) const {
    if (!lit(ux, uy)) return 0;
    if (tag(ux)) return 1;
    return prom[size_t(((ux >> 3) + 4) * 32 + column(uy))] & 7;
}

void Machine::render(uint32_t* upright) const {
    for (int uy = 0; uy < kUprightH; uy++) {
        for (int ux = 0; ux < kUprightW; ux++) {
            const uint8_t p = pen(ux, uy);
            // RBG_3BIT: bit 0 red, bit 1 blue, bit 2 green
            const uint32_t rgb = (p & 1 ? 0xFF0000u : 0) | (p & 4 ? 0x00FF00u : 0) | (p & 2 ? 0x0000FFu : 0);
            upright[size_t(uy) * kUprightW + size_t(ux)] = rgb;
        }
    }
}

}  // namespace lunarrescue
