#include "cpu_z80.h"

namespace z80 {

void Cpu::reset() {
    // AF and SP read FFFFh after reset; the rest are undefined (Young).
    pc = 0;
    i = r = 0;
    im = 0;
    iff1 = iff2 = false;
    set_af(0xFFFF);
    sp = 0xFFFF;
    halted = false;
    q = 0;
    after_ei = after_ld_air = false;
    nmi_pending_ = false;
    from_bus_ = false;
    cycles = 0;
}

void Cpu::set_nmi(bool asserted) {
    if (asserted && !nmi_line_) nmi_pending_ = true;
    nmi_line_ = asserted;
}

// --- bus cycles ---

void Cpu::tick(int t) {
    if (t <= 0) return;
    cycles += uint64_t(t);
    if (bus_.tick) bus_.tick(t);
}

int Cpu::waits(uint16_t addr, Cycle kind) {
    if (!bus_.wait) return 0;
    int w = bus_.wait(addr, kind);
    return w > 0 ? w : 0;
}

void Cpu::bump_r() { r = uint8_t((r & 0x80) | ((r + 1) & 0x7F)); }

uint8_t Cpu::m1(uint16_t addr) {
    tick(1);
    tick(waits(addr, Cycle::Fetch));
    uint8_t v = bus_.read(addr);
    tick(1);
    if (bus_.refresh) bus_.refresh(uint16_t((i << 8) | r));
    bump_r();
    tick(2);
    return v;
}

uint8_t Cpu::mr(uint16_t addr) {
    tick(1);
    tick(waits(addr, Cycle::Read));
    uint8_t v = bus_.read(addr);
    tick(2);
    return v;
}

void Cpu::mw(uint16_t addr, uint8_t v) {
    tick(1);
    tick(waits(addr, Cycle::Write));
    bus_.write(addr, v);
    tick(2);
}

// I/O cycles carry one automatic wait state (UM0080 fig. 7).
uint8_t Cpu::ior(uint16_t port) {
    tick(2);
    tick(waits(port, Cycle::In));
    uint8_t v = bus_.in ? bus_.in(port) : uint8_t(0xFF);
    tick(2);
    return v;
}

void Cpu::iow(uint16_t port, uint8_t v) {
    tick(2);
    tick(waits(port, Cycle::Out));
    if (bus_.out) bus_.out(port, v);
    tick(2);
}

uint8_t Cpu::fetch_op() {
    if (from_bus_) {
        tick(1);
        tick(waits(pc, Cycle::IntAck));
        uint8_t v = bus_.irq_data ? bus_.irq_data() : uint8_t(0xFF);
        tick(1);
        if (bus_.refresh) bus_.refresh(uint16_t((i << 8) | r));
        bump_r();
        tick(2);
        return v;
    }
    uint8_t v = m1(pc);
    pc = uint16_t(pc + 1);
    return v;
}

uint8_t Cpu::fetch_arg() {
    if (from_bus_) {
        tick(1);
        tick(waits(pc, Cycle::Read));
        uint8_t v = bus_.irq_data ? bus_.irq_data() : uint8_t(0xFF);
        tick(2);
        return v;
    }
    uint8_t v = mr(pc);
    pc = uint16_t(pc + 1);
    return v;
}

uint16_t Cpu::fetch_arg16() {
    uint8_t lo = fetch_arg();
    uint8_t hi = fetch_arg();
    return uint16_t(lo | (hi << 8));
}

void Cpu::push16(uint16_t v) {
    sp = uint16_t(sp - 1);
    mw(sp, uint8_t(v >> 8));
    sp = uint16_t(sp - 1);
    mw(sp, uint8_t(v));
}

uint16_t Cpu::pop16() {
    uint8_t lo = mr(sp);
    sp = uint16_t(sp + 1);
    uint8_t hi = mr(sp);
    sp = uint16_t(sp + 1);
    return uint16_t(lo | (hi << 8));
}

// --- flags and ALU ---

bool Cpu::parity_even(uint8_t v) {
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (v & 1) == 0;
}

void Cpu::set_szxy_p(uint8_t v) {
    setf(uint8_t((f & (FLAG_C | FLAG_N | FLAG_H)) |
                 (v & (FLAG_S | FLAG_X | FLAG_Y)) |
                 (v == 0 ? FLAG_Z : 0) |
                 (parity_even(v) ? FLAG_PV : 0)));
}

uint8_t Cpu::add8(uint8_t x, uint8_t y, bool cin) {
    unsigned res = unsigned(x) + y + (cin ? 1u : 0u);
    uint8_t v = uint8_t(res);
    bool ov = ((x ^ v) & (y ^ v) & 0x80) != 0;
    setf(uint8_t((v & (FLAG_S | FLAG_X | FLAG_Y)) |
                 (v == 0 ? FLAG_Z : 0) |
                 ((x ^ y ^ v) & FLAG_H) |
                 (ov ? FLAG_PV : 0) |
                 (res > 0xFF ? FLAG_C : 0)));
    return v;
}

uint8_t Cpu::sub8(uint8_t x, uint8_t y, bool bin) {
    unsigned res = unsigned(x) - y - (bin ? 1u : 0u);
    uint8_t v = uint8_t(res);
    bool ov = ((x ^ y) & (x ^ v) & 0x80) != 0;
    setf(uint8_t((v & (FLAG_S | FLAG_X | FLAG_Y)) |
                 (v == 0 ? FLAG_Z : 0) |
                 ((x ^ y ^ v) & FLAG_H) |
                 (ov ? FLAG_PV : 0) |
                 FLAG_N |
                 (res > 0xFF ? FLAG_C : 0)));
    return v;
}

void Cpu::add16(uint16_t& dest, uint16_t v) {
    unsigned res = unsigned(dest) + v;
    setf(uint8_t((f & (FLAG_S | FLAG_Z | FLAG_PV)) |
                 ((res >> 8) & (FLAG_X | FLAG_Y)) |
                 (((dest ^ v ^ res) >> 8) & FLAG_H) |
                 (res > 0xFFFF ? FLAG_C : 0)));
    dest = uint16_t(res);
}

void Cpu::adc16(uint16_t v) {
    uint16_t x = hl();
    unsigned res = unsigned(x) + v + (flag(FLAG_C) ? 1u : 0u);
    uint16_t out = uint16_t(res);
    bool ov = ((x ^ out) & (v ^ out) & 0x8000) != 0;
    setf(uint8_t(((out >> 8) & (FLAG_S | FLAG_X | FLAG_Y)) |
                 (out == 0 ? FLAG_Z : 0) |
                 (((x ^ v ^ out) >> 8) & FLAG_H) |
                 (ov ? FLAG_PV : 0) |
                 (res > 0xFFFF ? FLAG_C : 0)));
    set_hl(out);
}

void Cpu::sbc16(uint16_t v) {
    uint16_t x = hl();
    unsigned res = unsigned(x) - v - (flag(FLAG_C) ? 1u : 0u);
    uint16_t out = uint16_t(res);
    bool ov = ((x ^ v) & (x ^ out) & 0x8000) != 0;
    setf(uint8_t(((out >> 8) & (FLAG_S | FLAG_X | FLAG_Y)) |
                 (out == 0 ? FLAG_Z : 0) |
                 (((x ^ v ^ out) >> 8) & FLAG_H) |
                 (ov ? FLAG_PV : 0) |
                 FLAG_N |
                 (res > 0xFFFF ? FLAG_C : 0)));
    set_hl(out);
}

void Cpu::alu(int op, uint8_t v) {
    switch (op) {
        case 0: a = add8(a, v, false); break;
        case 1: a = add8(a, v, flag(FLAG_C)); break;
        case 2: a = sub8(a, v, false); break;
        case 3: a = sub8(a, v, flag(FLAG_C)); break;
        case 4:
            a &= v;
            setf(FLAG_H);
            set_szxy_p(a);
            break;
        case 5:
            a ^= v;
            setf(0);
            set_szxy_p(a);
            break;
        case 6:
            a |= v;
            setf(0);
            set_szxy_p(a);
            break;
        default:
            (void)sub8(a, v, false);
            // CP takes X/Y from the operand, not the result.
            setf(uint8_t((f & ~(FLAG_X | FLAG_Y)) | (v & (FLAG_X | FLAG_Y))));
            break;
    }
}

uint8_t Cpu::inc8(uint8_t v) {
    uint8_t res = uint8_t(v + 1);
    setf(uint8_t((f & FLAG_C) |
                 (res & (FLAG_S | FLAG_X | FLAG_Y)) |
                 (res == 0 ? FLAG_Z : 0) |
                 ((v & 0x0F) == 0x0F ? FLAG_H : 0) |
                 (v == 0x7F ? FLAG_PV : 0)));
    return res;
}

uint8_t Cpu::dec8(uint8_t v) {
    uint8_t res = uint8_t(v - 1);
    setf(uint8_t((f & FLAG_C) |
                 (res & (FLAG_S | FLAG_X | FLAG_Y)) |
                 (res == 0 ? FLAG_Z : 0) |
                 ((v & 0x0F) == 0 ? FLAG_H : 0) |
                 (v == 0x80 ? FLAG_PV : 0) |
                 FLAG_N));
    return res;
}

void Cpu::daa() {
    uint8_t t = 0;
    bool carry = flag(FLAG_C);
    if (flag(FLAG_H) || (a & 0x0F) > 9) t |= 0x06;
    if (carry || a > 0x99) {
        t |= 0x60;
        carry = true;
    }
    bool n = flag(FLAG_N);
    bool half = n ? (flag(FLAG_H) && (a & 0x0F) < 6) : ((a & 0x0F) > 9);
    a = n ? uint8_t(a - t) : uint8_t(a + t);
    setf(uint8_t((n ? FLAG_N : 0) | (carry ? FLAG_C : 0) | (half ? FLAG_H : 0)));
    set_szxy_p(a);
}

uint8_t Cpu::shift(int op, uint8_t v) {
    uint8_t res;
    bool out;
    switch (op) {
        case 0: out = v & 0x80; res = uint8_t((v << 1) | (out ? 1 : 0)); break;            // RLC
        case 1: out = v & 0x01; res = uint8_t((v >> 1) | (out ? 0x80 : 0)); break;         // RRC
        case 2: out = v & 0x80; res = uint8_t((v << 1) | (flag(FLAG_C) ? 1 : 0)); break;   // RL
        case 3: out = v & 0x01; res = uint8_t((v >> 1) | (flag(FLAG_C) ? 0x80 : 0)); break;  // RR
        case 4: out = v & 0x80; res = uint8_t(v << 1); break;                              // SLA
        case 5: out = v & 0x01; res = uint8_t((v >> 1) | (v & 0x80)); break;               // SRA
        case 6: out = v & 0x80; res = uint8_t((v << 1) | 1); break;                        // SLL
        default: out = v & 0x01; res = uint8_t(v >> 1); break;                             // SRL
    }
    setf(out ? FLAG_C : 0);
    set_szxy_p(res);
    return res;
}

void Cpu::bit(int n, uint8_t v, uint8_t xy_src) {
    bool zero = (v & (1u << n)) == 0;
    setf(uint8_t((f & FLAG_C) | FLAG_H |
                 (zero ? (FLAG_Z | FLAG_PV) : 0) |
                 (n == 7 && !zero ? FLAG_S : 0) |
                 (xy_src & (FLAG_X | FLAG_Y))));
}

// --- decode ---

uint8_t Cpu::get_r(int z, int xy) {
    switch (z) {
        case 0: return b;
        case 1: return c;
        case 2: return d;
        case 3: return e;
        case 4: return xy ? uint8_t(xy_reg(xy) >> 8) : h;
        case 5: return xy ? uint8_t(xy_reg(xy)) : l;
        default: return a;
    }
}

void Cpu::set_r(int z, int xy, uint8_t v) {
    switch (z) {
        case 0: b = v; break;
        case 1: c = v; break;
        case 2: d = v; break;
        case 3: e = v; break;
        case 4:
            if (xy) xy_reg(xy) = uint16_t((v << 8) | (xy_reg(xy) & 0xFF));
            else h = v;
            break;
        case 5:
            if (xy) xy_reg(xy) = uint16_t((xy_reg(xy) & 0xFF00) | v);
            else l = v;
            break;
        default: a = v; break;
    }
}

bool Cpu::cond(int y) const {
    switch (y) {
        case 0: return !flag(FLAG_Z);
        case 1: return flag(FLAG_Z);
        case 2: return !flag(FLAG_C);
        case 3: return flag(FLAG_C);
        case 4: return !flag(FLAG_PV);
        case 5: return flag(FLAG_PV);
        case 6: return !flag(FLAG_S);
        default: return flag(FLAG_S);
    }
}

uint16_t Cpu::index_addr(int xy) {
    int8_t dsp = int8_t(fetch_arg());
    wz = uint16_t(xy_reg(xy) + dsp);
    return wz;
}

void Cpu::execute(uint8_t op) {
    int xy = 0;
    while (op == 0xDD || op == 0xFD) {
        xy = op;
        q = 0;
        op = fetch_op();
    }
    if (op == 0xCB) {
        if (xy) exec_xycb(xy);
        else exec_cb();
    } else if (op == 0xED) {
        exec_ed(fetch_op());
    } else {
        exec_main(op, xy);
    }
}

void Cpu::exec_main(uint8_t op, int xy) {
    const int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    const int p = y >> 1, qq = y & 1;

    auto rp = [&](int n) -> uint16_t {
        switch (n) {
            case 0: return bc();
            case 1: return de();
            case 2: return xy ? xy_reg(xy) : hl();
            default: return sp;
        }
    };
    auto set_rp = [&](int n, uint16_t v) {
        switch (n) {
            case 0: set_bc(v); break;
            case 1: set_de(v); break;
            case 2:
                if (xy) xy_reg(xy) = v;
                else set_hl(v);
                break;
            default: sp = v; break;
        }
    };
    // (HL), or (IX+d) with the five internal T-states that add the displacement.
    auto mem_operand = [&]() -> uint16_t {
        if (!xy) return hl();
        uint16_t ea = index_addr(xy);
        tick(5);
        return ea;
    };

    if (x == 0) {
        switch (z) {
            case 0:
                switch (y) {
                    case 0: return;
                    case 1: {
                        uint8_t ta = a, tf = f;
                        a = a_; f = f_;
                        a_ = ta; f_ = tf;
                        return;
                    }
                    case 2: {
                        tick(1);
                        int8_t dsp = int8_t(fetch_arg());
                        b = uint8_t(b - 1);
                        if (b) {
                            tick(5);
                            pc = uint16_t(pc + dsp);
                            wz = pc;
                        }
                        return;
                    }
                    case 3: {
                        int8_t dsp = int8_t(fetch_arg());
                        tick(5);
                        pc = uint16_t(pc + dsp);
                        wz = pc;
                        return;
                    }
                    default: {
                        int8_t dsp = int8_t(fetch_arg());
                        if (cond(y - 4)) {
                            tick(5);
                            pc = uint16_t(pc + dsp);
                            wz = pc;
                        }
                        return;
                    }
                }
            case 1:
                if (qq == 0) {
                    set_rp(p, fetch_arg16());
                } else {
                    uint16_t v = rp(2);
                    tick(7);
                    wz = uint16_t(v + 1);
                    add16(v, rp(p));
                    set_rp(2, v);
                }
                return;
            case 2: {
                if (p == 2) {
                    uint16_t nn = fetch_arg16();
                    if (qq == 0) {
                        uint16_t v = rp(2);
                        mw(nn, uint8_t(v));
                        mw(uint16_t(nn + 1), uint8_t(v >> 8));
                    } else {
                        uint8_t lo = mr(nn);
                        uint8_t hi = mr(uint16_t(nn + 1));
                        set_rp(2, uint16_t(lo | (hi << 8)));
                    }
                    wz = uint16_t(nn + 1);
                    return;
                }
                uint16_t ea = p == 0 ? bc() : p == 1 ? de() : fetch_arg16();
                if (qq == 0) {
                    mw(ea, a);
                    wz = uint16_t((a << 8) | ((ea + 1) & 0xFF));
                } else {
                    a = mr(ea);
                    wz = uint16_t(ea + 1);
                }
                return;
            }
            case 3:
                tick(2);
                set_rp(p, uint16_t(rp(p) + (qq ? -1 : 1)));
                return;
            case 4:
            case 5:
                if (y == 6) {
                    uint16_t ea = mem_operand();
                    uint8_t v = mr(ea);
                    tick(1);
                    mw(ea, z == 4 ? inc8(v) : dec8(v));
                } else {
                    uint8_t v = get_r(y, xy);
                    set_r(y, xy, z == 4 ? inc8(v) : dec8(v));
                }
                return;
            case 6:
                if (y == 6) {
                    if (xy) {
                        uint16_t ea = index_addr(xy);
                        uint8_t n = fetch_arg();
                        tick(2);
                        mw(ea, n);
                    } else {
                        mw(hl(), fetch_arg());
                    }
                } else {
                    set_r(y, xy, fetch_arg());
                }
                return;
            default: {
                const uint8_t keep = f & (FLAG_S | FLAG_Z | FLAG_PV);
                switch (y) {
                    case 0: {
                        bool cy = a & 0x80;
                        a = uint8_t((a << 1) | (cy ? 1 : 0));
                        setf(uint8_t(keep | (a & (FLAG_X | FLAG_Y)) | (cy ? FLAG_C : 0)));
                        return;
                    }
                    case 1: {
                        bool cy = a & 1;
                        a = uint8_t((a >> 1) | (cy ? 0x80 : 0));
                        setf(uint8_t(keep | (a & (FLAG_X | FLAG_Y)) | (cy ? FLAG_C : 0)));
                        return;
                    }
                    case 2: {
                        bool cy = a & 0x80;
                        a = uint8_t((a << 1) | (flag(FLAG_C) ? 1 : 0));
                        setf(uint8_t(keep | (a & (FLAG_X | FLAG_Y)) | (cy ? FLAG_C : 0)));
                        return;
                    }
                    case 3: {
                        bool cy = a & 1;
                        a = uint8_t((a >> 1) | (flag(FLAG_C) ? 0x80 : 0));
                        setf(uint8_t(keep | (a & (FLAG_X | FLAG_Y)) | (cy ? FLAG_C : 0)));
                        return;
                    }
                    case 4: daa(); return;
                    case 5:
                        a = uint8_t(~a);
                        setf(uint8_t((f & (FLAG_S | FLAG_Z | FLAG_PV | FLAG_C)) |
                                     FLAG_H | FLAG_N | (a & (FLAG_X | FLAG_Y))));
                        return;
                    // SCF/CCF X/Y = (Q ^ F) | A on NMOS Zilog parts (Patrik Rak, 2018).
                    case 6:
                        setf(uint8_t(keep | (((q ^ f) | a) & (FLAG_X | FLAG_Y)) | FLAG_C));
                        return;
                    default: {
                        bool cy = flag(FLAG_C);
                        setf(uint8_t(keep | (((q ^ f) | a) & (FLAG_X | FLAG_Y)) |
                                     (cy ? FLAG_H : FLAG_C)));
                        return;
                    }
                }
            }
        }
    }

    if (x == 1) {
        if (y == 6 && z == 6) {
            halted = true;
            return;
        }
        if (y == 6) {
            uint16_t ea = mem_operand();
            mw(ea, get_r(z, 0));
        } else if (z == 6) {
            uint16_t ea = mem_operand();
            set_r(y, 0, mr(ea));
        } else {
            set_r(y, xy, get_r(z, xy));
        }
        return;
    }

    if (x == 2) {
        alu(y, z == 6 ? mr(mem_operand()) : get_r(z, xy));
        return;
    }

    switch (z) {
        case 0:
            tick(1);
            if (cond(y)) {
                pc = pop16();
                wz = pc;
            }
            return;
        case 1:
            if (qq == 0) {
                uint16_t v = pop16();
                if (p == 3) set_af(v);
                else set_rp(p, v);
                return;
            }
            switch (p) {
                case 0:
                    pc = pop16();
                    wz = pc;
                    return;
                case 1: {
                    uint8_t t;
                    t = b; b = b_; b_ = t;
                    t = c; c = c_; c_ = t;
                    t = d; d = d_; d_ = t;
                    t = e; e = e_; e_ = t;
                    t = h; h = h_; h_ = t;
                    t = l; l = l_; l_ = t;
                    return;
                }
                case 2:
                    pc = rp(2);
                    return;
                default:
                    tick(2);
                    sp = rp(2);
                    return;
            }
        case 2: {
            uint16_t nn = fetch_arg16();
            wz = nn;
            if (cond(y)) pc = nn;
            return;
        }
        case 3:
            switch (y) {
                case 0:
                    pc = fetch_arg16();
                    wz = pc;
                    return;
                case 2: {
                    uint8_t n = fetch_arg();
                    iow(uint16_t((a << 8) | n), a);
                    wz = uint16_t((a << 8) | ((n + 1) & 0xFF));
                    return;
                }
                case 3: {
                    uint16_t port = uint16_t((a << 8) | fetch_arg());
                    a = ior(port);
                    wz = uint16_t(port + 1);
                    return;
                }
                case 4: {
                    uint8_t lo = mr(sp);
                    uint8_t hi = mr(uint16_t(sp + 1));
                    tick(1);
                    uint16_t v = rp(2);
                    mw(uint16_t(sp + 1), uint8_t(v >> 8));
                    mw(sp, uint8_t(v));
                    tick(2);
                    wz = uint16_t(lo | (hi << 8));
                    set_rp(2, wz);
                    return;
                }
                case 5: {
                    uint16_t t = de();
                    set_de(hl());
                    set_hl(t);
                    return;
                }
                case 6:
                    iff1 = iff2 = false;
                    return;
                case 7:
                    iff1 = iff2 = true;
                    after_ei = true;
                    return;
                default:
                    return;
            }
        case 4: {
            uint16_t nn = fetch_arg16();
            wz = nn;
            if (cond(y)) {
                tick(1);
                push16(pc);
                pc = nn;
            }
            return;
        }
        case 5:
            if (qq == 0) {
                tick(1);
                push16(p == 3 ? af() : rp(p));
                return;
            }
            if (p == 0) {
                uint16_t nn = fetch_arg16();
                wz = nn;
                tick(1);
                push16(pc);
                pc = nn;
            }
            return;
        case 6:
            alu(y, fetch_arg());
            return;
        default:
            tick(1);
            push16(pc);
            pc = uint16_t(y * 8);
            wz = pc;
            return;
    }
}

void Cpu::exec_cb() {
    const uint8_t op = fetch_op();
    const int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    if (z == 6) {
        const uint16_t ea = hl();
        uint8_t v = mr(ea);
        tick(1);
        // BIT n,(HL) takes X/Y from MEMPTR's high byte (Young).
        if (x == 1) {
            bit(y, v, uint8_t(wz >> 8));
            return;
        }
        if (x == 0) v = shift(y, v);
        else if (x == 2) v = uint8_t(v & ~(1u << y));
        else v = uint8_t(v | (1u << y));
        mw(ea, v);
        return;
    }
    uint8_t v = get_r(z, 0);
    if (x == 1) {
        bit(y, v, v);
        return;
    }
    if (x == 0) v = shift(y, v);
    else if (x == 2) v = uint8_t(v & ~(1u << y));
    else v = uint8_t(v | (1u << y));
    set_r(z, 0, v);
}

// DD CB d op: the opcode byte is a plain memory read, not an M1 (no R increment).
void Cpu::exec_xycb(int xy) {
    const uint16_t ea = index_addr(xy);
    const uint8_t op = fetch_arg();
    tick(2);
    const int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    uint8_t v = mr(ea);
    tick(1);
    if (x == 1) {
        bit(y, v, uint8_t(ea >> 8));
        return;
    }
    if (x == 0) v = shift(y, v);
    else if (x == 2) v = uint8_t(v & ~(1u << y));
    else v = uint8_t(v | (1u << y));
    mw(ea, v);
    if (z != 6) set_r(z, 0, v);
}

void Cpu::exec_ed(uint8_t op) {
    const int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    const int p = y >> 1, qq = y & 1;

    auto rp = [&](int n) -> uint16_t {
        switch (n) {
            case 0: return bc();
            case 1: return de();
            case 2: return hl();
            default: return sp;
        }
    };

    if (x == 2 && z <= 3 && y >= 4) {
        const bool inc = (y & 1) == 0;
        const bool repeat = y >= 6;
        switch (z) {
            case 0: block_ld(inc, repeat); break;
            case 1: block_cp(inc, repeat); break;
            case 2: block_in(inc, repeat); break;
            default: block_out(inc, repeat); break;
        }
        return;
    }
    if (x != 1) return;

    switch (z) {
        case 0: {
            uint8_t v = ior(bc());
            wz = uint16_t(bc() + 1);
            setf(f & FLAG_C);
            set_szxy_p(v);
            if (y != 6) set_r(y, 0, v);
            return;
        }
        case 1:
            // NMOS parts drive 0 for OUT (C),(HL) slot; CMOS drive FFh.
            iow(bc(), y == 6 ? uint8_t(0) : get_r(y, 0));
            wz = uint16_t(bc() + 1);
            return;
        case 2:
            tick(7);
            wz = uint16_t(hl() + 1);
            if (qq) adc16(rp(p));
            else sbc16(rp(p));
            return;
        case 3: {
            uint16_t nn = fetch_arg16();
            if (qq == 0) {
                uint16_t v = rp(p);
                mw(nn, uint8_t(v));
                mw(uint16_t(nn + 1), uint8_t(v >> 8));
            } else {
                uint8_t lo = mr(nn);
                uint8_t hi = mr(uint16_t(nn + 1));
                uint16_t v = uint16_t(lo | (hi << 8));
                switch (p) {
                    case 0: set_bc(v); break;
                    case 1: set_de(v); break;
                    case 2: set_hl(v); break;
                    default: sp = v; break;
                }
            }
            wz = uint16_t(nn + 1);
            return;
        }
        case 4:
            a = sub8(0, a, false);
            return;
        case 5:
            // RETN and RETI both copy IFF2 to IFF1 (Young).
            pc = pop16();
            wz = pc;
            iff1 = iff2;
            return;
        case 6: {
            static constexpr uint8_t kMode[8] = {0, 0, 1, 2, 0, 0, 1, 2};
            im = kMode[y];
            return;
        }
        default:
            switch (y) {
                case 0:
                    tick(1);
                    i = a;
                    return;
                case 1:
                    tick(1);
                    r = a;
                    return;
                case 2:
                case 3:
                    tick(1);
                    a = y == 2 ? i : r;
                    setf(uint8_t((f & FLAG_C) | (a & (FLAG_S | FLAG_X | FLAG_Y)) |
                                 (a == 0 ? FLAG_Z : 0) | (iff2 ? FLAG_PV : 0)));
                    after_ld_air = true;
                    return;
                case 4:
                case 5: {
                    uint8_t m = mr(hl());
                    tick(4);
                    uint8_t na;
                    if (y == 4) {
                        mw(hl(), uint8_t((m >> 4) | (a << 4)));
                        na = uint8_t((a & 0xF0) | (m & 0x0F));
                    } else {
                        mw(hl(), uint8_t((m << 4) | (a & 0x0F)));
                        na = uint8_t((a & 0xF0) | (m >> 4));
                    }
                    a = na;
                    wz = uint16_t(hl() + 1);
                    setf(f & FLAG_C);
                    set_szxy_p(a);
                    return;
                }
                default:
                    return;
            }
    }
}

// --- block instructions ---

void Cpu::block_ld(bool inc, bool repeat) {
    uint8_t v = mr(hl());
    mw(de(), v);
    tick(2);
    const int step = inc ? 1 : -1;
    set_hl(uint16_t(hl() + step));
    set_de(uint16_t(de() + step));
    set_bc(uint16_t(bc() - 1));
    uint8_t n = uint8_t(a + v);
    setf(uint8_t((f & (FLAG_S | FLAG_Z | FLAG_C)) |
                 ((n & 0x02) ? FLAG_Y : 0) |
                 (n & FLAG_X) |
                 (bc() ? FLAG_PV : 0)));
    if (repeat && bc() != 0) {
        tick(5);
        pc = uint16_t(pc - 2);
        wz = uint16_t(pc + 1);
        // Interrupted repeats show PC bits 13/11 in Y/X (David Banks, 2018).
        setf(uint8_t((f & ~(FLAG_X | FLAG_Y)) | ((pc >> 8) & (FLAG_X | FLAG_Y))));
    }
}

void Cpu::block_cp(bool inc, bool repeat) {
    uint8_t v = mr(hl());
    tick(5);
    uint8_t res = uint8_t(a - v);
    bool half = ((a ^ v ^ res) & FLAG_H) != 0;
    const int step = inc ? 1 : -1;
    set_hl(uint16_t(hl() + step));
    set_bc(uint16_t(bc() - 1));
    wz = uint16_t(wz + step);
    uint8_t n = uint8_t(res - (half ? 1 : 0));
    setf(uint8_t((f & FLAG_C) | (res & FLAG_S) |
                 (res == 0 ? FLAG_Z : 0) |
                 (half ? FLAG_H : 0) |
                 (bc() ? FLAG_PV : 0) |
                 FLAG_N |
                 ((n & 0x02) ? FLAG_Y : 0) |
                 (n & FLAG_X)));
    if (repeat && bc() != 0 && res != 0) {
        tick(5);
        pc = uint16_t(pc - 2);
        wz = uint16_t(pc + 1);
        setf(uint8_t((f & ~(FLAG_X | FLAG_Y)) | ((pc >> 8) & (FLAG_X | FLAG_Y))));
    }
}

void Cpu::block_io_flags(uint8_t v, unsigned t) {
    setf(uint8_t((b & (FLAG_S | FLAG_X | FLAG_Y)) |
                 (b == 0 ? FLAG_Z : 0) |
                 (t > 0xFF ? (FLAG_H | FLAG_C) : 0) |
                 (parity_even(uint8_t((t & 7) ^ b)) ? FLAG_PV : 0) |
                 ((v & 0x80) ? FLAG_N : 0)));
}

// Interrupted INIR/OTIR family: H and P/V follow B's next step (David Banks, 2018).
void Cpu::block_io_repeat_flags(uint8_t v) {
    wz = uint16_t(pc + 1);
    uint8_t nf = uint8_t((f & ~(FLAG_X | FLAG_Y)) | ((pc >> 8) & (FLAG_X | FLAG_Y)));
    bool p;
    if (f & FLAG_C) {
        nf = uint8_t(nf & ~FLAG_H);
        if (v & 0x80) {
            p = parity_even(uint8_t((b - 1) & 7));
            if ((b & 0x0F) == 0x00) nf |= FLAG_H;
        } else {
            p = parity_even(uint8_t((b + 1) & 7));
            if ((b & 0x0F) == 0x0F) nf |= FLAG_H;
        }
    } else {
        p = parity_even(uint8_t(b & 7));
    }
    if (!p) nf ^= FLAG_PV;
    setf(nf);
}

void Cpu::block_in(bool inc, bool repeat) {
    tick(1);
    uint8_t v = ior(bc());
    wz = uint16_t(bc() + (inc ? 1 : -1));
    b = uint8_t(b - 1);
    mw(hl(), v);
    set_hl(uint16_t(hl() + (inc ? 1 : -1)));
    block_io_flags(v, unsigned(uint8_t(c + (inc ? 1 : -1))) + v);
    if (repeat && b != 0) {
        tick(5);
        pc = uint16_t(pc - 2);
        block_io_repeat_flags(v);
    }
}

void Cpu::block_out(bool inc, bool repeat) {
    tick(1);
    uint8_t v = mr(hl());
    b = uint8_t(b - 1);
    wz = uint16_t(bc() + (inc ? 1 : -1));
    iow(bc(), v);
    set_hl(uint16_t(hl() + (inc ? 1 : -1)));
    block_io_flags(v, unsigned(l) + v);
    if (repeat && b != 0) {
        tick(5);
        pc = uint16_t(pc - 2);
        block_io_repeat_flags(v);
    }
}

// --- instruction loop and interrupts ---

int Cpu::step() {
    const uint64_t start = cycles;
    if (nmi_pending_) {
        accept_nmi();
    } else if (int_line && iff1 && !after_ei) {
        accept_int();
    } else {
        after_ei = after_ld_air = false;
        if (halted) {
            // Halted M1s fetch and discard the byte after HALT (Brewer 2014).
            (void)m1(pc);
            q = 0;
        } else {
            flags_written_ = false;
            execute(fetch_op());
            q = flags_written_ ? f : 0;
        }
    }
    return int(cycles - start);
}

int Cpu::interrupt() {
    if (!iff1 || after_ei) return 0;
    const uint64_t start = cycles;
    accept_int();
    return int(cycles - start);
}

int Cpu::nmi() {
    const uint64_t start = cycles;
    accept_nmi();
    return int(cycles - start);
}

void Cpu::accept_int() {
    // NMOS: INT accepted right after LD A,I/R reads P/V as 0 (Young).
    if (after_ld_air) f = uint8_t(f & ~FLAG_PV);
    after_ei = after_ld_air = false;
    halted = false;
    iff1 = iff2 = false;

    // INTA: M1 with two automatic wait states, data from the device.
    tick(2);
    tick(2);
    tick(waits(pc, Cycle::IntAck));
    uint8_t data = bus_.irq_data ? bus_.irq_data() : uint8_t(0xFF);
    if (bus_.refresh) bus_.refresh(uint16_t((i << 8) | r));
    bump_r();
    tick(2);

    if (im == 0) {
        from_bus_ = true;
        flags_written_ = false;
        execute(data);
        from_bus_ = false;
        q = flags_written_ ? f : 0;
        return;
    }
    tick(1);
    push16(pc);
    if (im == 1) {
        pc = 0x0038;
    } else {
        uint16_t vec = uint16_t((i << 8) | data);
        uint8_t lo = mr(vec);
        uint8_t hi = mr(uint16_t(vec + 1));
        pc = uint16_t(lo | (hi << 8));
    }
    wz = pc;
}

void Cpu::accept_nmi() {
    nmi_pending_ = false;
    if (after_ld_air) f = uint8_t(f & ~FLAG_PV);
    after_ei = after_ld_air = false;
    halted = false;
    // IFF2 keeps the pre-NMI IFF1 for RETN (UM0080).
    iff1 = false;
    (void)m1(pc);
    tick(1);
    push16(pc);
    pc = 0x0066;
    wz = pc;
}

}  // namespace z80
