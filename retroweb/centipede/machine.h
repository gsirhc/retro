// Atari Centipede (1981) board: MOS 6502 @ 1.512 MHz + playfield/sprites + POKEY.
//
// Memory map: computerarcheology.com/Arcade/Centipede/Hardware.html,
// 6502disassembly.com/va-centipede, Atari DP-182 schematics.
// Clocks / IRQ / watchdog / palette: MAME centiped.cpp / centiped_v.cpp.

#ifndef CENTIPEDE_MACHINE_H
#define CENTIPEDE_MACHINE_H

#include "cpu_mos6502.h"
#include "pokey.h"

#include <array>
#include <cstdint>
#include <vector>

namespace centipede {

constexpr int kCpuHz = 1512000;          // 12.096 MHz / 8
constexpr int kFrameHz = 60;
constexpr int kLines = 256;              // 32×8 including blanked rows
constexpr int kVisRows = 30;             // visible tile rows
constexpr int kFbW = 256;                // machine (pre-rotate) width
constexpr int kFbH = 240;                // visible height
constexpr int kUprightW = 240;           // ROT270 portrait
constexpr int kUprightH = 256;
constexpr int kCyclesPerLine = kCpuHz / (kFrameHz * kLines);  // 98.4375 → 98
constexpr int kCyclesPerFrame = kCyclesPerLine * kLines;
constexpr int kWatchdogFrames = 8;       // MAME set_vblank_count(8)

struct Inputs {
    // IN0 bits 4–6 (cabinet / service / vblank filled by the board).
    // Trackball X/Y: 4-bit magnitude + sign bit 7, updated each frame.
    uint8_t track_x = 0;
    uint8_t track_y = 0;
    uint8_t in0_switches = 0x20;  // bit5 service active-low idle, bit4 upright=0
    // IN1 active-low (idle 0xFF): start1/2, fire, tilt, coins, service credit.
    uint8_t in1 = 0xFF;
    uint8_t in3 = 0xFF;  // cocktail P2 stick, unused upright
    uint8_t dsw1 = 0x54; // lives/bonus/difficulty/language (MAME default-ish)
    uint8_t dsw2 = 0x02; // coinage
};

struct RomSet {
    std::array<uint8_t, 0x2000> program{};  // $2000–$3FFF
    std::array<uint8_t, 0x1000> gfx{};      // 2×0x800 bitplanes
};

class Machine {
public:
    Inputs inputs;
    mos6502::Cpu cpu;
    atari::Pokey pokey;

    std::array<uint8_t, 0x0400> ram{};         // $0000–$03FF
    std::array<uint8_t, 0x03C0> videoram{};    // $0400–$07BF
    std::array<uint8_t, 0x0040> spriteram{};   // $07C0–$07FF
    std::array<uint8_t, 0x0010> paletteram{};  // $1400–$140F
    std::array<uint8_t, 0x0040> earom{};       // ER2055, 64×8
    std::array<uint8_t, 0x2000> program{};
    std::array<uint8_t, 0x1000> gfx{};

    int frames = 0;
    bool watchdog_reset = false;
    std::vector<float> audio;
    int audio_hz = 48000;
    bool flipscreen = false;
    uint8_t outlatch = 0;

    Machine();
    void reset();
    void load_roms(const RomSet& set);
    int run_cycles(int n);
    // Portrait RGBA (ROT270), kUprightW × kUprightH.
    void render(uint32_t* rgba) const;

    uint8_t mem_read(uint16_t addr) const;
    void mem_write(uint16_t addr, uint8_t v);

    // EAROM image for high-score persist (arcade.md).
    const std::array<uint8_t, 0x40>& earom_bytes() const { return earom; }
    void set_earom_bytes(const uint8_t* p, size_t n);

private:
    mos6502::Bus make_bus();
    void service_video(int cycles);
    void kick_watchdog();
    void earom_control(uint8_t v);
    void earom_update();
    uint32_t pen_rgb(int pen) const;
    uint8_t tile_pixel(uint8_t code, int x, int y) const;
    uint8_t sprite_pixel(uint8_t code, int x, int y) const;

    int line_ = 0;
    int line_cycles_ = 0;
    int watchdog_frames_ = kWatchdogFrames;
    bool vblank_ = false;
    bool irq_line_ = false;
    mutable bool playfield_touch_ = false;

    // ER2055. C1=/DB1, C2=DB2, CS1=DB3; /CS2 is tied active.
    uint8_t earom_data_ = 0;
    uint8_t earom_addr_ = 0;
    bool earom_cs1_ = false;
    bool earom_c1_ = true;
    bool earom_c2_ = false;
    bool earom_clk_ = false;
};

}  // namespace centipede

#endif
