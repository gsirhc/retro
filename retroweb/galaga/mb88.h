// Fujitsu MB8843 / MB8844 (Namco 51XX / 54XX).
// Timing and flags follow MB8840 as cross-checked against MAME mb88xx.cpp.

#ifndef GALAGA_MB88_H
#define GALAGA_MB88_H

#include <array>
#include <cstdint>
#include <functional>

namespace galaga {

class Mb88 {
public:
    static constexpr int kRomBytes = 0x400;
    static constexpr int kDataNibbles = 64;

    std::array<uint8_t, kRomBytes> rom{};
    std::function<uint8_t()> read_k;
    std::function<uint8_t(int n)> read_r;
    std::function<void(int n, uint8_t v)> write_r;
    std::function<void(uint8_t v, uint8_t mask)> write_o;
    std::function<void(uint8_t v)> write_p;
    // SI pin; unconnected reads 0.
    std::function<bool()> read_si;

    uint8_t a = 0, x = 0, y = 0;
    uint8_t pc = 0, pa = 0, si = 0;
    uint8_t st = 1, zf = 0, cf = 0, vf = 0, sf = 0, pio = 0;
    uint8_t th = 0, tl = 0, sb = 0;
    uint8_t o_output = 0;
    bool in_irq = false;
    bool halted_reset = true;

    void reset();
    void set_irq(bool level);
    void set_tc(bool level);
    // One instruction. Returns instruction cycles (input clock / 6).
    int step();

    int pc_full() const { return (int(pa) << 6) + pc; }

private:
    std::array<uint8_t, kDataNibbles> data_{};
    std::array<uint16_t, 4> sp_{};
    int tp_ = 0;
    uint8_t pending_ = 0;
    bool if_ = false;
    bool ctr_ = false;
    int sb_count_ = 0;

    uint8_t fetch();
    uint8_t read_data(int addr) const;
    void write_data(int addr, uint8_t v);
    void burn(int cycles);
    void out_o(uint8_t index);
    uint8_t in_r(int n) const;
    void out_r(int n, uint8_t v);
};

}  // namespace galaga

#endif
