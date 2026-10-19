// Midway/Taito 8080 B&W board: 8080, 1-bit bitmap, beam-timed RST 1/RST 2, MB14241.

#ifndef SHARED_MW8080_BOARD_H
#define SHARED_MW8080_BOARD_H

#include "cpu_i8080.h"

#include <array>
#include <cstdint>

namespace mw8080 {

constexpr int kCpuHz = 1996800;  // 19.968 MHz / 10
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
    uint8_t in0 = 0;
    uint8_t in1 = 0;
    uint8_t in2 = 0;
};

// Fujitsu MB14241 barrel shifter.
struct Mb14241 {
    uint16_t data = 0;
    uint8_t count = 0;
    void count_w(uint8_t v) { count = uint8_t(~v & 7); }
    void data_w(uint8_t v) { data = uint16_t((data >> 8) | (uint16_t(v) << 7)); }
    uint8_t result() const { return uint8_t(data >> count); }
};

class Board {
public:
    Inputs inputs;
    i8080::Cpu cpu;
    Mb14241 shifter;
    std::array<uint8_t, 0x2000> program{};    // $0000–$1FFF
    std::array<uint8_t, 0x2000> program_hi{}; // $4000–$5FFF, an empty socket reads 0
    std::array<uint8_t, 0x2000> ram{};        // $2000–$3FFF, mirrored at $6000
    bool watchdog_enabled = true;
    // Cabinet state latched at each line start (Taito's red-screen flip-flop).
    uint8_t beam_tag = 0;
    int frames = 0;
    bool watchdog_reset = false;

    Board();
    virtual ~Board() = default;
    virtual void reset();
    int run_cycles(int n);

    uint8_t mem_read(uint16_t addr) const;
    void mem_write(uint16_t addr, uint8_t v);
    uint8_t port_in(uint8_t port) const;
    void port_out(uint8_t port, uint8_t v);

    int line() const { return int(frame_cycle_ / kLineCycles); }
    bool int_pending() const { return int_ff_; }
    // Vertical sync chain count for a beam line: 0x20–0xFF active, 0xDA–0xFF VBLANK.
    static uint8_t vcounter(int line);
    static uint8_t rst_vector(int line);

    // The frame as the beam last drew it, upright (ROT270), and the tag each line was drawn with.
    bool lit(int ux, int uy) const;
    uint8_t tag(int ux) const { return shown_tags_[size_t(ux)]; }
    // Byte column feeding upright row uy, -1 for the four flushed rows.
    static int column(int uy) { return uy < 256 ? 31 - (uy >> 3) : -1; }

protected:
    // OUT 3 and OUT 5 go to the cabinet's sound board.
    virtual void sound_out(uint8_t port, uint8_t v) { (void)port; (void)v; }
    virtual void elapsed(int t) { (void)t; }

private:
    i8080::Bus make_bus();
    void catch_up();
    void advance(int t);
    void line_start(int line);
    void watchdog_fire();

    std::array<uint8_t, kNativeH * 32> beam_{};
    std::array<uint8_t, kNativeH * 32> shown_{};
    std::array<uint8_t, kNativeH> beam_tags_{};
    std::array<uint8_t, kNativeH> shown_tags_{};
    uint64_t frame_origin_ = 0;
    uint32_t frame_cycle_ = 0;
    int fetched_ = 0;
    bool int_ff_ = false;
    int watchdog_ = kWatchdogFrames;
};

}  // namespace mw8080

#endif
