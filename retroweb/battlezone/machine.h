// Atari Battlezone (1980) board (MAME bzone.cpp, Atari BZONE.MAC).

#ifndef BATTLEZONE_MACHINE_H
#define BATTLEZONE_MACHINE_H

#include "avg.h"
#include "cpu_mos6502.h"
#include "mathbox.h"
#include "pokey.h"
#include "sound.h"

#include <array>
#include <cstdint>
#include <vector>

namespace battlezone {

constexpr int kCpuHz = 1512000;
constexpr int kCyclesPer3kHz = 512;
constexpr int kCyclesPerNmi = kCyclesPer3kHz * 12;  // gated off by the self-test switch
constexpr int kWatchdogCycles = kCyclesPer3kHz * 256;
constexpr int kFbW = atari::Avg::kWidth;
constexpr int kFbH = atari::Avg::kHeight;
// Cabinet overlay: red over the top fifth (radar, score), green below (MAME bzone.lay).
constexpr float kOverlaySplit = 0.2f;

struct Inputs {
    // $0800 D0-D5, active low: right, middle, left coin (COIN65.MAC), slam,
    // self-test, diag step.
    uint8_t in0 = 0x3F;
    // ALLPOT, active high: R down, R up, L down, L up, fire, start.
    uint8_t in3 = 0x00;
    uint8_t dsw0 = 0x15;  // 3 tanks, missile at 10000, bonus 15k & 100k, English
    uint8_t dsw1 = 0x03;  // 2 coins / 1 credit, both mechs x1, no bonus coins
};

struct RomSet {
    std::array<uint8_t, 0x3000> program{};  // $5000-$7FFF
    std::array<uint8_t, 0x1000> vector{};   // $3000-$3FFF
    std::array<uint8_t, 32> mb_map{};       // 036174 B1
    // 036175 M1, 036176 L1, 036177 K1, 036178 J1, 036179 H1, 036180 F1
    std::array<std::array<uint8_t, 256>, 6> mb_ucode{};
};

class Machine {
public:
    Inputs inputs;
    mos6502::Cpu cpu;
    atari::Avg avg;
    atari::Mathbox mathbox;
    atari::Pokey pokey;
    Sound sound;

    std::array<uint8_t, 0x0400> ram{};
    std::array<uint8_t, 0x1000> vector_ram{};  // $2000-$2FFF
    std::array<uint8_t, 0x3000> program{};
    std::array<uint8_t, 0x1000> vector_rom{};

    int nmis = 0;
    bool watchdog_reset = false;
    uint8_t coin_counters = 0;
    std::vector<float> audio;
    int audio_hz = 48000;

    Machine();
    void reset();
    void load_roms(const RomSet& set);
    int run_cycles(int n);
    void render(uint32_t* rgba) const;

    bool start_lamp() const { return (sound.latch & 0x40) != 0; }
    uint8_t mem_read(uint16_t addr) const;
    void mem_write(uint16_t addr, uint8_t v);

private:
    mos6502::Bus make_bus();
    void tick_watchdog(int cycles);
    void mix_audio(int cycles);

    int cycles_to_nmi_ = kCyclesPerNmi;
    int watchdog_cycles_ = kWatchdogCycles;
    uint64_t clk3k_ = 0;
    int audio_hz_set_ = 0;
    std::vector<float> pokey_buf_;
    atari::Avg::MemRead avg_read_;
};

}  // namespace battlezone

#endif
