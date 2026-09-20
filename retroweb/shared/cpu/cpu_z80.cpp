#include "cpu_z80.h"

namespace z80 {

void Cpu::reset() {
    a = f = b = c = d = e = h = l = 0;
    a_ = f_ = b_ = c_ = d_ = e_ = h_ = l_ = 0;
    ix = iy = sp = pc = 0;
    i = r = 0;
    im = 0;
    iff1 = iff2 = false;
    halted = false;
    ei_delay_ = 0;
    cycles = 0;
}

bool Cpu::parity_even(uint8_t v) {
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (v & 1) == 0;
}

void Cpu::set_szxy(uint8_t v) {
    f = uint8_t((f & (FLAG_C | FLAG_N | FLAG_PV | FLAG_H)) |
                (v & (FLAG_S | FLAG_X | FLAG_Y)) |
                (v == 0 ? FLAG_Z : 0));
}

void Cpu::set_szxy_p(uint8_t v) {
    f = uint8_t((f & (FLAG_C | FLAG_N | FLAG_H)) |
                (v & (FLAG_S | FLAG_X | FLAG_Y)) |
                (v == 0 ? FLAG_Z : 0) |
                (parity_even(v) ? FLAG_PV : 0));
}

uint8_t Cpu::fetch8() {
    uint8_t v = read8(pc);
    pc = uint16_t(pc + 1);
    return v;
}

uint16_t Cpu::fetch16() {
    uint8_t lo = fetch8();
    uint8_t hi = fetch8();
    return uint16_t(lo | (uint16_t(hi) << 8));
}

uint8_t Cpu::read8(uint16_t addr) { return bus_.read(addr); }

void Cpu::write8(uint16_t addr, uint8_t v) { bus_.write(addr, v); }

uint16_t Cpu::read16(uint16_t addr) {
    return uint16_t(read8(addr) | (uint16_t(read8(uint16_t(addr + 1))) << 8));
}

void Cpu::write16(uint16_t addr, uint16_t v) {
    write8(addr, uint8_t(v));
    write8(uint16_t(addr + 1), uint8_t(v >> 8));
}

void Cpu::push16(uint16_t v) {
    sp = uint16_t(sp - 1);
    write8(sp, uint8_t(v >> 8));
    sp = uint16_t(sp - 1);
    write8(sp, uint8_t(v));
}

uint16_t Cpu::pop16() {
    uint8_t lo = read8(sp);
    sp = uint16_t(sp + 1);
    uint8_t hi = read8(sp);
    sp = uint16_t(sp + 1);
    return uint16_t(lo | (uint16_t(hi) << 8));
}

uint8_t Cpu::add8(uint8_t x, uint8_t y, bool cin) {
    unsigned r = unsigned(x) + y + (cin ? 1u : 0u);
    unsigned hc = (x & 0x0F) + (y & 0x0F) + (cin ? 1u : 0u);
    uint8_t res = uint8_t(r);
    bool ov = ((x ^ res) & (y ^ res) & 0x80) != 0;
    f = uint8_t((res & (FLAG_S | FLAG_X | FLAG_Y)) |
                (res == 0 ? FLAG_Z : 0) |
                (hc & 0x10 ? FLAG_H : 0) |
                (ov ? FLAG_PV : 0) |
                (r > 0xFF ? FLAG_C : 0));
    return res;
}

uint8_t Cpu::sub8(uint8_t x, uint8_t y, bool bin) {
    unsigned r = unsigned(x) - y - (bin ? 1u : 0u);
    unsigned hc = (x & 0x0F) - (y & 0x0F) - (bin ? 1u : 0u);
    uint8_t res = uint8_t(r);
    bool ov = ((x ^ y) & (x ^ res) & 0x80) != 0;
    f = uint8_t((res & (FLAG_S | FLAG_X | FLAG_Y)) |
                (res == 0 ? FLAG_Z : 0) |
                (hc & 0x10 ? FLAG_H : 0) |
                (ov ? FLAG_PV : 0) |
                FLAG_N |
                (r > 0xFF ? FLAG_C : 0));
    return res;
}

void Cpu::add16(uint16_t& dest, uint16_t v) {
    unsigned r = unsigned(dest) + v;
    unsigned hc = (dest & 0x0FFF) + (v & 0x0FFF);
    uint8_t hi = uint8_t(r >> 8);
    f = uint8_t((f & (FLAG_S | FLAG_Z | FLAG_PV)) |
                (hi & (FLAG_X | FLAG_Y)) |
                (hc & 0x1000 ? FLAG_H : 0) |
                (r > 0xFFFF ? FLAG_C : 0));
    dest = uint16_t(r);
}

void Cpu::adc16(uint16_t& dest, uint16_t v) {
    unsigned cin = flag(FLAG_C) ? 1u : 0u;
    unsigned r = unsigned(dest) + v + cin;
    unsigned hc = (dest & 0x0FFF) + (v & 0x0FFF) + cin;
    uint16_t res = uint16_t(r);
    bool ov = ((dest ^ res) & (v ^ res) & 0x8000) != 0;
    uint8_t hi = uint8_t(res >> 8);
    f = uint8_t((hi & (FLAG_S | FLAG_X | FLAG_Y)) |
                (res == 0 ? FLAG_Z : 0) |
                (hc & 0x1000 ? FLAG_H : 0) |
                (ov ? FLAG_PV : 0) |
                (r > 0xFFFF ? FLAG_C : 0));
    dest = res;
}

void Cpu::sbc16(uint16_t& dest, uint16_t v) {
    unsigned bin = flag(FLAG_C) ? 1u : 0u;
    unsigned r = unsigned(dest) - v - bin;
    unsigned hc = (dest & 0x0FFF) - (v & 0x0FFF) - bin;
    uint16_t res = uint16_t(r);
    bool ov = ((dest ^ v) & (dest ^ res) & 0x8000) != 0;
    uint8_t hi = uint8_t(res >> 8);
    f = uint8_t((hi & (FLAG_S | FLAG_X | FLAG_Y)) |
                (res == 0 ? FLAG_Z : 0) |
                (hc & 0x1000 ? FLAG_H : 0) |
                (ov ? FLAG_PV : 0) |
                FLAG_N |
                (r > 0xFFFF ? FLAG_C : 0));
    dest = res;
}

void Cpu::logic_and(uint8_t v) {
    a &= v;
    f = FLAG_H;
    set_szxy_p(a);
}

void Cpu::logic_or(uint8_t v) {
    a |= v;
    f = 0;
    set_szxy_p(a);
}

void Cpu::logic_xor(uint8_t v) {
    a ^= v;
    f = 0;
    set_szxy_p(a);
}

void Cpu::cp8(uint8_t v) {
    (void)sub8(a, v, false);
    // CP copies X/Y from the operand, not the result (Zilog).
    f = uint8_t((f & ~(FLAG_X | FLAG_Y)) | (v & (FLAG_X | FLAG_Y)));
}

uint8_t Cpu::inc8(uint8_t v) {
    uint8_t r = uint8_t(v + 1);
    bool ov = v == 0x7F;
    f = uint8_t((f & FLAG_C) |
                (r & (FLAG_S | FLAG_X | FLAG_Y)) |
                (r == 0 ? FLAG_Z : 0) |
                ((v & 0x0F) == 0x0F ? FLAG_H : 0) |
                (ov ? FLAG_PV : 0));
    return r;
}

uint8_t Cpu::dec8(uint8_t v) {
    uint8_t r = uint8_t(v - 1);
    bool ov = v == 0x80;
    f = uint8_t((f & FLAG_C) |
                (r & (FLAG_S | FLAG_X | FLAG_Y)) |
                (r == 0 ? FLAG_Z : 0) |
                ((v & 0x0F) == 0 ? FLAG_H : 0) |
                (ov ? FLAG_PV : 0) |
                FLAG_N);
    return r;
}

void Cpu::daa() {
    uint8_t t = 0;
    bool c = flag(FLAG_C);
    if (flag(FLAG_H) || (a & 0x0F) > 9) t |= 0x06;
    if (c || a > 0x99) {
        t |= 0x60;
        c = true;
    }
    if (flag(FLAG_N)) {
        uint8_t r = uint8_t(a - t);
        bool h = ((a & 0x0F) < (t & 0x0F));
        a = r;
        f = uint8_t((f & FLAG_N) | (c ? FLAG_C : 0) | (h ? FLAG_H : 0));
        set_szxy_p(a);
        set_flag(FLAG_N, true);
    } else {
        uint8_t r = uint8_t(a + t);
        bool h = ((a & 0x0F) + (t & 0x0F)) > 0x0F;
        a = r;
        f = uint8_t((c ? FLAG_C : 0) | (h ? FLAG_H : 0));
        set_szxy_p(a);
    }
}

uint8_t Cpu::rot_left(uint8_t v, bool with_carry, bool circular) {
    bool c = with_carry ? flag(FLAG_C) : false;
    bool out = (v & 0x80) != 0;
    uint8_t r = uint8_t(v << 1);
    if (circular) r = uint8_t(r | (out ? 1 : 0));
    else if (with_carry) r = uint8_t(r | (c ? 1 : 0));
    f = uint8_t(out ? FLAG_C : 0);
    set_szxy_p(r);
    return r;
}

uint8_t Cpu::rot_right(uint8_t v, bool with_carry, bool circular) {
    bool c = with_carry ? flag(FLAG_C) : false;
    bool out = (v & 0x01) != 0;
    uint8_t r = uint8_t(v >> 1);
    if (circular) r = uint8_t(r | (out ? 0x80 : 0));
    else if (with_carry) r = uint8_t(r | (c ? 0x80 : 0));
    f = uint8_t(out ? FLAG_C : 0);
    set_szxy_p(r);
    return r;
}

uint16_t& Cpu::xy_reg(int xy) { return xy == 0xDD ? ix : iy; }

uint8_t Cpu::read_xy_d(int xy, int8_t d) {
    return read8(uint16_t(xy_reg(xy) + d));
}

void Cpu::write_xy_d(int xy, int8_t d, uint8_t v) {
    write8(uint16_t(xy_reg(xy) + d), v);
}

uint8_t Cpu::get_r(int z, int xy) {
    switch (z) {
        case 0: return b;
        case 1: return c;
        case 2: return d;
        case 3: return e;
        case 4:
            if (xy) return uint8_t(xy_reg(xy) >> 8);
            return h;
        case 5:
            if (xy) return uint8_t(xy_reg(xy));
            return l;
        case 6: return read8(hl());
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
        case 6: write8(hl(), v); break;
        default: a = v; break;
    }
}

int Cpu::block_ldi(bool repeat, bool inc) {
    uint8_t v = read8(hl());
    write8(de(), v);
    set_hl(uint16_t(hl() + (inc ? 1 : -1)));
    set_de(uint16_t(de() + (inc ? 1 : -1)));
    set_bc(uint16_t(bc() - 1));
    uint8_t n = uint8_t(a + v);
    f = uint8_t((f & (FLAG_S | FLAG_Z | FLAG_C)) |
                ((n & 0x02) ? FLAG_Y : 0) |
                ((n & 0x08) ? FLAG_X : 0) |
                (bc() != 0 ? FLAG_PV : 0));
    if (repeat && bc() != 0) {
        pc = uint16_t(pc - 2);
        return 21;
    }
    return 16;
}

int Cpu::block_cpi(bool repeat, bool inc) {
    uint8_t v = read8(hl());
    uint8_t res = uint8_t(a - v);
    set_hl(uint16_t(hl() + (inc ? 1 : -1)));
    set_bc(uint16_t(bc() - 1));
    bool h = ((a & 0x0F) < (v & 0x0F));
    uint8_t n = uint8_t(res - (h ? 1 : 0));
    f = uint8_t((f & FLAG_C) |
                (res & FLAG_S) |
                (res == 0 ? FLAG_Z : 0) |
                (h ? FLAG_H : 0) |
                (bc() != 0 ? FLAG_PV : 0) |
                FLAG_N |
                ((n & 0x02) ? FLAG_Y : 0) |
                ((n & 0x08) ? FLAG_X : 0));
    if (repeat && bc() != 0 && res != 0) {
        pc = uint16_t(pc - 2);
        return 21;
    }
    return 16;
}

int Cpu::exec_cb(uint8_t op, int xy, int8_t d) {
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    uint8_t v;
    if (xy) v = read_xy_d(xy, d);
    else if (z == 6) v = read8(hl());
    else v = get_r(z, 0);

    if (x == 0) {
        switch (y) {
            case 0: v = rot_left(v, false, true); break;   // RLC
            case 1: v = rot_right(v, false, true); break;  // RRC
            case 2: v = rot_left(v, true, false); break;   // RL
            case 3: v = rot_right(v, true, false); break;  // RR
            case 4: {  // SLA
                bool c = (v & 0x80) != 0;
                v = uint8_t(v << 1);
                f = uint8_t(c ? FLAG_C : 0);
                set_szxy_p(v);
                break;
            }
            case 5: {  // SRA
                bool c = (v & 1) != 0;
                v = uint8_t((v >> 1) | (v & 0x80));
                f = uint8_t(c ? FLAG_C : 0);
                set_szxy_p(v);
                break;
            }
            case 6: {  // SLL (undocumented: shift left, 1 into bit 0)
                bool c = (v & 0x80) != 0;
                v = uint8_t((v << 1) | 1);
                f = uint8_t(c ? FLAG_C : 0);
                set_szxy_p(v);
                break;
            }
            default: {  // SRL
                bool c = (v & 1) != 0;
                v = uint8_t(v >> 1);
                f = uint8_t(c ? FLAG_C : 0);
                set_szxy_p(v);
                break;
            }
        }
    } else if (x == 1) {
        // BIT y, r — Zilog: X/Y from the operand for register BIT, from
        // the high byte of IX+d for indexed BIT.
        bool zf = (v & (1u << y)) == 0;
        uint8_t xyf = xy ? uint8_t((xy_reg(xy) + d) >> 8) : v;
        f = uint8_t((f & FLAG_C) | FLAG_H | (zf ? (FLAG_Z | FLAG_PV) : 0) |
                    ((v & 0x80) && y == 7 ? FLAG_S : 0) |
                    (xyf & (FLAG_X | FLAG_Y)));
        if (xy) return 16;  // 20 with DD/FD prefix
        return z == 6 ? 12 : 8;
    } else if (x == 2) {
        v = uint8_t(v & ~(1u << y));
    } else {
        v = uint8_t(v | (1u << y));
    }

    if (x != 1) {
        if (xy) {
            write_xy_d(xy, d, v);
            if (z != 6) set_r(z, 0, v);  // undocumented: also write register
            return 19;  // 23 with DD/FD prefix
        }
        if (z == 6) {
            write8(hl(), v);
            return 15;
        }
        set_r(z, 0, v);
        return 8;
    }
    return 8;
}

int Cpu::exec_ed(uint8_t op) {
    switch (op) {
        case 0x40: case 0x48: case 0x50: case 0x58:
        case 0x60: case 0x68: case 0x70: case 0x78: {
            int z = (op >> 3) & 7;
            uint8_t v = bus_.in ? bus_.in(c) : uint8_t(0xFF);
            f = uint8_t(f & FLAG_C);
            set_szxy_p(v);
            if (z != 6) set_r(z, 0, v);
            return 12;
        }
        case 0x41: case 0x49: case 0x51: case 0x59:
        case 0x61: case 0x69: case 0x71: case 0x79: {
            int z = (op >> 3) & 7;
            uint8_t v = (z == 6) ? 0 : get_r(z, 0);
            if (bus_.out) bus_.out(c, v);
            return 12;
        }
        case 0x42: { uint16_t hl = this->hl(); sbc16(hl, bc()); set_hl(hl); return 15; }
        case 0x52: { uint16_t hl = this->hl(); sbc16(hl, de()); set_hl(hl); return 15; }
        case 0x62: { uint16_t hl = this->hl(); sbc16(hl, hl); set_hl(hl); return 15; }
        case 0x72: { uint16_t hl = this->hl(); sbc16(hl, sp); set_hl(hl); return 15; }
        case 0x4A: { uint16_t hl = this->hl(); adc16(hl, bc()); set_hl(hl); return 15; }
        case 0x5A: { uint16_t hl = this->hl(); adc16(hl, de()); set_hl(hl); return 15; }
        case 0x6A: { uint16_t hl = this->hl(); adc16(hl, hl); set_hl(hl); return 15; }
        case 0x7A: { uint16_t hl = this->hl(); adc16(hl, sp); set_hl(hl); return 15; }
        case 0x43: write16(fetch16(), bc()); return 20;
        case 0x53: write16(fetch16(), de()); return 20;
        case 0x63: write16(fetch16(), hl()); return 20;
        case 0x73: write16(fetch16(), sp); return 20;
        case 0x4B: set_bc(read16(fetch16())); return 20;
        case 0x5B: set_de(read16(fetch16())); return 20;
        case 0x6B: set_hl(read16(fetch16())); return 20;
        case 0x7B: sp = read16(fetch16()); return 20;
        case 0x44: case 0x4C: case 0x54: case 0x5C:
        case 0x64: case 0x6C: case 0x74: case 0x7C:
            a = sub8(0, a, false); return 8;
        case 0x45: case 0x55: case 0x5D: case 0x65:
        case 0x6D: case 0x75: case 0x7D:
            // RETN
            iff1 = iff2;
            pc = pop16();
            return 14;
        case 0x4D:
            iff1 = iff2;
            pc = pop16();
            return 14;
        case 0x46: case 0x4E: case 0x66: case 0x6E: im = 0; return 8;
        case 0x56: case 0x76: im = 1; return 8;
        case 0x5E: case 0x7E: im = 2; return 8;
        case 0x47: i = a; return 9;
        case 0x4F: r = a; return 9;
        case 0x57:
            a = i;
            f = uint8_t((f & FLAG_C) | (a & (FLAG_S | FLAG_X | FLAG_Y)) |
                        (a == 0 ? FLAG_Z : 0) | (iff2 ? FLAG_PV : 0));
            return 9;
        case 0x5F:
            a = r;
            f = uint8_t((f & FLAG_C) | (a & (FLAG_S | FLAG_X | FLAG_Y)) |
                        (a == 0 ? FLAG_Z : 0) | (iff2 ? FLAG_PV : 0));
            return 9;
        case 0x67: {  // RRD
            uint8_t m = read8(hl());
            uint8_t na = uint8_t((a & 0xF0) | (m & 0x0F));
            write8(hl(), uint8_t((m >> 4) | (a << 4)));
            a = na;
            f = uint8_t(f & FLAG_C);
            set_szxy_p(a);
            return 18;
        }
        case 0x6F: {  // RLD
            uint8_t m = read8(hl());
            uint8_t na = uint8_t((a & 0xF0) | (m >> 4));
            write8(hl(), uint8_t((m << 4) | (a & 0x0F)));
            a = na;
            f = uint8_t(f & FLAG_C);
            set_szxy_p(a);
            return 18;
        }
        case 0xA0: return block_ldi(false, true);
        case 0xB0: return block_ldi(true, true);
        case 0xA8: return block_ldi(false, false);
        case 0xB8: return block_ldi(true, false);
        case 0xA1: return block_cpi(false, true);
        case 0xB1: return block_cpi(true, true);
        case 0xA9: return block_cpi(false, false);
        case 0xB9: return block_cpi(true, false);
        default:
            return 8;
    }
}

int Cpu::exec_main(uint8_t op, int xy) {
    auto rr = [&](int p) -> uint16_t {
        switch (p) {
            case 0: return bc();
            case 1: return de();
            case 2: return xy ? xy_reg(xy) : hl();
            default: return sp;
        }
    };
    auto set_rr = [&](int p, uint16_t v) {
        switch (p) {
            case 0: set_bc(v); break;
            case 1: set_de(v); break;
            case 2:
                if (xy) xy_reg(xy) = v;
                else set_hl(v);
                break;
            default: sp = v; break;
        }
    };
    auto hlxy = [&]() -> uint16_t { return xy ? xy_reg(xy) : hl(); };
    auto set_hlxy = [&](uint16_t v) {
        if (xy) xy_reg(xy) = v;
        else set_hl(v);
    };

    int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    int p = y >> 1, q = y & 1;

    if (x == 0) {
        switch (z) {
            case 0:
                if (y == 0) return 4;  // NOP
                if (y == 1) {  // EX AF,AF'
                    uint8_t ta = a, tf = f;
                    a = a_; f = f_; a_ = ta; f_ = tf;
                    return 4;
                }
                if (y == 2) {  // DJNZ d
                    int8_t d = int8_t(fetch8());
                    b = uint8_t(b - 1);
                    if (b) {
                        pc = uint16_t(pc + d);
                        return 13;
                    }
                    return 8;
                }
                if (y == 3) {  // JR d
                    int8_t d = int8_t(fetch8());
                    pc = uint16_t(pc + d);
                    return 12;
                }
                {  // JR cc, d
                    int8_t d = int8_t(fetch8());
                    bool take = false;
                    switch (y) {
                        case 4: take = !flag(FLAG_Z); break;
                        case 5: take = flag(FLAG_Z); break;
                        case 6: take = !flag(FLAG_C); break;
                        default: take = flag(FLAG_C); break;
                    }
                    if (take) {
                        pc = uint16_t(pc + d);
                        return 12;
                    }
                    return 7;
                }
            case 1:
                if (q == 0) {
                    set_rr(p, fetch16());
                    return 10;
                }
                {
                    uint16_t hl = hlxy();
                    add16(hl, rr(p));
                    set_hlxy(hl);
                    return 11;
                }
            case 2:
                if (q == 0) {
                    switch (p) {
                        case 0: write8(bc(), a); return 7;
                        case 1: write8(de(), a); return 7;
                        case 2: write16(fetch16(), hlxy()); return 16;
                        default: write8(fetch16(), a); return 13;
                    }
                }
                switch (p) {
                    case 0: a = read8(bc()); return 7;
                    case 1: a = read8(de()); return 7;
                    case 2: set_hlxy(read16(fetch16())); return 16;
                    default: a = read8(fetch16()); return 13;
                }
            case 3:
                if (q == 0) { set_rr(p, uint16_t(rr(p) + 1)); return 6; }
                set_rr(p, uint16_t(rr(p) - 1));
                return 6;
            case 4: {  // INC r
                if (y == 6 && xy) {
                    int8_t d = int8_t(fetch8());
                    write_xy_d(xy, d, inc8(read_xy_d(xy, d)));
                    return 19;
                }
                if (y == 6) {
                    write8(hl(), inc8(read8(hl())));
                    return 11;
                }
                set_r(y, xy, inc8(get_r(y, xy)));
                return 4;
            }
            case 5: {
                if (y == 6 && xy) {
                    int8_t d = int8_t(fetch8());
                    write_xy_d(xy, d, dec8(read_xy_d(xy, d)));
                    return 19;
                }
                if (y == 6) {
                    write8(hl(), dec8(read8(hl())));
                    return 11;
                }
                set_r(y, xy, dec8(get_r(y, xy)));
                return 4;
            }
            case 6: {
                if (y == 6 && xy) {
                    int8_t d = int8_t(fetch8());
                    write_xy_d(xy, d, fetch8());
                    return 15;
                }
                if (y == 6) {
                    write8(hl(), fetch8());
                    return 10;
                }
                set_r(y, xy, fetch8());
                return 7;
            }
            default:  // z == 7
                switch (y) {
                    case 0: {  // RLCA
                        bool c = (a & 0x80) != 0;
                        a = uint8_t((a << 1) | (c ? 1 : 0));
                        f = uint8_t((f & (FLAG_S | FLAG_Z | FLAG_PV)) |
                                    (a & (FLAG_X | FLAG_Y)) | (c ? FLAG_C : 0));
                        return 4;
                    }
                    case 1: {  // RRCA
                        bool c = (a & 1) != 0;
                        a = uint8_t((a >> 1) | (c ? 0x80 : 0));
                        f = uint8_t((f & (FLAG_S | FLAG_Z | FLAG_PV)) |
                                    (a & (FLAG_X | FLAG_Y)) | (c ? FLAG_C : 0));
                        return 4;
                    }
                    case 2: {  // RLA
                        bool c = (a & 0x80) != 0;
                        a = uint8_t((a << 1) | (flag(FLAG_C) ? 1 : 0));
                        f = uint8_t((f & (FLAG_S | FLAG_Z | FLAG_PV)) |
                                    (a & (FLAG_X | FLAG_Y)) | (c ? FLAG_C : 0));
                        return 4;
                    }
                    case 3: {  // RRA
                        bool c = (a & 1) != 0;
                        a = uint8_t((a >> 1) | (flag(FLAG_C) ? 0x80 : 0));
                        f = uint8_t((f & (FLAG_S | FLAG_Z | FLAG_PV)) |
                                    (a & (FLAG_X | FLAG_Y)) | (c ? FLAG_C : 0));
                        return 4;
                    }
                    case 4: daa(); return 4;
                    case 5:  // CPL
                        a = uint8_t(~a);
                        f = uint8_t((f & ~(FLAG_X | FLAG_Y)) | FLAG_H | FLAG_N |
                                    (a & (FLAG_X | FLAG_Y)));
                        return 4;
                    case 6:  // SCF
                        f = uint8_t((f & (FLAG_S | FLAG_Z | FLAG_PV)) |
                                    (a & (FLAG_X | FLAG_Y)) | FLAG_C);
                        return 4;
                    default: {  // CCF
                        bool c = flag(FLAG_C);
                        f = uint8_t((f & (FLAG_S | FLAG_Z | FLAG_PV)) |
                                    (a & (FLAG_X | FLAG_Y)) |
                                    (c ? FLAG_H : 0) | (c ? 0 : FLAG_C));
                        return 4;
                    }
                }
        }
    }

    if (x == 1) {
        if (y == 6 && z == 6) {
            halted = true;
            return 4;
        }
        if (xy && (y == 6 || z == 6)) {
            int8_t d = int8_t(fetch8());
            if (y == 6) {
                write_xy_d(xy, d, get_r(z, 0));
                return 15;
            }
            set_r(y, 0, read_xy_d(xy, d));
            return 15;
        }
        // LD r,r' — IXH/IXL when xy set and neither is (HL)
        uint8_t v = get_r(z, (z == 4 || z == 5) ? xy : 0);
        set_r(y, (y == 4 || y == 5) ? xy : 0, v);
        return (y == 6 || z == 6) ? 7 : 4;
    }

    if (x == 2) {
        uint8_t v;
        int t = 4;
        if (z == 6 && xy) {
            int8_t d = int8_t(fetch8());
            v = read_xy_d(xy, d);
            t = 15;
        } else if (z == 6) {
            v = read8(hl());
            t = 7;
        } else {
            v = get_r(z, (z == 4 || z == 5) ? xy : 0);
        }
        switch (y) {
            case 0: a = add8(a, v, false); break;
            case 1: a = add8(a, v, flag(FLAG_C)); break;
            case 2: a = sub8(a, v, false); break;
            case 3: a = sub8(a, v, flag(FLAG_C)); break;
            case 4: logic_and(v); break;
            case 5: logic_xor(v); break;
            case 6: logic_or(v); break;
            default: cp8(v); break;
        }
        return t;
    }

    // x == 3
    auto cc = [&](int y) -> bool {
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
    };

    switch (z) {
        case 0:  // RET cc
            if (cc(y)) {
                pc = pop16();
                return 11;
            }
            return 5;
        case 1:
            if (q == 0) {
                uint16_t v = pop16();
                if (p == 0) set_bc(v);
                else if (p == 1) set_de(v);
                else if (p == 2) set_hlxy(v);
                else set_af(v);
                return 10;
            }
            switch (p) {
                case 0: pc = pop16(); return 10;  // RET
                case 1: {  // EXX
                    uint8_t t;
                    t = b; b = b_; b_ = t;
                    t = c; c = c_; c_ = t;
                    t = d; d = d_; d_ = t;
                    t = e; e = e_; e_ = t;
                    t = h; h = h_; h_ = t;
                    t = l; l = l_; l_ = t;
                    return 4;
                }
                case 2: pc = hlxy(); return 4;  // JP (HL/IX/IY)
                default: sp = hlxy(); return 6;  // LD SP,HL
            }
        case 2: {  // JP cc,nn
            uint16_t nn = fetch16();
            if (cc(y)) pc = nn;
            return 10;
        }
        case 3:
            switch (y) {
                case 0: pc = fetch16(); return 10;
                case 1: {  // CB prefix handled in step()
                    return 0;
                }
                case 2: {  // OUT (n),A
                    uint8_t n = fetch8();
                    if (bus_.out) bus_.out(n, a);
                    return 11;
                }
                case 3: {  // IN A,(n)
                    uint8_t n = fetch8();
                    a = bus_.in ? bus_.in(n) : uint8_t(0xFF);
                    return 11;
                }
                case 4: {  // EX (SP),HL/IX/IY
                    uint16_t v = read16(sp);
                    write16(sp, hlxy());
                    set_hlxy(v);
                    return 19;
                }
                case 5: {  // EX DE,HL (always HL, never IX)
                    uint16_t t = de();
                    set_de(hl());
                    set_hl(t);
                    return 4;
                }
                case 6: iff1 = iff2 = false; return 4;  // DI
                default:  // EI
                    iff1 = iff2 = true;
                    ei_delay_ = 1;
                    return 4;
            }
        case 4: {  // CALL cc,nn
            uint16_t nn = fetch16();
            if (cc(y)) {
                push16(pc);
                pc = nn;
                return 17;
            }
            return 10;
        }
        case 5:
            if (q == 0) {
                uint16_t v = (p == 0) ? bc() : (p == 1) ? de() : (p == 2) ? hlxy() : af();
                push16(v);
                return 11;
            }
            if (p == 0) {
                uint16_t nn = fetch16();
                push16(pc);
                pc = nn;
                return 17;
            }
            return 0;  // DD/ED/FD handled in step()
        case 6: {
            uint8_t n = fetch8();
            switch (y) {
                case 0: a = add8(a, n, false); break;
                case 1: a = add8(a, n, flag(FLAG_C)); break;
                case 2: a = sub8(a, n, false); break;
                case 3: a = sub8(a, n, flag(FLAG_C)); break;
                case 4: logic_and(n); break;
                case 5: logic_xor(n); break;
                case 6: logic_or(n); break;
                default: cp8(n); break;
            }
            return 7;
        }
        default:  // RST
            push16(pc);
            pc = uint16_t(y * 8);
            return 11;
    }
}

int Cpu::step() {
    if (ei_delay_ > 0) ei_delay_--;

    if (halted) {
        r = uint8_t((r & 0x80) | ((r + 1) & 0x7F));
        cycles += 4;
        return 4;
    }

    r = uint8_t((r & 0x80) | ((r + 1) & 0x7F));
    uint8_t op = fetch8();
    int xy = 0;
    int extra = 0;

    while (op == 0xDD || op == 0xFD) {
        xy = op;
        extra += 4;
        r = uint8_t((r & 0x80) | ((r + 1) & 0x7F));
        op = fetch8();
    }

    int t;
    if (op == 0xCB) {
        if (xy) {
            int8_t d = int8_t(fetch8());
            uint8_t cb = fetch8();
            t = exec_cb(cb, xy, d);
        } else {
            uint8_t cb = fetch8();
            r = uint8_t((r & 0x80) | ((r + 1) & 0x7F));
            t = exec_cb(cb, 0, 0);
        }
    } else if (op == 0xED) {
        uint8_t ed = fetch8();
        r = uint8_t((r & 0x80) | ((r + 1) & 0x7F));
        t = exec_ed(ed);
        xy = 0;  // ED ignores DD/FD
    } else {
        t = exec_main(op, xy);
    }
    t += extra;
    cycles += uint64_t(t);
    return t;
}

int Cpu::interrupt() {
    if (!iff1 || ei_delay_ > 0) return 0;
    iff1 = iff2 = false;
    halted = false;
    int t;
    if (im == 2) {
        uint8_t data = bus_.irq_data ? bus_.irq_data() : uint8_t(0xFF);
        uint16_t vec = uint16_t((uint16_t(i) << 8) | data);
        push16(pc);
        pc = read16(vec);
        t = 19;
    } else if (im == 0) {
        uint8_t data = bus_.irq_data ? bus_.irq_data() : uint8_t(0xFF);
        push16(pc);
        // Hardware usually jams RST 38 (0xFF). Treat any RST the same way.
        pc = uint16_t((data & 0x38));
        t = 13;
    } else {
        push16(pc);
        pc = 0x0038;
        t = 13;
    }
    cycles += uint64_t(t);
    return t;
}

int Cpu::nmi() {
    iff2 = iff1;
    iff1 = false;
    halted = false;
    push16(pc);
    pc = 0x0066;
    cycles += 11;
    return 11;
}

}  // namespace z80
