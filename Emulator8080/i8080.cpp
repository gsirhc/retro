#include "i8080.h"

namespace i8080 {

// Base T-state count per opcode. Conditional CALL/RET that are *taken* cost
// more; those deltas are added in the decoder.
static const uint8_t kCycles[256] = {
//   0   1   2   3   4   5   6   7   8   9   A   B   C   D   E   F
     4, 10,  7,  5,  5,  5,  7,  4,  4, 10,  7,  5,  5,  5,  7,  4, // 00
     4, 10,  7,  5,  5,  5,  7,  4,  4, 10,  7,  5,  5,  5,  7,  4, // 10
     4, 10, 16,  5,  5,  5,  7,  4,  4, 10, 16,  5,  5,  5,  7,  4, // 20
     4, 10, 13,  5, 10, 10, 10,  4,  4, 10, 13,  5,  5,  5,  7,  4, // 30
     5,  5,  5,  5,  5,  5,  7,  5,  5,  5,  5,  5,  5,  5,  7,  5, // 40
     5,  5,  5,  5,  5,  5,  7,  5,  5,  5,  5,  5,  5,  5,  7,  5, // 50
     5,  5,  5,  5,  5,  5,  7,  5,  5,  5,  5,  5,  5,  5,  7,  5, // 60
     7,  7,  7,  7,  7,  7,  7,  7,  5,  5,  5,  5,  5,  5,  7,  5, // 70
     4,  4,  4,  4,  4,  4,  7,  4,  4,  4,  4,  4,  4,  4,  7,  4, // 80
     4,  4,  4,  4,  4,  4,  7,  4,  4,  4,  4,  4,  4,  4,  7,  4, // 90
     4,  4,  4,  4,  4,  4,  7,  4,  4,  4,  4,  4,  4,  4,  7,  4, // A0
     4,  4,  4,  4,  4,  4,  7,  4,  4,  4,  4,  4,  4,  4,  7,  4, // B0
     5, 10, 10, 10, 11, 11,  7, 11,  5, 10, 10, 10, 11, 17,  7, 11, // C0
     5, 10, 10, 10, 11, 11,  7, 11,  5, 10, 10, 10, 11, 17,  7, 11, // D0
     5, 10, 10, 18, 11, 11,  7, 11,  5,  5, 10,  5, 11, 17,  7, 11, // E0
     5, 10, 10,  4, 11, 11,  7, 11,  5,  5, 10,  4, 11, 17,  7, 11, // F0
};

void Cpu::reset() {
    a = b = c = d = e = h = l = 0;
    f = FLAG_N1;
    pc = sp = 0;
    halted = false;
    int_enabled = false;
    cycles = 0;
}

// --- flag helpers -----------------------------------------------------------

bool Cpu::parity_even(uint8_t v) {
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (v & 1) == 0;
}

void Cpu::set_szp(uint8_t v) {
    set_flag(FLAG_S, v & 0x80);
    set_flag(FLAG_Z, v == 0);
    set_flag(FLAG_P, parity_even(v));
}

// --- ALU primitives -------------------------------------------------------

void Cpu::add(uint8_t v, bool carry_in) {
    uint16_t cin = carry_in ? 1 : 0;
    uint16_t r = uint16_t(a) + v + cin;
    set_flag(FLAG_C, r > 0xFF);
    set_flag(FLAG_AC, ((a & 0xF) + (v & 0xF) + cin) > 0xF);
    a = uint8_t(r);
    set_szp(a);
}

void Cpu::sub(uint8_t v, bool borrow_in) {
    uint16_t bin = borrow_in ? 1 : 0;
    uint16_t r = uint16_t(a) - v - bin;
    set_flag(FLAG_C, r > 0xFF);                     // set on borrow
    // The 8080 subtracts via A + ~v + 1, so AC is the half-carry of that
    // internal add — the complement of the plain add-style expression.
    set_flag(FLAG_AC, (~(a ^ v ^ uint8_t(r)) & 0x10) != 0);
    a = uint8_t(r);
    set_szp(a);
}

void Cpu::cmp(uint8_t v) {
    uint16_t r = uint16_t(a) - v;
    set_flag(FLAG_C, r > 0xFF);
    set_flag(FLAG_AC, (~(a ^ v ^ uint8_t(r)) & 0x10) != 0);
    set_szp(uint8_t(r));
}

void Cpu::ana(uint8_t v) {
    // 8080 quirk: AC is set from the OR of bit 3 of the two operands.
    set_flag(FLAG_AC, ((a | v) & 0x08) != 0);
    a &= v;
    set_flag(FLAG_C, false);
    set_szp(a);
}

void Cpu::xra(uint8_t v) {
    a ^= v;
    set_flag(FLAG_C, false);
    set_flag(FLAG_AC, false);
    set_szp(a);
}

void Cpu::ora(uint8_t v) {
    a |= v;
    set_flag(FLAG_C, false);
    set_flag(FLAG_AC, false);
    set_szp(a);
}

uint8_t Cpu::inr(uint8_t v) {
    uint8_t r = v + 1;
    set_flag(FLAG_AC, (v & 0xF) == 0xF);
    set_szp(r);
    return r;
}

uint8_t Cpu::dcr(uint8_t v) {
    uint8_t r = v - 1;
    set_flag(FLAG_AC, (v & 0xF) != 0x0);            // no borrow from bit 4 when low nibble != 0
    set_szp(r);
    return r;
}

void Cpu::dad(uint16_t v) {
    uint32_t r = uint32_t(hl()) + v;
    set_flag(FLAG_C, r > 0xFFFF);
    set_hl(uint16_t(r));
}

void Cpu::daa() {
    uint8_t add_val = 0;
    bool carry = flag(FLAG_C);
    uint8_t lo = a & 0x0F;
    if (lo > 9 || flag(FLAG_AC)) add_val += 0x06;
    if (a > 0x99 || carry) { add_val += 0x60; carry = true; }
    // AC out of the low-nibble adjustment:
    set_flag(FLAG_AC, ((a & 0xF) + (add_val & 0xF)) > 0xF);
    a += add_val;
    set_flag(FLAG_C, carry);
    set_szp(a);
}

void Cpu::rlc() {
    uint8_t hi = a >> 7;
    a = (a << 1) | hi;
    set_flag(FLAG_C, hi);
}

void Cpu::rrc() {
    uint8_t lo = a & 1;
    a = (a >> 1) | (lo << 7);
    set_flag(FLAG_C, lo);
}

void Cpu::ral() {
    uint8_t hi = a >> 7;
    a = (a << 1) | (flag(FLAG_C) ? 1 : 0);
    set_flag(FLAG_C, hi);
}

void Cpu::rar() {
    uint8_t lo = a & 1;
    a = (a >> 1) | (flag(FLAG_C) ? 0x80 : 0);
    set_flag(FLAG_C, lo);
}

// --- register-file access by opcode bit-field ---------------------------

uint8_t Cpu::get_r(int idx) {
    switch (idx) {
        case 0: return b;
        case 1: return c;
        case 2: return d;
        case 3: return e;
        case 4: return h;
        case 5: return l;
        case 6: return rb(hl());   // "M" — memory at HL
        case 7: return a;
    }
    return 0;
}

void Cpu::set_r(int idx, uint8_t v) {
    switch (idx) {
        case 0: b = v; break;
        case 1: c = v; break;
        case 2: d = v; break;
        case 3: e = v; break;
        case 4: h = v; break;
        case 5: l = v; break;
        case 6: wb(hl(), v); break;
        case 7: a = v; break;
    }
}

// --- control-flow helpers ---------------------------------------------------

void Cpu::jump_if(bool cond) {
    uint16_t target = fetch16();
    if (cond) pc = target;
}

void Cpu::call_if(bool cond, int &extra_cycles) {
    uint16_t target = fetch16();
    if (cond) {
        push(pc);
        pc = target;
        extra_cycles += 6;
    }
}

void Cpu::ret_if(bool cond, int &extra_cycles) {
    if (cond) {
        pc = pop();
        extra_cycles += 6;
    }
}

// --- the decoder ----------------------------------------------------------

int Cpu::interrupt(uint8_t opcode) {
    if (!int_enabled) return 0;
    int_enabled = false;
    halted = false;
    // Feed the jammed opcode straight through the executor. RST n pushes PC
    // and vectors to n*8; a CALL supplies its own address bytes, which a real
    // device would also jam — not modelled here beyond the single opcode.
    push(pc);
    pc = (opcode & 0x38);
    cycles += 11;
    return 11;
}

int Cpu::step() {
    if (halted) {
        // A halted 8080 still burns cycles until an interrupt arrives.
        cycles += 4;
        return 4;
    }

    uint16_t op_pc = pc;
    (void)op_pc;
    uint8_t op = fetch8();
    int extra = 0;

    // MOV r,r  (0x40..0x7F) — 0x76 is HALT, handled below.
    if (op >= 0x40 && op <= 0x7F && op != 0x76) {
        int dst = (op >> 3) & 7;
        int src = op & 7;
        set_r(dst, get_r(src));
        cycles += kCycles[op];
        return kCycles[op];
    }

    // ALU A,r  (0x80..0xBF)
    if (op >= 0x80 && op <= 0xBF) {
        uint8_t v = get_r(op & 7);
        switch ((op >> 3) & 7) {
            case 0: add(v, false); break;              // ADD
            case 1: add(v, flag(FLAG_C)); break;       // ADC
            case 2: sub(v, false); break;              // SUB
            case 3: sub(v, flag(FLAG_C)); break;       // SBB
            case 4: ana(v); break;                     // ANA
            case 5: xra(v); break;                     // XRA
            case 6: ora(v); break;                     // ORA
            case 7: cmp(v); break;                     // CMP
        }
        cycles += kCycles[op];
        return kCycles[op];
    }

    switch (op) {
        // --- misc / NOP-likes ---------------------------------------------
        case 0x00: case 0x08: case 0x10: case 0x18:
        case 0x20: case 0x28: case 0x30: case 0x38:
            break;                                     // NOP (and undoc NOPs)

        case 0x76: halted = true; break;               // HLT

        // --- LXI rp,d16 --------------------------------------------------
        case 0x01: set_bc(fetch16()); break;
        case 0x11: set_de(fetch16()); break;
        case 0x21: set_hl(fetch16()); break;
        case 0x31: sp = fetch16(); break;

        // --- STAX / LDAX -----------------------------------------------
        case 0x02: wb(bc(), a); break;
        case 0x12: wb(de(), a); break;
        case 0x0A: a = rb(bc()); break;
        case 0x1A: a = rb(de()); break;

        // --- STA / LDA / SHLD / LHLD -----------------------------------
        case 0x32: wb(fetch16(), a); break;
        case 0x3A: a = rb(fetch16()); break;
        case 0x22: { uint16_t addr = fetch16(); ww(addr, hl()); } break;
        case 0x2A: { uint16_t addr = fetch16(); set_hl(rw(addr)); } break;

        // --- INX / DCX --------------------------------------------------
        case 0x03: set_bc(bc() + 1); break;
        case 0x13: set_de(de() + 1); break;
        case 0x23: set_hl(hl() + 1); break;
        case 0x33: sp++; break;
        case 0x0B: set_bc(bc() - 1); break;
        case 0x1B: set_de(de() - 1); break;
        case 0x2B: set_hl(hl() - 1); break;
        case 0x3B: sp--; break;

        // --- INR / DCR (r comes from bits 3-5) ------------------------
        case 0x04: case 0x0C: case 0x14: case 0x1C:
        case 0x24: case 0x2C: case 0x34: case 0x3C: {
            int r = (op >> 3) & 7;
            set_r(r, inr(get_r(r)));
        } break;
        case 0x05: case 0x0D: case 0x15: case 0x1D:
        case 0x25: case 0x2D: case 0x35: case 0x3D: {
            int r = (op >> 3) & 7;
            set_r(r, dcr(get_r(r)));
        } break;

        // --- MVI r,d8 -------------------------------------------------
        case 0x06: case 0x0E: case 0x16: case 0x1E:
        case 0x26: case 0x2E: case 0x36: case 0x3E: {
            int r = (op >> 3) & 7;
            set_r(r, fetch8());
        } break;

        // --- DAD rp --------------------------------------------------
        case 0x09: dad(bc()); break;
        case 0x19: dad(de()); break;
        case 0x29: dad(hl()); break;
        case 0x39: dad(sp);   break;

        // --- rotates -------------------------------------------------
        case 0x07: rlc(); break;
        case 0x0F: rrc(); break;
        case 0x17: ral(); break;
        case 0x1F: rar(); break;

        // --- DAA / CMA / STC / CMC ----------------------------------
        case 0x27: daa(); break;
        case 0x2F: a = ~a; break;
        case 0x37: set_flag(FLAG_C, true); break;
        case 0x3F: set_flag(FLAG_C, !flag(FLAG_C)); break;

        // --- ALU A,d8 ---------------------------------------------
        case 0xC6: add(fetch8(), false); break;        // ADI
        case 0xCE: add(fetch8(), flag(FLAG_C)); break;  // ACI
        case 0xD6: sub(fetch8(), false); break;         // SUI
        case 0xDE: sub(fetch8(), flag(FLAG_C)); break;  // SBI
        case 0xE6: ana(fetch8()); break;                // ANI
        case 0xEE: xra(fetch8()); break;                // XRI
        case 0xF6: ora(fetch8()); break;                // ORI
        case 0xFE: cmp(fetch8()); break;                // CPI

        // --- jumps -------------------------------------------------
        case 0xC3: case 0xCB: jump_if(true); break;     // JMP (0xCB undoc)
        case 0xC2: jump_if(!flag(FLAG_Z)); break;       // JNZ
        case 0xCA: jump_if(flag(FLAG_Z)); break;        // JZ
        case 0xD2: jump_if(!flag(FLAG_C)); break;       // JNC
        case 0xDA: jump_if(flag(FLAG_C)); break;        // JC
        case 0xE2: jump_if(!flag(FLAG_P)); break;       // JPO
        case 0xEA: jump_if(flag(FLAG_P)); break;        // JPE
        case 0xF2: jump_if(!flag(FLAG_S)); break;       // JP
        case 0xFA: jump_if(flag(FLAG_S)); break;        // JM
        case 0xE9: pc = hl(); break;                    // PCHL

        // --- calls -----------------------------------------------
        case 0xCD: case 0xDD: case 0xED: case 0xFD:
            call_if(true, extra); break;                // CALL (+ undoc)
        case 0xC4: call_if(!flag(FLAG_Z), extra); break;
        case 0xCC: call_if(flag(FLAG_Z), extra); break;
        case 0xD4: call_if(!flag(FLAG_C), extra); break;
        case 0xDC: call_if(flag(FLAG_C), extra); break;
        case 0xE4: call_if(!flag(FLAG_P), extra); break;
        case 0xEC: call_if(flag(FLAG_P), extra); break;
        case 0xF4: call_if(!flag(FLAG_S), extra); break;
        case 0xFC: call_if(flag(FLAG_S), extra); break;

        // --- returns --------------------------------------------
        case 0xC9: case 0xD9: pc = pop(); break;        // RET (0xD9 undoc)
        case 0xC0: ret_if(!flag(FLAG_Z), extra); break;
        case 0xC8: ret_if(flag(FLAG_Z), extra); break;
        case 0xD0: ret_if(!flag(FLAG_C), extra); break;
        case 0xD8: ret_if(flag(FLAG_C), extra); break;
        case 0xE0: ret_if(!flag(FLAG_P), extra); break;
        case 0xE8: ret_if(flag(FLAG_P), extra); break;
        case 0xF0: ret_if(!flag(FLAG_S), extra); break;
        case 0xF8: ret_if(flag(FLAG_S), extra); break;

        // --- RST n ----------------------------------------------
        case 0xC7: case 0xCF: case 0xD7: case 0xDF:
        case 0xE7: case 0xEF: case 0xF7: case 0xFF:
            push(pc);
            pc = op & 0x38;
            break;

        // --- stack: PUSH / POP rp ------------------------------
        case 0xC5: push(bc()); break;
        case 0xD5: push(de()); break;
        case 0xE5: push(hl()); break;
        case 0xF5: push((uint16_t(a) << 8) | (f & 0xD7) | FLAG_N1); break; // PSW
        case 0xC1: set_bc(pop()); break;
        case 0xD1: set_de(pop()); break;
        case 0xE1: set_hl(pop()); break;
        case 0xF1: { uint16_t v = pop();
                     a = v >> 8;
                     f = (v & 0xD7) | FLAG_N1; } break;

        // --- XCHG / XTHL / SPHL -------------------------------
        case 0xEB: { uint16_t t = hl(); set_hl(de()); set_de(t); } break;
        case 0xE3: { uint16_t t = rw(sp); ww(sp, hl()); set_hl(t); } break;
        case 0xF9: sp = hl(); break;

        // --- I/O and interrupt control -----------------------
        case 0xDB: { uint8_t port = fetch8();
                     a = bus_.in ? bus_.in(port) : 0xFF; } break;             // IN
        case 0xD3: { uint8_t port = fetch8();
                     if (bus_.out) bus_.out(port, a); } break;                // OUT
        case 0xFB: int_enabled = true; break;          // EI
        case 0xF3: int_enabled = false; break;         // DI

        default:
            // Every 8080 opcode is accounted for above; nothing should reach
            // here. Treat as NOP so a stray byte can't wedge the core.
            break;
    }

    int total = kCycles[op] + extra;
    cycles += total;
    return total;
}

} // namespace i8080
