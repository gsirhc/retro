#include "machine.h"

namespace invaders {

Machine::Machine() {
    Machine::reset();
}

void Machine::reset() {
    Board::reset();
    // Bit 3 set, coin bit set. Credit is the falling edge (invaders.h $0020).
    inputs = {0x08, 0x09, 0x00};
    sound.reset();
    audio.clear();
    audio_acc_ = 0;
}

void Machine::sound_out(uint8_t port, uint8_t v) {
    if (port == 3) sound.port3_w(v);
    else sound.port5_w(v);
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

void Machine::render(uint32_t* upright) const {
    for (int uy = 0; uy < kUprightH; uy++) {
        for (int ux = 0; ux < kUprightW; ux++)
            upright[size_t(uy) * kUprightW + size_t(ux)] = lit(ux, uy) ? 0xFFFFFFu : 0u;
    }
}

}  // namespace invaders
