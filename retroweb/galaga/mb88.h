// Fujitsu MB8843 / MB8844 (Namco 51XX / 54XX), Fujitsu MB8840/MB8840H series datasheet, Jan 1987.

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
    uint8_t o_output = 0xFF;
    bool in_irq = false;
    bool halted_reset = true;

    void reset();
    // true holds /IRQ low; the low-going edge requests the external interrupt.
    void set_irq(bool level);
    // TC pin level; a falling edge clocks the counter.
    void set_tc(bool level);
    // One instruction. Returns instruction cycles (input clock / 6), interrupt entry included.
    int step();

    int pc_full() const { return (int(pa) << 6) + pc; }

private:
    struct Frame {
        uint16_t pc;
        uint8_t cf, zf, st;
    };

    std::array<uint8_t, kDataNibbles> ram_{};
    std::array<Frame, 4> stack_{};
    int sp_ = 0;
    std::array<uint8_t, 4> r_latch_{};
    uint8_t p_latch_ = 0x0F;
    bool irq_pin_ = false;
    bool tc_pin_ = true;
    bool req_ext_ = false;
    bool req_timer_ = false;
    bool req_serial_ = false;
    int prescale_ = 0;
    int shifts_ = 0;

    uint8_t fetch();
    uint8_t& mem() { return ram_[((x & 7) * 16 + y) % kDataNibbles]; }
    void push(uint16_t ret);
    void count_timer();
    void clock(int cycles);
    int take_interrupt();
    uint8_t r_in(int port) const;
    void r_out(int port, uint8_t v);
    void execute(uint8_t op, int& cycles);
};

}  // namespace galaga

#endif
