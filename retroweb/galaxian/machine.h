// Namco Galaxian (1979) board: single Z80 + Galaxian video + discrete sound.
//
// Memory map is the schematic-derived Namco parent map. MAME galaxian.cpp
// (galaxian_map_base + galaxian_map_discrete) is a cross-check of decoded
// chip-selects, not a behavior source.
// Clocks: Z80 18.432/6 = 3.072 MHz. Discrete analog (555/LFSR) is a
// labelled simplification — see sound.h.

#ifndef GALAXIAN_MACHINE_H
#define GALAXIAN_MACHINE_H

#include "cpu_z80.h"
#include "galaxian/video.h"
#include "sound.h"

#include <array>
#include <cstdint>
#include <vector>

namespace galaxian {

constexpr int kWatchdogFrames = 8;

struct Inputs {
    // Active-high cabinet bits (1 = pressed). DIP bits sit in IN1/IN2.
    uint8_t in0 = 0x00;  // coins, L/R, fire, cabinet
    uint8_t in1 = 0x00;  // starts + coinage DIP default 1C/1C
    uint8_t in2 = 0x04;  // bonus + lives DIP default 3 lives
};

struct RomSet {
    std::array<uint8_t, 0x4000> program{};
    std::array<uint8_t, 0x1000> gfx{};
    std::array<uint8_t, 32> color_prom{};
};

class Machine {
public:
    Inputs inputs;
    z80::Cpu cpu;
    Video video;
    DiscreteSound sound;
    std::array<uint8_t, 0x4000> program{};
    std::array<uint8_t, 0x400> ram{};   // $4000–$43FF, mirrored $0400
    bool nmi_enable = false;
    int watchdog_ = kWatchdogFrames;
    int frames = 0;
    std::vector<float> audio;
    int audio_hz = 48000;
    bool watchdog_reset = false;

    Machine();
    void reset();
    void load_roms(const RomSet& set);
    int run_cycles(int n);
    void render(uint32_t* upright_224x256) const { video.render(upright_224x256); }

    uint8_t mem_read(uint16_t addr);
    void mem_write(uint16_t addr, uint8_t v);

private:
    z80::Bus make_bus();
    double audio_acc_ = 0;
};

}  // namespace galaxian

#endif
