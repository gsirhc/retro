// Zilog Z80 (NMOS), T-state exact (UM0080; Young, "The Undocumented Z80 Documented").

#ifndef SHARED_CPU_Z80_H
#define SHARED_CPU_Z80_H

#include <cstdint>
#include <functional>

namespace z80 {

enum Flag : uint8_t {
    FLAG_C  = 1 << 0,
    FLAG_N  = 1 << 1,
    FLAG_PV = 1 << 2,
    FLAG_X  = 1 << 3,
    FLAG_H  = 1 << 4,
    FLAG_Y  = 1 << 5,
    FLAG_Z  = 1 << 6,
    FLAG_S  = 1 << 7,
};

enum class Cycle : uint8_t { Fetch, Read, Write, In, Out, IntAck };

// Every callback runs at the T-state its bus cycle strobes; `Cpu::cycles` is
// current when it does.
struct Bus {
    std::function<uint8_t(uint16_t addr)>         read;
    std::function<void(uint16_t addr, uint8_t v)> write;
    std::function<uint8_t(uint16_t port)>         in;
    std::function<void(uint16_t port, uint8_t v)> out;
    // Byte on the data bus during INTA, and each further IM 0 instruction byte.
    std::function<uint8_t()> irq_data;
    // Optional. /WAIT T-states for a bus cycle, sampled at T2 (TW for I/O).
    std::function<int(uint16_t addr, Cycle kind)> wait;
    // Optional. T-states as they elapse, so a board can interleave mid-instruction.
    std::function<void(int t)> tick;
    // Optional. I*256+R on the address bus during M1 T3 (/RFSH).
    std::function<void(uint16_t addr)> refresh;
};

class Cpu {
public:
    uint8_t a = 0, f = 0, b = 0, c = 0, d = 0, e = 0, h = 0, l = 0;
    uint8_t a_ = 0, f_ = 0, b_ = 0, c_ = 0, d_ = 0, e_ = 0, h_ = 0, l_ = 0;
    uint16_t ix = 0, iy = 0, sp = 0, pc = 0;
    uint8_t i = 0, r = 0;
    uint8_t im = 0;
    bool iff1 = false, iff2 = false;
    bool halted = false;
    uint16_t wz = 0;          // MEMPTR
    uint8_t q = 0;            // F if the last instruction wrote flags, else 0
    bool after_ei = false;    // INT is not sampled after EI
    bool after_ld_air = false;
    bool int_line = false;
    uint64_t cycles = 0;

    explicit Cpu(Bus bus) : bus_(std::move(bus)) {}

    void reset();
    // One instruction, one halted M1, or one interrupt acknowledge. Returns T-states.
    int step();
    void set_int(bool asserted) { int_line = asserted; }
    // /NMI is edge-triggered.
    void set_nmi(bool asserted);
    bool nmi_pending() const { return nmi_pending_; }
    // Accept INT now if enabled. Returns T-states consumed (0 if ignored).
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
    bool nmi_line_ = false;
    bool nmi_pending_ = false;
    bool flags_written_ = false;
    bool from_bus_ = false;   // IM 0: instruction bytes come from the interrupting device

    void tick(int t);
    int waits(uint16_t addr, Cycle kind);
    void bump_r();
    uint8_t m1(uint16_t addr);
    uint8_t mr(uint16_t addr);
    void mw(uint16_t addr, uint8_t v);
    uint8_t ior(uint16_t port);
    void iow(uint16_t port, uint8_t v);
    uint8_t fetch_op();
    uint8_t fetch_arg();
    uint16_t fetch_arg16();
    void push16(uint16_t v);
    uint16_t pop16();

    void setf(uint8_t v) {
        f = v;
        flags_written_ = true;
    }
    void set_szxy_p(uint8_t v);
    static bool parity_even(uint8_t v);

    uint8_t add8(uint8_t x, uint8_t y, bool cin);
    uint8_t sub8(uint8_t x, uint8_t y, bool bin);
    void add16(uint16_t& dest, uint16_t v);
    void adc16(uint16_t v);
    void sbc16(uint16_t v);
    void alu(int op, uint8_t v);
    uint8_t inc8(uint8_t v);
    uint8_t dec8(uint8_t v);
    void daa();
    uint8_t shift(int op, uint8_t v);
    void bit(int n, uint8_t v, uint8_t xy_src);

    void execute(uint8_t op);
    void exec_main(uint8_t op, int xy);
    void exec_cb();
    void exec_xycb(int xy);
    void exec_ed(uint8_t op);
    uint16_t& xy_reg(int xy) { return xy == 0xDD ? ix : iy; }
    uint16_t index_addr(int xy);
    uint8_t get_r(int z, int xy);
    void set_r(int z, int xy, uint8_t v);
    bool cond(int y) const;

    void block_ld(bool inc, bool repeat);
    void block_cp(bool inc, bool repeat);
    void block_in(bool inc, bool repeat);
    void block_out(bool inc, bool repeat);
    void block_io_flags(uint8_t v, unsigned t);
    void block_io_repeat_flags(uint8_t v);

    void accept_int();
    void accept_nmi();
};

}  // namespace z80

#endif
