// Zilog Z80 CPU core.
//
// Host-agnostic: memory and I/O go through Bus callbacks. Instruction timing
// follows the Zilog Z80 CPU User's Manual (UM0080) T-state tables. Flag
// polarity and ALU rules are Z80, not 8080 (P/V is overflow on add/sub,
// parity on logic; N distinguishes add vs subtract for DAA).

#ifndef PACMAN_CPU_Z80_H
#define PACMAN_CPU_Z80_H

#include <cstdint>
#include <functional>

namespace z80 {

enum Flag : uint8_t {
    FLAG_C  = 1 << 0,
    FLAG_N  = 1 << 1,
    FLAG_PV = 1 << 2,
    FLAG_X  = 1 << 3,  // undocumented copy of bit 3
    FLAG_H  = 1 << 4,
    FLAG_Y  = 1 << 5,  // undocumented copy of bit 5
    FLAG_Z  = 1 << 6,
    FLAG_S  = 1 << 7,
};

struct Bus {
    std::function<uint8_t(uint16_t addr)>         read;
    std::function<void(uint16_t addr, uint8_t v)> write;
    std::function<uint8_t(uint8_t port)>          in;
    std::function<void(uint8_t port, uint8_t v)>  out;
    // Byte jammed on the bus during IM 0 / IM 2 acknowledge. Unused in IM 1.
    std::function<uint8_t()> irq_data;
};

class Cpu {
public:
    uint8_t a = 0, f = 0, b = 0, c = 0, d = 0, e = 0, h = 0, l = 0;
    uint8_t a_ = 0, f_ = 0, b_ = 0, c_ = 0, d_ = 0, e_ = 0, h_ = 0, l_ = 0;
    uint16_t ix = 0, iy = 0, sp = 0, pc = 0;
    uint8_t i = 0, r = 0;
    uint8_t im = 0;          // 0, 1, or 2
    bool iff1 = false, iff2 = false;
    bool halted = false;
    uint64_t cycles = 0;

    explicit Cpu(Bus bus) : bus_(std::move(bus)) {}

    void reset();
    int step();
    // Maskable INT. Returns T-states consumed (0 if ignored).
    int interrupt();
    int nmi();

    uint16_t bc() const { return (uint16_t(b) << 8) | c; }
    uint16_t de() const { return (uint16_t(d) << 8) | e; }
    uint16_t hl() const { return (uint16_t(h) << 8) | l; }
    uint16_t af() const { return (uint16_t(a) << 8) | f; }
    void set_bc(uint16_t v) { b = uint8_t(v >> 8); c = uint8_t(v); }
    void set_de(uint16_t v) { d = uint8_t(v >> 8); e = uint8_t(v); }
    void set_hl(uint16_t v) { h = uint8_t(v >> 8); l = uint8_t(v); }
    void set_af(uint16_t v) { a = uint8_t(v >> 8); f = uint8_t(v); }

    bool flag(Flag fl) const { return (f & fl) != 0; }

private:
    Bus bus_;
    int ei_delay_ = 0;   // >0: INT accepted only after the next instruction (EI)

    uint8_t fetch8();
    uint16_t fetch16();
    uint8_t read8(uint16_t addr);
    void write8(uint16_t addr, uint8_t v);
    uint16_t read16(uint16_t addr);
    void write16(uint16_t addr, uint16_t v);
    void push16(uint16_t v);
    uint16_t pop16();

    void set_flag(Flag fl, bool on) {
        if (on) f = uint8_t(f | fl);
        else    f = uint8_t(f & ~fl);
    }
    void set_szxy(uint8_t v);
    void set_szxy_p(uint8_t v);
    static bool parity_even(uint8_t v);

    uint8_t add8(uint8_t x, uint8_t y, bool cin);
    uint8_t sub8(uint8_t x, uint8_t y, bool bin);
    void add16(uint16_t& dest, uint16_t v);
    void adc16(uint16_t& dest, uint16_t v);
    void sbc16(uint16_t& dest, uint16_t v);
    void logic_and(uint8_t v);
    void logic_or(uint8_t v);
    void logic_xor(uint8_t v);
    void cp8(uint8_t v);
    uint8_t inc8(uint8_t v);
    uint8_t dec8(uint8_t v);
    void daa();
    uint8_t rot_left(uint8_t v, bool with_carry, bool circular);
    uint8_t rot_right(uint8_t v, bool with_carry, bool circular);

    int exec_main(uint8_t op, int xy);          // xy: 0 none, 0xDD IX, 0xFD IY
    int exec_cb(uint8_t op, int xy, int8_t d);
    int exec_ed(uint8_t op);
    uint16_t& xy_reg(int xy);
    uint8_t read_xy_d(int xy, int8_t d);
    void write_xy_d(int xy, int8_t d, uint8_t v);
    uint8_t get_r(int z, int xy);
    void set_r(int z, int xy, uint8_t v);
    uint8_t get_r_cb(int z, int xy, int8_t d, bool* wrote_hl);
    void set_r_cb(int z, int xy, int8_t d, uint8_t v);

    int block_ldi(bool repeat, bool inc);
    int block_cpi(bool repeat, bool inc);
};

}  // namespace z80

#endif
