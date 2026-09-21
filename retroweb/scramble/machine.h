// Konami Scramble (1981) board: dual Z80 + Galaxian video + two AY-3-8910s.
//
// Memory map is the schematic-derived The End map (Konami parent of this
// PCB family). MAME galaxian.cpp (machine scramble / theend_map) is a
// cross-check of decoded chip-selects, not a behavior source.
// Clocks: main 18.432/6 = 3.072 MHz; sound Z80 and both AYs 14.31818/8 =
// 1.789772 MHz.

#ifndef SCRAMBLE_MACHINE_H
#define SCRAMBLE_MACHINE_H

#include "cpu_z80.h"
#include "galaxian/ay8910.h"
#include "galaxian/i8255.h"
#include "galaxian/timer.h"
#include "galaxian/video.h"

#include <array>
#include <cstdint>
#include <vector>

namespace scramble {

constexpr int kCpuHz = 3072000;
constexpr int kSoundHz = 1789772;
constexpr int kWatchdogFrames = 8;
using galaxian::kCpuPerFrame;
using galaxian::kUprightW;
using galaxian::kUprightH;

struct Inputs {
    // Active-low cabinet bits (1 = released). DIP bits sit in IN1/IN2.
    // Protection PAL bits 5/7 of IN2 are overlaid on PPI0 C, not stored here.
    uint8_t in0 = 0xFF;  // bomb, fire, L/R, coins
    uint8_t in1 = 0xFC;  // starts + lives DIP default 3 (bits 1:0 = 00)
    uint8_t in2 = 0x51;  // U/D + coinage/cabinet DIP default 1C/1C upright
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
    galaxian::Video video;
    galaxian::Ay8910 ay1;
    galaxian::Ay8910 ay2;
    galaxian::I8255 ppi0;
    galaxian::I8255 ppi1;
    std::array<uint8_t, 0x4000> program{};
    std::array<uint8_t, 0x1800> sound_rom{};
    std::array<uint8_t, 0x800> ram{};          // $4000–$47FF
    std::array<uint8_t, 0x400> sound_ram{};    // $8000–$83FF
    bool nmi_enable = false;
    int watchdog_ = kWatchdogFrames;
    int frames = 0;
    std::vector<float> audio;
    int audio_hz = 48000;
    bool watchdog_reset = false;

    uint8_t sound_latch = 0;
    uint8_t sound_control = 0;
    uint8_t protection_result = 0;

    Machine();
    void reset();
    void load_roms(const RomSet& set);
    int run_cycles(int n);
    void render(uint32_t* upright_224x256) const { video.render(upright_224x256); }

    uint8_t mem_read(uint16_t addr);
    void mem_write(uint16_t addr, uint8_t v);
    uint8_t sound_read(uint16_t addr);
    void sound_write(uint16_t addr, uint8_t v);
    uint8_t sound_in(uint8_t port);
    void sound_out(uint8_t port, uint8_t v);

    void pal6j_write(uint8_t v);

private:
    z80::Bus make_main_bus();
    z80::Bus make_sound_bus();
    void wire_ppi();
    void on_sound_control(uint8_t v);
    uint8_t in2_with_protection() const;
    int sound_credit_ = 0;
    double audio_acc_ = 0;
    bool sound_irq_ = false;
    uint32_t protection_state_ = 0;
};

}  // namespace scramble

#endif
