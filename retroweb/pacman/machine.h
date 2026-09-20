// Midway Pac-Man (1980) board: Z80 + tilemap/sprites + Namco WSG.
//
// Memory map from the Midway Pac-Man service manual / schematics. MAME
// pacman.cpp is a cross-check of decoded chip-selects, not a behavior source.
// Clock: 18.432 MHz master, CPU 18.432/6 = 3.072 MHz.

#ifndef PACMAN_MACHINE_H
#define PACMAN_MACHINE_H

#include "cpu_z80.h"
#include "video.h"
#include "wsg.h"

#include <array>
#include <cstdint>
#include <vector>

namespace pacman {

constexpr int kCpuHz = 3072000;
constexpr int kWatchdogFrames = 8;

struct Inputs {
    // Active-low bits as the cabinet presents them (1 = released).
    uint8_t in0 = 0xFF;
    uint8_t in1 = 0xFF;
    uint8_t dsw1 = 0xC9;  // Midway factory-ish: 3 lives, bonus 10k
    uint8_t dsw2 = 0xFF;
};

struct RomSet {
    std::array<uint8_t, 0x4000> program{};
    std::array<uint8_t, 4096> tiles{};
    std::array<uint8_t, 4096> sprites{};
    std::array<uint8_t, 32> color_prom{};
    std::array<uint8_t, 256> lookup_prom{};
    std::array<uint8_t, 256> wave_prom{};
};

class Machine {
public:
    Inputs inputs;
    z80::Cpu cpu;
    Video video;
    Wsg wsg;
    std::array<uint8_t, 0x4000> program{};
    std::array<uint8_t, 0x800> ram{};  // $4800–$4FFF
    bool irq_enable = false;
    // Latched by `OUT (0),A`: the real board wires the Z80's IM 2 vector
    // byte to a discrete output latch, not a fixed value. The ROM reprograms
    // it (0xFA during the self-test's per-vblank checksum passes, 0xFC once
    // the main game's vblank ISR at $008D takes over) -- see the "interrupt
    // vector" writes at $233F/$3183 in the Midway pacman disassembly.
    uint8_t irq_vector = 0xFF;
    int watchdog_ = kWatchdogFrames;
    int frames = 0;
    std::vector<float> audio;
    int audio_hz = 48000;
    // Sticky: set when the 8-vblank watchdog elapses. reset() does not
    // clear it — a host that wants a clean board assigns false itself.
    bool watchdog_reset = false;

    Machine();
    void reset();
    void load_roms(const RomSet& set);
    int run_cycles(int n);
    void render(uint32_t* upright_224x288) const { video.render(upright_224x288); }

    uint8_t mem_read(uint16_t addr);
    void mem_write(uint16_t addr, uint8_t v);

private:
    z80::Bus make_bus();
};

}  // namespace pacman

#endif
