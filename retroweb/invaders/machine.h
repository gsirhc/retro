// Midway Space Invaders (1978), PCB #739: the shared 8080 B&W board with Midway's sound board.

#ifndef INVADERS_MACHINE_H
#define INVADERS_MACHINE_H

#include "mw8080/board.h"
#include "mw8080/sound.h"

#include <array>
#include <cstdint>
#include <vector>

namespace invaders {

using mw8080::kCpuHz;
using mw8080::kFrameCycles;
using mw8080::kLineCycles;
using mw8080::kUprightH;
using mw8080::kUprightW;

struct RomSet {
    std::array<uint8_t, 0x2000> program{};  // H, G, F, E at $0000–$1FFF
};

class Machine : public mw8080::Board {
public:
    mw8080::Sound sound{mw8080::SoundBoard::Midway};
    std::vector<float> audio;
    int audio_hz = 48000;

    Machine();
    void reset() override;
    void load_roms(const RomSet& set) { program = set.program; }
    // Upright B&W frame, 0x00RRGGBB.
    void render(uint32_t* upright) const;

protected:
    void sound_out(uint8_t port, uint8_t v) override;
    void elapsed(int t) override;

private:
    double audio_acc_ = 0;
};

}  // namespace invaders

#endif
