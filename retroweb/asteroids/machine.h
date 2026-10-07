// Atari Asteroids (1979) board: MOS 6502 @ 1.5 MHz + DVG + discrete sound.
//
// Memory map and I/O: computerarcheology.com/Arcade/Asteroids/Hardware.html,
// 6502disassembly.com/va-asteroids, Atari TM-143 / DP-143 schematics.
// A15 is ignored, so vectors at $FFFA mirror into ROM at $7FFA.
// Clocks from the 12.096 MHz master: CPU /8, 3 kHz = /4096, NMI every 12
// 3 kHz edges (MAME asteroid.cpp).

#ifndef ASTEROIDS_MACHINE_H
#define ASTEROIDS_MACHINE_H

#include "cpu_mos6502.h"
#include "dvg.h"

#include <array>
#include <cstdint>
#include <vector>

namespace asteroids {

constexpr int kCpuHz = 1512000;              // 12.096 MHz / 8
constexpr int kCyclesPer3kHz = 512;          // CPU cycles per 3 kHz edge
constexpr int kCyclesPerNmi = kCyclesPer3kHz * 12;  // ≈ 246.1 Hz
constexpr int kWatchdogCycles = kCyclesPer3kHz * 256;  // ≈ 87 ms
constexpr int kFbW = 1024;
constexpr int kFbH = 1024;

struct Inputs {
    // Active-high switch bits (1 = pressed).
    uint8_t in0 = 0x00;  // $2000
    uint8_t in1 = 0x00;  // $2400 coins, start, thrust, rotate
    // DSW1: four 2-bit fields at $2800..$2803 (coinage, right mult,
    // center/lives, language). $2802: 0=1x&4 lives, 1=1x&3, 2=2x&4, 3=2x&3.
    uint8_t dsw1 = 0x12;
};

struct RomSet {
    std::array<uint8_t, 0x1800> program{};   // $6800–$7FFF (6K)
    std::array<uint8_t, 0x0800> vector{};    // $5000–$57FF (2K)
};

class Machine {
public:
    Inputs inputs;
    mos6502::Cpu cpu;
    atari::Dvg dvg;
    // $0000–$01FF always; $0200–$02FF is player bank 0 (RAMSEL=0 view).
    std::array<uint8_t, 0x0400> ram{};
    std::array<uint8_t, 0x0800> vector_ram{}; // $4000–$47FF
    std::array<uint8_t, 0x1800> program{};
    std::array<uint8_t, 0x0800> vector_rom{};

    int frames = 0;
    bool watchdog_reset = false;
    std::vector<float> audio;
    int audio_hz = 48000;

    uint8_t explosion = 0;
    uint8_t thump = 0;
    bool saucer = false;
    bool saucer_fire = false;
    // LS259 Q2, SAUCERSEL. 1 = large saucer (ScrStatus 2, three RORs into D7).
    bool saucer_sel = false;
    bool thrust = false;
    bool ship_fire = false;
    bool bonus = false;

    uint8_t lamps = 0;   // LMPSCNS at $3200 (lamps + RAMSEL + coin counters)
    bool ramsel = false; // OUT latch bit 2 — swaps $0200/$0300 banks

    Machine();
    void reset();
    void load_roms(const RomSet& set);
    int run_cycles(int n);
    void render(uint32_t* rgba_1024x1024) const;

    uint8_t mem_read(uint16_t addr) const;
    void mem_write(uint16_t addr, uint8_t v);

private:
    mos6502::Bus make_bus();
    void kick_watchdog();
    void service_nmi();
    void tick_watchdog(int cycles);
    void dvg_go();
    void mix_audio(int cycles);
    uint8_t read_banked(uint16_t addr) const;
    void write_banked(uint16_t addr, uint8_t v);

    int cycles_to_nmi_ = 0;
    int watchdog_cycles_ = kWatchdogCycles;
    uint64_t clk3k_phase_ = 0;
    std::array<uint8_t, 0x100> bank1_{};  // alternate $0200 page

    uint32_t noise_lfsr_ = 1;
    float thrust_phase_ = 0;
    float fire_freq_ = 820.0f;
    float fire_phase_ = 0;
    float saucer_fire_freq_ = 830.0f;
    float saucer_fire_phase_ = 0;
    int saucer_fire_remain_ = 0;
    float saucer_phase_ = 0;
    float saucer_warble_ = 0;
    float thump_phase_ = 0;
    float thump_lp_ = 0;
    float thrust_lp_ = 0;
    float thrust_bp_ = 0;
    float explode_hold_ = 0;
    float explode_lp_ = 0;
    int explode_pitch_div_ = 12;
    int explode_vol_ = 0;
    int explode_clk_ = 0;
    int fire_remain_ = 0;
    float bonus_phase_ = 0;
    bool thump_en_ = false;
};

}  // namespace asteroids

#endif
