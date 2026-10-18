// Taito Lunar Rescue (1979): the shared 8080 B&W board, colour PROM and the Taito PV sound board.

#ifndef LUNARRESCUE_MACHINE_H
#define LUNARRESCUE_MACHINE_H

#include "mw8080/board.h"
#include "mw8080/sound.h"

#include <array>
#include <cstdint>
#include <vector>

namespace lunarrescue {

using mw8080::kCpuHz;
using mw8080::kFrameCycles;
using mw8080::kLineCycles;
using mw8080::kUprightH;
using mw8080::kUprightW;

struct RomSet {
    std::array<uint8_t, 0x2000> program{};     // lrescue.1-4 at $0000-$1FFF
    std::array<uint8_t, 0x1000> program_hi{};  // lrescue.5-6 at $4000-$4FFF
    std::array<uint8_t, 0x400> prom{};         // 7643-1.cpu colour map
};

class Machine : public mw8080::Board {
public:
    mw8080::Sound sound{mw8080::SoundBoard::TaitoPv};
    std::array<uint8_t, 0x400> prom{};
    std::vector<float> audio;
    int audio_hz = 48000;

    Machine();
    void reset() override;
    void load_roms(const RomSet& set);
    bool screen_red() const { return beam_tag != 0; }
    // RBG_3BIT pen for upright pixel (ux, uy), 0 for black.
    uint8_t pen(int ux, int uy) const;
    // Upright colour frame, 0x00RRGGBB.
    void render(uint32_t* upright) const;

protected:
    void sound_out(uint8_t port, uint8_t v) override;
    void elapsed(int t) override;

private:
    double audio_acc_ = 0;
};

}  // namespace lunarrescue

#endif
