// Atari Math Box: four Am2901 slices and sequencer (Atari MBUDOC.DOC, Am2901 data sheet).

#ifndef SHARED_ATARI_MATHBOX_H
#define SHARED_ATARI_MATHBOX_H

#include <array>
#include <cstdint>

namespace atari {

class Mathbox {
public:
    static constexpr int kTicksPerCpuCycle = 2;  // E3MHZ through B4 (DP-156 sheet 3B)

    std::array<uint8_t, 32> map{};
    // M1 holds bits 23-20, F1 bits 3-0 (MBUCOD.V05 $OUT macro).
    std::array<uint32_t, 256> ucode{};

    std::array<uint16_t, 16> r{};
    uint16_t q = 0;
    uint16_t y = 0;
    uint64_t steps = 0;

    void reset();
    void load_proms(const uint8_t* map32, const uint8_t* const nibbles[6]);

    // Writing any of the 32 addresses loads the uPC and restarts the clock.
    void write(uint8_t offset, uint8_t data);
    void run(int cpu_cycles);

    bool busy() const { return running_; }
    uint8_t status() const { return running_ ? 0x80 : 0x00; }
    uint8_t lo() const { return uint8_t(y); }
    uint8_t hi() const { return uint8_t(y >> 8); }

    // Runs one microinstruction; true when it carried STALL.
    bool step();

private:
    bool running_ = false;
    uint8_t upc_ = 0;
    uint8_t jump_ = 0;
    uint16_t d_ = 0;
    bool q0_ = true;
    int ticks_ = 0;
};

}  // namespace atari

#endif
