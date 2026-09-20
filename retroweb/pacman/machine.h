// Midway Pac-Man (1980) board: Z80 + tilemap/sprites + Namco WSG.
// Optional GCC Ms. Pac-Man aux board in the Z80 socket (U5/U6/U7 + PAL
// overlay / dump-protection latch, US 4,525,599).
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
    // DSW1 at $5080. Midway factory: 1C/1C, 3 lives, bonus 10k, normal
    // difficulty, normal ghost names (0xC9). Bit map from the service
    // manual, cross-checked against MAME INPUT_PORTS_START(pacman):
    //   1:0 coinage  01=1C/1C  11=2C/1C  10=1C/2C  00=free
    //   3:2 lives    00=1  01=2  10=3  11=5
    //   5:4 bonus    00=10k  01=15k  10=20k  11=none
    //   6   difficulty  1=normal  0=hard   (solder pad on some boards)
    //   7   ghost names 1=normal  0=alternate (solder pad on some boards)
    // Rack test is IN0 bit 4; cabinet upright/cocktail is IN1 bit 7.
    uint8_t dsw1 = 0xC9;
    uint8_t dsw2 = 0xFF;  // unused on the Midway pacman set
};

struct RomSet {
    std::array<uint8_t, 0x4000> program{};
    std::array<uint8_t, 4096> tiles{};
    std::array<uint8_t, 4096> sprites{};
    std::array<uint8_t, 32> color_prom{};
    std::array<uint8_t, 256> lookup_prom{};
    std::array<uint8_t, 256> wave_prom{};
    // GCC/Midway Ms. Pac-Man aux board (U5 2716, U6/U7 2532). Empty and
    // aux_board=false means the stock Pac-Man PCB, Z80 in socket 6B.
    std::array<uint8_t, 0x0800> aux_u5{};
    std::array<uint8_t, 0x1000> aux_u6{};
    std::array<uint8_t, 0x1000> aux_u7{};
    bool aux_board = false;
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

    // GCC aux board in the Z80 socket. aux_decode is the dump-protection
    // latch (US 4,525,599): access $3FF8–$3FFF sets it, several 8-byte
    // trap windows clear it. Z80 RESET does not clear the PAL.
    bool aux_board = false;
    bool aux_decode = false;

    Machine();
    void reset();
    void load_roms(const RomSet& set);
    int run_cycles(int n);
    void render(uint32_t* upright_224x288) const { video.render(upright_224x288); }

    uint8_t mem_read(uint16_t addr);
    void mem_write(uint16_t addr, uint8_t v);

private:
    z80::Bus make_bus();
    void aux_trap(uint16_t addr);
    void rebuild_aux();
    std::array<uint8_t, 0x10000> aux_decrypted_{};
    std::array<uint8_t, 0x0800> aux_u5_{};
    std::array<uint8_t, 0x1000> aux_u6_{};
    std::array<uint8_t, 0x1000> aux_u7_{};
};

}  // namespace pacman

#endif
