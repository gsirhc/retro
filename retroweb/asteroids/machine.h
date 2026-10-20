// Atari Asteroids (1979) board (DP-143 schematics and memory map, ASTDEC.XX).

#ifndef ASTEROIDS_MACHINE_H
#define ASTEROIDS_MACHINE_H

#include "cpu_mos6502.h"
#include "dvg.h"
#include "sound.h"

#include <array>
#include <cstdint>
#include <vector>

namespace asteroids {

constexpr int kCpuHz = 1512000;              // 12.096 MHz / 8
constexpr int kCyclesPer3kHz = 512;          // CPU cycles per 3 kHz edge
constexpr int kCyclesPerNmi = kCyclesPer3kHz * 12;  // C5 LS161 divides 3 kHz by 12
// D5 LS393 reaches 128 at the 3 kHz rate and sets RESET (DP-143 sheet 1B).
constexpr int kWatchdogCycles = kCyclesPer3kHz * 128;
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

    Sound sound;

    uint8_t lamps = 0;   // LMPSCNS at $3200 (lamps + RAMSEL + coin counters)
    bool ramsel = false; // OUT latch bit 2, swaps $0200/$0300 banks

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

    int audio_hz_set_ = 0;
    int64_t audio_acc_ = 0;
};

}  // namespace asteroids

#endif
