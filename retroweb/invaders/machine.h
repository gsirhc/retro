// Midway Space Invaders (1978) board, PCB #739: 8080 + 1-bit bitmap + MB14241 shifter (MAME mw8080bw.cpp).

#ifndef INVADERS_MACHINE_H
#define INVADERS_MACHINE_H

#include "cpu_i8080.h"
#include "sound.h"

#include <array>
#include <cstdint>
#include <vector>

namespace invaders {

// 256 shifted pixels plus the 4 the shift register flushes after HBLANK (MW8080BW_HPIXCOUNT).
constexpr int kNativeW = 260;
constexpr int kNativeH = 224;
constexpr int kUprightW = kNativeH;
constexpr int kUprightH = kNativeW;
// 320 pixel clocks at 19.968 MHz / 4 is 128 CPU T-states.
constexpr int kLineCycles = 128;
constexpr int kLines = 262;
constexpr int kFrameCycles = kLineCycles * kLines;
constexpr int kVblankLine = 224;
constexpr int kMidLine = 96;
constexpr int kWatchdogFrames = 255;

constexpr uint16_t kRamBase = 0x2000;
constexpr uint16_t kVramBase = 0x2400;

struct Inputs {
    uint8_t in0 = 0x08;  // bit 3 unused, pulled high
    uint8_t in1 = 0x09;  // COIN active-low, bit 3 pulled high
    uint8_t in2 = 0x00;  // 3 lives, bonus 1500, coinage shown
};

struct RomSet {
    std::array<uint8_t, 0x2000> program{};  // H, G, F, E at $0000–$1FFF
};

// Fujitsu MB14241 barrel shifter (MAME mb14241.cpp).
struct Mb14241 {
    uint16_t data = 0;
    uint8_t count = 0;
    void count_w(uint8_t v) { count = uint8_t(~v & 7); }
    void data_w(uint8_t v) { data = uint16_t((data >> 8) | (uint16_t(v) << 7)); }
    uint8_t result() const { return uint8_t(data >> count); }
};

class Machine {
public:
    Inputs inputs;
    i8080::Cpu cpu;
    Sound sound;
    Mb14241 shifter;
    std::array<uint8_t, 0x2000> program{};
    std::array<uint8_t, 0x2000> ram{};  // $2000–$3FFF, mirrored at $6000
    int frames = 0;
    bool watchdog_reset = false;
    std::vector<float> audio;
    int audio_hz = 48000;

    Machine();
    void reset();
    void load_roms(const RomSet& set);
    int run_cycles(int n);
    // Upright (ROT270) frame as the beam last drew it, 0x00RRGGBB.
    void render(uint32_t* upright) const;

    uint8_t mem_read(uint16_t addr) const;
    void mem_write(uint16_t addr, uint8_t v);
    uint8_t port_in(uint8_t port) const;
    void port_out(uint8_t port, uint8_t v);

    int line() const { return int(frame_cycle_ / kLineCycles); }
    bool int_pending() const { return int_ff_; }
    // Vertical sync chain count for a beam line: 0x20–0xFF active, 0xDA–0xFF VBLANK.
    static uint8_t vcounter(int line);
    static uint8_t rst_vector(int line);

private:
    i8080::Bus make_bus();
    void catch_up();
    void elapse(int t);
    void line_start(int line);
    void watchdog_fire();

    std::array<uint8_t, kNativeH * 32> beam_{};
    std::array<uint8_t, kNativeH * 32> shown_{};
    uint64_t frame_origin_ = 0;
    uint32_t frame_cycle_ = 0;
    int fetched_ = 0;
    bool int_ff_ = false;
    int watchdog_ = kWatchdogFrames;
    double audio_acc_ = 0;
};

}  // namespace invaders

#endif
