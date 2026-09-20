// Konami Frogger (1981) board: dual Z80 + Galaxian video + AY-3-8910.
//
// Memory map from the Konami/Sega service material and Computer Archaeology
// Frogger hardware notes. MAME galaxian.cpp (machine frogger) is a
// cross-check of decoded chip-selects, not a behavior source.
// Clocks: main 18.432/6 = 3.072 MHz; sound Z80 and AY 14.31818/8 = 1.789772 MHz.

#ifndef FROGGER_MACHINE_H
#define FROGGER_MACHINE_H

#include "ay8910.h"
#include "cpu_z80.h"
#include "i8255.h"
#include "video.h"

#include <array>
#include <cstdint>
#include <vector>

namespace frogger {

constexpr int kCpuHz = 3072000;
constexpr int kSoundHz = 1789772;
constexpr int kWatchdogFrames = 8;

struct Inputs {
    // Active-low cabinet bits (1 = released). DIP bits sit in IN1/IN2.
    uint8_t in0 = 0xFF;  // L/R, service, coins
    uint8_t in1 = 0xFC;  // starts + lives DIP default 3 (bits 1:0 = 00)
    uint8_t in2 = 0xF1;  // U/D + coinage/cabinet DIP default 1C/1C upright
};

struct RomSet {
    std::array<uint8_t, 0x4000> program{};
    std::array<uint8_t, 0x1800> sound{};
    std::array<uint8_t, 0x1000> gfx{};
    std::array<uint8_t, 32> color_prom{};
};

class Machine {
public:
    Inputs inputs;
    z80::Cpu main;
    z80::Cpu sound;
    Video video;
    Ay8910 ay;
    I8255 ppi0;
    I8255 ppi1;
    std::array<uint8_t, 0x4000> program{};
    std::array<uint8_t, 0x1800> sound_rom{};
    std::array<uint8_t, 0x800> ram{};          // $8000–$87FF
    std::array<uint8_t, 0x400> sound_ram{};    // $4000–$43FF
    bool nmi_enable = false;
    int watchdog_ = kWatchdogFrames;
    int frames = 0;
    std::vector<float> audio;
    int audio_hz = 48000;
    bool watchdog_reset = false;

    uint8_t sound_latch = 0;
    uint8_t sound_control = 0;

    Machine();
    void reset();
    void load_roms(const RomSet& set);
    int run_cycles(int n);
    void render(uint32_t* upright_224x256) const { video.render(upright_224x256); }

    uint8_t mem_read(uint16_t addr);
    void mem_write(uint16_t addr, uint8_t v);
    uint8_t sound_read(uint16_t addr);
    void sound_write(uint16_t addr, uint8_t v);
    // Sound-CPU I/O: bit 6 = AY data, bit 7 = AY address (Konami decode).
    uint8_t sound_in(uint8_t port);
    void sound_out(uint8_t port, uint8_t v);

private:
    z80::Bus make_main_bus();
    z80::Bus make_sound_bus();
    void wire_ppi();
    void on_sound_control(uint8_t v);
    int sound_credit_ = 0;
    bool sound_irq_ = false;
};

// PCB wiring: first 2K of sound ROM (608) and gfx ROM 606 (low plane at $800)
// have D0↔D1 swapped. 607 is the high plane and is wired straight.
uint8_t swap_d0d1(uint8_t v);

// Konami sound-board timer on AY port B. `sound_cpu_cycles` is T-states
// of the 1.789772 MHz sound Z80. Frogger swaps bits 3 and 5 of the
// generic Konami reading. Computer Archaeology / Konami sound board;
// MAME konami_sound_timer_r / frogger_sound_timer_r is a cross-check.
uint8_t sound_timer_port(uint64_t sound_cpu_cycles);

}  // namespace frogger

#endif
