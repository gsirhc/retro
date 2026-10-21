// Atari Missile Command (1980), board A035467. DP-147 2nd printing;
// rev 3 program listing for RAM use and switch decode.

#ifndef MISSILE_MACHINE_H
#define MISSILE_MACHINE_H

#include "cpu_mos6502.h"
#include "pokey.h"

#include <array>
#include <cstdint>
#include <vector>

namespace missile {

constexpr int kMasterHz = 10000000;
constexpr int kCpuHz = kMasterHz / 8;          // 1.25 MHz nominal
constexpr int kPokeyHz = kMasterHz / 8;
constexpr int kHTotal = 320;                    // 5 MHz pixel clock
constexpr int kLines = 256;
constexpr int kCyclesPerLine = kHTotal / 4;     // 80
constexpr int kSlowV = 224;                     // 3INH low: last 32 lines, half clock
constexpr int kCyclesPerFrame = kSlowV * kCyclesPerLine + (kLines - kSlowV) * kCyclesPerLine / 2;
constexpr double kFrameHz = double(kMasterHz / 2) / (kHTotal * kLines);  // 61.035
constexpr int kAvgCpuHz = 1171875;              // 19200 cycles × kFrameHz
constexpr int kVisTop = 25;                     // VBLANK* blanks V < 24 and V = 24
constexpr int kFbW = 256;
constexpr int kFbH = kLines - kVisTop;          // 231
constexpr int kWatchdogFrames = 8;
constexpr double kAmpCouplingHz = 10.0;
constexpr double kAudioScale = 4.0;             // page level fit
constexpr uint8_t kR8Default = 0x73;            // 6 cities, 10000, mini (TM-147, rev 3)
constexpr uint8_t kR10Default = 0x81;
constexpr uint16_t kHiscoreAddr = 0x002C;       // hscore_initials + hscore_scores
constexpr int kHiscoreLen = 48;

struct Inputs {
    // IN0 ($4800, CTRLD=0) active-low: R/C/L coin, 1P, 2P start, P2 L/C/R fire.
    uint8_t in0 = 0xFF;
    // IN1 ($4900): bits 0–2 R/C/L fire, 5 slam, 6 self-test, all active-low.
    uint8_t in1 = 0x67;
    // 4-bit trackball counters, read as VVVVHHHH when CTRLD=1.
    uint8_t track_x = 0;
    uint8_t track_y = 0;
    uint8_t r8 = kR8Default;    // POKEY ALLPOT
    uint8_t r10 = kR10Default;  // $4A00
};

struct RomSet {
    std::array<uint8_t, 0x3000> program{};  // $5000–$7FFF
    std::array<uint8_t, 0x20> prom{};       // 035826-01 L6
    bool has_prom = false;
};

class Machine {
public:
    Inputs inputs;
    mos6502::Cpu cpu;
    atari::Pokey pokey;

    // One 16K DRAM array: work RAM, 3rd-bit plane and 2bpp plane share it.
    std::array<uint8_t, 0x4000> vram{};
    std::array<uint8_t, 0x3000> program{};
    std::array<uint8_t, 0x20> prom{};
    std::array<uint8_t, 8> palette{};

    int frames = 0;
    bool watchdog_reset = false;
    std::vector<float> audio;
    int audio_hz = 48000;
    bool flipscreen = false;
    bool ctrld = false;
    uint8_t outlatch = 0;

    Machine();
    // Power cycle: clears DRAM and every latch.
    void reset();
    void load_roms(const RomSet& set);
    int run_cycles(int n);
    // RGBA, kFbW × kFbH.
    void render(uint32_t* rgba) const;

    uint8_t mem_read(uint16_t addr) const;
    void mem_write(uint16_t addr, uint8_t v);
    uint8_t mad_read(uint16_t ba) const;
    void mad_write(uint16_t ba, uint8_t d);

    int line() const { return line_; }
    int v() const { return v_of(line_); }
    bool vblank() const { return v_of(line_) < 24; }
    bool irq_pin() const { return irq_pin_; }
    // Last instruction's MADSEL access count, for tests.
    int madsel_hits = 0;

private:
    mos6502::Bus make_bus();
    void soft_reset();
    void set_default_prom();
    void dram_write_cycle(uint16_t ba, bool mushroom, uint8_t d);
    void kick_watchdog() { watchdog_frames_ = kWatchdogFrames; }
    void next_line();
    void clock_irq();
    int v_of(int scanline) const { return flipscreen ? ((256 - scanline) & 0xFF) : scanline; }
    int line_cycles(int scanline) const { return v_of(scanline) >= kSlowV ? kCyclesPerLine / 2 : kCyclesPerLine; }
    uint8_t bus_read(uint16_t addr);
    void bus_write(uint16_t addr, uint8_t v);

    int line_ = 0;
    int line_used_ = 0;
    int watchdog_frames_ = kWatchdogFrames;
    bool irq_state_ = false;  // F7 D input
    bool irq_pin_ = false;    // F7 Q, /IRQ
    bool irqh_ = false;       // E7, /IRQ one SYNC late
    bool madsel_armed_ = false;
    int access_ = 0;
    int extra_cycles_ = 0;
    double hp_ = 5.0;
};

}  // namespace missile

#endif
