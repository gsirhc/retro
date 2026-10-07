// MOS Technology NMOS 6502 — see cpu_mos6502.h for the interface and
// citation trail. Documented opcodes follow the MCS6500 Family Hardware
// Manual cycle tables; undocumented opcodes follow the Nesdev / Visual6502
// stable set (unstable bus-fight ops use the common MAGIC approximations
// noted inline).

#include "cpu_mos6502.h"

namespace mos6502 {

void Cpu::reset() {
    a = x = y = 0;
    sp = 0xFD;
    // NMOS does not clear D on reset.
    p = uint8_t((p & FLAG_D) | FLAG_U | FLAG_I);
    jammed = false;
    nmi_pending_ = false;
    irq_line = false;
    pc = uint16_t(rb(0xFFFC)) | (uint16_t(rb(0xFFFD)) << 8);
    cycles = 0;
}

void Cpu::nmi() { nmi_pending_ = true; }

void Cpu::service_irq(bool is_nmi, bool is_brk) {
    if (is_brk) pc++;
    push16(pc);
    push8(is_brk ? uint8_t(p | FLAG_B) : uint8_t(p & ~FLAG_B));
    set_flag(FLAG_I, true);
    // NMOS leaves D unchanged on interrupt (65C02 clears it).
    uint16_t vec = is_nmi ? 0xFFFA : 0xFFFE;
    pc = uint16_t(rb(vec)) | (uint16_t(rb(vec + 1)) << 8);
}

// ---- ALU / RMW -------------------------------------------------------

void Cpu::adc(uint8_t v) {
    uint8_t cin = flag(FLAG_C) ? 1 : 0;
    unsigned sum = unsigned(a) + v + cin;
    set_flag(FLAG_V, ((~(a ^ v)) & (a ^ sum) & 0x80) != 0);
    set_flag(FLAG_C, sum > 0xFF);
    uint8_t bin = uint8_t(sum);
    // NMOS: N/Z always from the binary sum, even in decimal mode.
    set_nz(bin);
    if (!flag(FLAG_D)) {
        a = bin;
        return;
    }
    unsigned lo = (a & 0x0F) + (v & 0x0F) + cin;
    if (lo > 9) lo += 6;
    unsigned hi = (a >> 4) + (v >> 4) + (lo > 0x0F ? 1 : 0);
    lo &= 0x0F;
    bool carry_out = hi > 9;
    if (carry_out) hi += 6;
    a = uint8_t(((hi & 0x0F) << 4) | lo);
    set_flag(FLAG_C, carry_out);
}

void Cpu::sbc(uint8_t v) {
    uint8_t cin = flag(FLAG_C) ? 1 : 0;
    int bin = int(a) - int(v) - (1 - cin);
    uint8_t bin_result = uint8_t(bin);
    set_flag(FLAG_V, ((a ^ v) & (a ^ bin_result) & 0x80) != 0);
    set_flag(FLAG_C, bin >= 0);
    set_nz(bin_result);
    if (!flag(FLAG_D)) {
        a = bin_result;
        return;
    }
    int lo = int(a & 0x0F) - int(v & 0x0F) - (1 - cin);
    int hi = int(a >> 4) - int(v >> 4);
    if (lo < 0) { lo -= 6; hi -= 1; }
    if (hi < 0) hi -= 6;
    a = uint8_t(((hi << 4) & 0xF0) | (lo & 0x0F));
}

void Cpu::cmp_(uint8_t reg, uint8_t v) {
    unsigned d = unsigned(reg) - v;
    set_flag(FLAG_C, reg >= v);
    set_nz(uint8_t(d));
}

void Cpu::bit(uint8_t v) {
    set_flag(FLAG_Z, (a & v) == 0);
    set_flag(FLAG_N, (v & 0x80) != 0);
    set_flag(FLAG_V, (v & 0x40) != 0);
}

uint8_t Cpu::asl_(uint8_t v) {
    set_flag(FLAG_C, (v & 0x80) != 0);
    v = uint8_t(v << 1);
    set_nz(v);
    return v;
}
uint8_t Cpu::lsr_(uint8_t v) {
    set_flag(FLAG_C, (v & 0x01) != 0);
    v = uint8_t(v >> 1);
    set_nz(v);
    return v;
}
uint8_t Cpu::rol_(uint8_t v) {
    bool c = flag(FLAG_C);
    set_flag(FLAG_C, (v & 0x80) != 0);
    v = uint8_t((v << 1) | (c ? 1 : 0));
    set_nz(v);
    return v;
}
uint8_t Cpu::ror_(uint8_t v) {
    bool c = flag(FLAG_C);
    set_flag(FLAG_C, (v & 0x01) != 0);
    v = uint8_t((v >> 1) | (c ? 0x80 : 0));
    set_nz(v);
    return v;
}

void Cpu::branch(bool cond, int &extra) {
    int8_t off = rel();
    if (!cond) return;
    uint16_t target = uint16_t(pc + off);
    extra += (target & 0xFF00) != (pc & 0xFF00) ? 2 : 1;
    pc = target;
}

void Cpu::slo(uint16_t ea) {
    uint8_t v = asl_(rb(ea));
    wb(ea, v);
    a |= v;
    set_nz(a);
}
void Cpu::rla(uint16_t ea) {
    uint8_t v = rol_(rb(ea));
    wb(ea, v);
    a &= v;
    set_nz(a);
}
void Cpu::sre(uint16_t ea) {
    uint8_t v = lsr_(rb(ea));
    wb(ea, v);
    a ^= v;
    set_nz(a);
}
void Cpu::rra(uint16_t ea) {
    uint8_t v = ror_(rb(ea));
    wb(ea, v);
    adc(v);
}
void Cpu::dcp(uint16_t ea) {
    uint8_t v = uint8_t(rb(ea) - 1);
    wb(ea, v);
    cmp_(a, v);
}
void Cpu::isc(uint16_t ea) {
    uint8_t v = uint8_t(rb(ea) + 1);
    wb(ea, v);
    sbc(v);
}
void Cpu::sax(uint16_t ea) { wb(ea, uint8_t(a & x)); }
void Cpu::lax(uint8_t v) {
    a = x = v;
    set_nz(a);
}

// ---- fetch/decode/execute --------------------------------------------

int Cpu::step() {
    if (jammed) {
        cycles += 1;
        return 1;
    }
    if (nmi_pending_) {
        nmi_pending_ = false;
        service_irq(/*is_nmi=*/true, /*is_brk=*/false);
        cycles += 7;
        return 7;
    }
    if (irq_line && !flag(FLAG_I)) {
        service_irq(/*is_nmi=*/false, /*is_brk=*/false);
        cycles += 7;
        return 7;
    }

    uint8_t op = fetch8();
    int c = 0;
    int extra = 0;
    bool crossed = false;

    switch (op) {
        // ---- Loads / stores ------------------------------------------
        case 0xA9: a = fetch8(); set_nz(a); c = 2; break;
        case 0xA5: a = rb(am_zp()); set_nz(a); c = 3; break;
        case 0xB5: a = rb(am_zpx()); set_nz(a); c = 4; break;
        case 0xAD: a = rb(am_abs()); set_nz(a); c = 4; break;
        case 0xBD: a = rb(am_absx(crossed)); set_nz(a); c = 4 + (crossed ? 1 : 0); break;
        case 0xB9: a = rb(am_absy(crossed)); set_nz(a); c = 4 + (crossed ? 1 : 0); break;
        case 0xA1: a = rb(am_indx()); set_nz(a); c = 6; break;
        case 0xB1: a = rb(am_indy(crossed)); set_nz(a); c = 5 + (crossed ? 1 : 0); break;

        case 0xA2: x = fetch8(); set_nz(x); c = 2; break;
        case 0xA6: x = rb(am_zp()); set_nz(x); c = 3; break;
        case 0xB6: x = rb(am_zpy()); set_nz(x); c = 4; break;
        case 0xAE: x = rb(am_abs()); set_nz(x); c = 4; break;
        case 0xBE: x = rb(am_absy(crossed)); set_nz(x); c = 4 + (crossed ? 1 : 0); break;

        case 0xA0: y = fetch8(); set_nz(y); c = 2; break;
        case 0xA4: y = rb(am_zp()); set_nz(y); c = 3; break;
        case 0xB4: y = rb(am_zpx()); set_nz(y); c = 4; break;
        case 0xAC: y = rb(am_abs()); set_nz(y); c = 4; break;
        case 0xBC: y = rb(am_absx(crossed)); set_nz(y); c = 4 + (crossed ? 1 : 0); break;

        case 0x85: wb(am_zp(), a); c = 3; break;
        case 0x95: wb(am_zpx(), a); c = 4; break;
        case 0x8D: wb(am_abs(), a); c = 4; break;
        case 0x9D: { bool cr; wb(am_absx(cr), a); } c = 5; break;
        case 0x99: { bool cr; wb(am_absy(cr), a); } c = 5; break;
        case 0x81: wb(am_indx(), a); c = 6; break;
        case 0x91: { bool cr; wb(am_indy(cr), a); } c = 6; break;

        case 0x86: wb(am_zp(), x); c = 3; break;
        case 0x96: wb(am_zpy(), x); c = 4; break;
        case 0x8E: wb(am_abs(), x); c = 4; break;

        case 0x84: wb(am_zp(), y); c = 3; break;
        case 0x94: wb(am_zpx(), y); c = 4; break;
        case 0x8C: wb(am_abs(), y); c = 4; break;

        // ---- Transfers / stack ---------------------------------------
        case 0xAA: x = a; set_nz(x); c = 2; break;
        case 0xA8: y = a; set_nz(y); c = 2; break;
        case 0x8A: a = x; set_nz(a); c = 2; break;
        case 0x98: a = y; set_nz(a); c = 2; break;
        case 0x9A: sp = x; c = 2; break;
        case 0xBA: x = sp; set_nz(x); c = 2; break;
        case 0x48: push8(a); c = 3; break;
        case 0x68: a = pop8(); set_nz(a); c = 4; break;
        case 0x08: push8(uint8_t(p | FLAG_B | FLAG_U)); c = 3; break;
        case 0x28: p = uint8_t((pop8() & ~FLAG_B) | FLAG_U); c = 4; break;

        // ---- ALU -----------------------------------------------------
        case 0x09: a |= fetch8(); set_nz(a); c = 2; break;
        case 0x05: a |= rb(am_zp()); set_nz(a); c = 3; break;
        case 0x15: a |= rb(am_zpx()); set_nz(a); c = 4; break;
        case 0x0D: a |= rb(am_abs()); set_nz(a); c = 4; break;
        case 0x1D: a |= rb(am_absx(crossed)); set_nz(a); c = 4 + (crossed ? 1 : 0); break;
        case 0x19: a |= rb(am_absy(crossed)); set_nz(a); c = 4 + (crossed ? 1 : 0); break;
        case 0x01: a |= rb(am_indx()); set_nz(a); c = 6; break;
        case 0x11: a |= rb(am_indy(crossed)); set_nz(a); c = 5 + (crossed ? 1 : 0); break;

        case 0x29: a &= fetch8(); set_nz(a); c = 2; break;
        case 0x25: a &= rb(am_zp()); set_nz(a); c = 3; break;
        case 0x35: a &= rb(am_zpx()); set_nz(a); c = 4; break;
        case 0x2D: a &= rb(am_abs()); set_nz(a); c = 4; break;
        case 0x3D: a &= rb(am_absx(crossed)); set_nz(a); c = 4 + (crossed ? 1 : 0); break;
        case 0x39: a &= rb(am_absy(crossed)); set_nz(a); c = 4 + (crossed ? 1 : 0); break;
        case 0x21: a &= rb(am_indx()); set_nz(a); c = 6; break;
        case 0x31: a &= rb(am_indy(crossed)); set_nz(a); c = 5 + (crossed ? 1 : 0); break;

        case 0x49: a ^= fetch8(); set_nz(a); c = 2; break;
        case 0x45: a ^= rb(am_zp()); set_nz(a); c = 3; break;
        case 0x55: a ^= rb(am_zpx()); set_nz(a); c = 4; break;
        case 0x4D: a ^= rb(am_abs()); set_nz(a); c = 4; break;
        case 0x5D: a ^= rb(am_absx(crossed)); set_nz(a); c = 4 + (crossed ? 1 : 0); break;
        case 0x59: a ^= rb(am_absy(crossed)); set_nz(a); c = 4 + (crossed ? 1 : 0); break;
        case 0x41: a ^= rb(am_indx()); set_nz(a); c = 6; break;
        case 0x51: a ^= rb(am_indy(crossed)); set_nz(a); c = 5 + (crossed ? 1 : 0); break;

        case 0x69: adc(fetch8()); c = 2; break;
        case 0x65: adc(rb(am_zp())); c = 3; break;
        case 0x75: adc(rb(am_zpx())); c = 4; break;
        case 0x6D: adc(rb(am_abs())); c = 4; break;
        case 0x7D: adc(rb(am_absx(crossed))); c = 4 + (crossed ? 1 : 0); break;
        case 0x79: adc(rb(am_absy(crossed))); c = 4 + (crossed ? 1 : 0); break;
        case 0x61: adc(rb(am_indx())); c = 6; break;
        case 0x71: adc(rb(am_indy(crossed))); c = 5 + (crossed ? 1 : 0); break;

        case 0xE9: case 0xEB: sbc(fetch8()); c = 2; break;  // $EB is undocumented SBC #imm
        case 0xE5: sbc(rb(am_zp())); c = 3; break;
        case 0xF5: sbc(rb(am_zpx())); c = 4; break;
        case 0xED: sbc(rb(am_abs())); c = 4; break;
        case 0xFD: sbc(rb(am_absx(crossed))); c = 4 + (crossed ? 1 : 0); break;
        case 0xF9: sbc(rb(am_absy(crossed))); c = 4 + (crossed ? 1 : 0); break;
        case 0xE1: sbc(rb(am_indx())); c = 6; break;
        case 0xF1: sbc(rb(am_indy(crossed))); c = 5 + (crossed ? 1 : 0); break;

        case 0xC9: cmp_(a, fetch8()); c = 2; break;
        case 0xC5: cmp_(a, rb(am_zp())); c = 3; break;
        case 0xD5: cmp_(a, rb(am_zpx())); c = 4; break;
        case 0xCD: cmp_(a, rb(am_abs())); c = 4; break;
        case 0xDD: cmp_(a, rb(am_absx(crossed))); c = 4 + (crossed ? 1 : 0); break;
        case 0xD9: cmp_(a, rb(am_absy(crossed))); c = 4 + (crossed ? 1 : 0); break;
        case 0xC1: cmp_(a, rb(am_indx())); c = 6; break;
        case 0xD1: cmp_(a, rb(am_indy(crossed))); c = 5 + (crossed ? 1 : 0); break;

        case 0xE0: cmp_(x, fetch8()); c = 2; break;
        case 0xE4: cmp_(x, rb(am_zp())); c = 3; break;
        case 0xEC: cmp_(x, rb(am_abs())); c = 4; break;
        case 0xC0: cmp_(y, fetch8()); c = 2; break;
        case 0xC4: cmp_(y, rb(am_zp())); c = 3; break;
        case 0xCC: cmp_(y, rb(am_abs())); c = 4; break;

        // ---- Inc / dec -----------------------------------------------
        case 0xE6: { uint16_t ea = am_zp(); uint8_t v = uint8_t(rb(ea) + 1); wb(ea, v); set_nz(v); } c = 5; break;
        case 0xF6: { uint16_t ea = am_zpx(); uint8_t v = uint8_t(rb(ea) + 1); wb(ea, v); set_nz(v); } c = 6; break;
        case 0xEE: { uint16_t ea = am_abs(); uint8_t v = uint8_t(rb(ea) + 1); wb(ea, v); set_nz(v); } c = 6; break;
        case 0xFE: { bool cr; uint16_t ea = am_absx(cr); uint8_t v = uint8_t(rb(ea) + 1); wb(ea, v); set_nz(v); } c = 7; break;
        case 0xC6: { uint16_t ea = am_zp(); uint8_t v = uint8_t(rb(ea) - 1); wb(ea, v); set_nz(v); } c = 5; break;
        case 0xD6: { uint16_t ea = am_zpx(); uint8_t v = uint8_t(rb(ea) - 1); wb(ea, v); set_nz(v); } c = 6; break;
        case 0xCE: { uint16_t ea = am_abs(); uint8_t v = uint8_t(rb(ea) - 1); wb(ea, v); set_nz(v); } c = 6; break;
        case 0xDE: { bool cr; uint16_t ea = am_absx(cr); uint8_t v = uint8_t(rb(ea) - 1); wb(ea, v); set_nz(v); } c = 7; break;
        case 0xE8: x++; set_nz(x); c = 2; break;
        case 0xC8: y++; set_nz(y); c = 2; break;
        case 0xCA: x--; set_nz(x); c = 2; break;
        case 0x88: y--; set_nz(y); c = 2; break;

        // ---- Shifts / rotates (NMOS RMW abs,X = 7) -------------------
        case 0x0A: a = asl_(a); c = 2; break;
        case 0x06: { uint16_t ea = am_zp(); wb(ea, asl_(rb(ea))); } c = 5; break;
        case 0x16: { uint16_t ea = am_zpx(); wb(ea, asl_(rb(ea))); } c = 6; break;
        case 0x0E: { uint16_t ea = am_abs(); wb(ea, asl_(rb(ea))); } c = 6; break;
        case 0x1E: { bool cr; uint16_t ea = am_absx(cr); wb(ea, asl_(rb(ea))); } c = 7; break;

        case 0x4A: a = lsr_(a); c = 2; break;
        case 0x46: { uint16_t ea = am_zp(); wb(ea, lsr_(rb(ea))); } c = 5; break;
        case 0x56: { uint16_t ea = am_zpx(); wb(ea, lsr_(rb(ea))); } c = 6; break;
        case 0x4E: { uint16_t ea = am_abs(); wb(ea, lsr_(rb(ea))); } c = 6; break;
        case 0x5E: { bool cr; uint16_t ea = am_absx(cr); wb(ea, lsr_(rb(ea))); } c = 7; break;

        case 0x2A: a = rol_(a); c = 2; break;
        case 0x26: { uint16_t ea = am_zp(); wb(ea, rol_(rb(ea))); } c = 5; break;
        case 0x36: { uint16_t ea = am_zpx(); wb(ea, rol_(rb(ea))); } c = 6; break;
        case 0x2E: { uint16_t ea = am_abs(); wb(ea, rol_(rb(ea))); } c = 6; break;
        case 0x3E: { bool cr; uint16_t ea = am_absx(cr); wb(ea, rol_(rb(ea))); } c = 7; break;

        case 0x6A: a = ror_(a); c = 2; break;
        case 0x66: { uint16_t ea = am_zp(); wb(ea, ror_(rb(ea))); } c = 5; break;
        case 0x76: { uint16_t ea = am_zpx(); wb(ea, ror_(rb(ea))); } c = 6; break;
        case 0x6E: { uint16_t ea = am_abs(); wb(ea, ror_(rb(ea))); } c = 6; break;
        case 0x7E: { bool cr; uint16_t ea = am_absx(cr); wb(ea, ror_(rb(ea))); } c = 7; break;

        case 0x24: bit(rb(am_zp())); c = 3; break;
        case 0x2C: bit(rb(am_abs())); c = 4; break;

        // ---- Branches ------------------------------------------------
        case 0x10: branch(!flag(FLAG_N), extra); c = 2; break;
        case 0x30: branch(flag(FLAG_N), extra); c = 2; break;
        case 0x50: branch(!flag(FLAG_V), extra); c = 2; break;
        case 0x70: branch(flag(FLAG_V), extra); c = 2; break;
        case 0x90: branch(!flag(FLAG_C), extra); c = 2; break;
        case 0xB0: branch(flag(FLAG_C), extra); c = 2; break;
        case 0xD0: branch(!flag(FLAG_Z), extra); c = 2; break;
        case 0xF0: branch(flag(FLAG_Z), extra); c = 2; break;

        // ---- Jumps / calls -------------------------------------------
        case 0x4C: pc = am_abs(); c = 3; break;
        case 0x6C: {
            // NMOS page-wrap: high byte from $xx00 when ptr ends in $FF.
            uint16_t ptr = fetch16();
            uint8_t lo = rb(ptr);
            uint8_t hi = rb((ptr & 0xFF00) | uint8_t(ptr + 1));
            pc = uint16_t(lo) | (uint16_t(hi) << 8);
            c = 5;
            break;
        }
        case 0x20: { uint16_t target = am_abs(); push16(uint16_t(pc - 1)); pc = target; } c = 6; break;
        case 0x60: pc = uint16_t(pop16() + 1); c = 6; break;
        case 0x00: service_irq(false, true); c = 7; break;
        case 0x40: {
            p = uint8_t((pop8() & ~FLAG_B) | FLAG_U);
            pc = pop16();
            c = 6;
            break;
        }

        // ---- Flags ---------------------------------------------------
        case 0x18: set_flag(FLAG_C, false); c = 2; break;
        case 0x38: set_flag(FLAG_C, true); c = 2; break;
        case 0x58: set_flag(FLAG_I, false); c = 2; break;
        case 0x78: set_flag(FLAG_I, true); c = 2; break;
        case 0xB8: set_flag(FLAG_V, false); c = 2; break;
        case 0xD8: set_flag(FLAG_D, false); c = 2; break;
        case 0xF8: set_flag(FLAG_D, true); c = 2; break;
        case 0xEA: c = 2; break;

        // ---- Undocumented: KIL / JAM ---------------------------------
        case 0x02: case 0x12: case 0x22: case 0x32:
        case 0x42: case 0x52: case 0x62: case 0x72:
        case 0x92: case 0xB2: case 0xD2: case 0xF2:
            jammed = true;
            c = 2;
            break;

        // ---- Undocumented: NOP variants ------------------------------
        case 0x1A: case 0x3A: case 0x5A: case 0x7A:
        case 0xDA: case 0xFA:
            c = 2; break;
        case 0x80: case 0x82: case 0xC2: case 0xE2:
        case 0x89:
            fetch8(); c = 2; break;
        case 0x04: case 0x44: case 0x64:
            fetch8(); c = 3; break;
        case 0x14: case 0x34: case 0x54: case 0x74:
        case 0xD4: case 0xF4:
            fetch8(); c = 4; break;
        case 0x0C:
            fetch16(); c = 4; break;
        case 0x1C: case 0x3C: case 0x5C: case 0x7C:
        case 0xDC: case 0xFC: {
            bool cr;
            (void)am_absx(cr);
            c = 4 + (cr ? 1 : 0);
            break;
        }

        // ---- Undocumented: SLO / ASL+ORA -----------------------------
        case 0x03: slo(am_indx()); c = 8; break;
        case 0x07: slo(am_zp()); c = 5; break;
        case 0x0F: slo(am_abs()); c = 6; break;
        case 0x13: { bool cr; slo(am_indy(cr)); } c = 8; break;
        case 0x17: slo(am_zpx()); c = 6; break;
        case 0x1B: { bool cr; slo(am_absy(cr)); } c = 7; break;
        case 0x1F: { bool cr; slo(am_absx(cr)); } c = 7; break;

        // ---- Undocumented: RLA / ROL+AND -----------------------------
        case 0x23: rla(am_indx()); c = 8; break;
        case 0x27: rla(am_zp()); c = 5; break;
        case 0x2F: rla(am_abs()); c = 6; break;
        case 0x33: { bool cr; rla(am_indy(cr)); } c = 8; break;
        case 0x37: rla(am_zpx()); c = 6; break;
        case 0x3B: { bool cr; rla(am_absy(cr)); } c = 7; break;
        case 0x3F: { bool cr; rla(am_absx(cr)); } c = 7; break;

        // ---- Undocumented: SRE / LSR+EOR -----------------------------
        case 0x43: sre(am_indx()); c = 8; break;
        case 0x47: sre(am_zp()); c = 5; break;
        case 0x4F: sre(am_abs()); c = 6; break;
        case 0x53: { bool cr; sre(am_indy(cr)); } c = 8; break;
        case 0x57: sre(am_zpx()); c = 6; break;
        case 0x5B: { bool cr; sre(am_absy(cr)); } c = 7; break;
        case 0x5F: { bool cr; sre(am_absx(cr)); } c = 7; break;

        // ---- Undocumented: RRA / ROR+ADC -----------------------------
        case 0x63: rra(am_indx()); c = 8; break;
        case 0x67: rra(am_zp()); c = 5; break;
        case 0x6F: rra(am_abs()); c = 6; break;
        case 0x73: { bool cr; rra(am_indy(cr)); } c = 8; break;
        case 0x77: rra(am_zpx()); c = 6; break;
        case 0x7B: { bool cr; rra(am_absy(cr)); } c = 7; break;
        case 0x7F: { bool cr; rra(am_absx(cr)); } c = 7; break;

        // ---- Undocumented: SAX / A&X store ---------------------------
        case 0x83: sax(am_indx()); c = 6; break;
        case 0x87: sax(am_zp()); c = 3; break;
        case 0x8F: sax(am_abs()); c = 4; break;
        case 0x97: sax(am_zpy()); c = 4; break;

        // ---- Undocumented: LAX ---------------------------------------
        case 0xA3: lax(rb(am_indx())); c = 6; break;
        case 0xA7: lax(rb(am_zp())); c = 3; break;
        case 0xAF: lax(rb(am_abs())); c = 4; break;
        case 0xB3: lax(rb(am_indy(crossed))); c = 5 + (crossed ? 1 : 0); break;
        case 0xB7: lax(rb(am_zpy())); c = 4; break;
        case 0xBF: lax(rb(am_absy(crossed))); c = 4 + (crossed ? 1 : 0); break;
        case 0xAB: {
            // Unstable LXA/LAX #imm. MAGIC=$EE is the common VIC-20 /
            // Nesdev approximation (bus fight A | MAGIC then AND imm).
            uint8_t imm = fetch8();
            lax(uint8_t((a | 0xEE) & imm));
            c = 2;
            break;
        }

        // ---- Undocumented: DCP / DEC+CMP -----------------------------
        case 0xC3: dcp(am_indx()); c = 8; break;
        case 0xC7: dcp(am_zp()); c = 5; break;
        case 0xCF: dcp(am_abs()); c = 6; break;
        case 0xD3: { bool cr; dcp(am_indy(cr)); } c = 8; break;
        case 0xD7: dcp(am_zpx()); c = 6; break;
        case 0xDB: { bool cr; dcp(am_absy(cr)); } c = 7; break;
        case 0xDF: { bool cr; dcp(am_absx(cr)); } c = 7; break;

        // ---- Undocumented: ISC / INC+SBC -----------------------------
        case 0xE3: isc(am_indx()); c = 8; break;
        case 0xE7: isc(am_zp()); c = 5; break;
        case 0xEF: isc(am_abs()); c = 6; break;
        case 0xF3: { bool cr; isc(am_indy(cr)); } c = 8; break;
        case 0xF7: isc(am_zpx()); c = 6; break;
        case 0xFB: { bool cr; isc(am_absy(cr)); } c = 7; break;
        case 0xFF: { bool cr; isc(am_absx(cr)); } c = 7; break;

        // ---- Undocumented: immediate hybrids -------------------------
        case 0x0B: case 0x2B: {
            // ANC: AND #imm, C = N
            a &= fetch8();
            set_nz(a);
            set_flag(FLAG_C, flag(FLAG_N));
            c = 2;
            break;
        }
        case 0x4B: {
            // ALR: AND #imm then LSR
            a &= fetch8();
            a = lsr_(a);
            c = 2;
            break;
        }
        case 0x6B: {
            // ARR: AND #imm then ROR; C/V from decimal-ish bit rules.
            a &= fetch8();
            a = ror_(a);
            set_flag(FLAG_C, (a & 0x40) != 0);
            set_flag(FLAG_V, (((a >> 6) ^ (a >> 5)) & 1) != 0);
            c = 2;
            break;
        }
        case 0xCB: {
            // AXS / SBX: X = (A & X) - imm, updates NZC
            uint8_t imm = fetch8();
            uint8_t t = uint8_t(a & x);
            set_flag(FLAG_C, t >= imm);
            x = uint8_t(t - imm);
            set_nz(x);
            c = 2;
            break;
        }
        case 0x8B: {
            // Unstable XAA/ANE. MAGIC=$FF is the common stable approximation.
            uint8_t imm = fetch8();
            a = uint8_t((a | 0xFF) & x & imm);
            set_nz(a);
            c = 2;
            break;
        }

        // ---- Undocumented: unstable high-byte AND stores -------------
        // SHY/SHX/AHX/TAS AND the value with (high_byte + 1) of the
        // effective address. Cite: Nesdev unofficial opcodes; VICE BUGS.
        case 0x9C: {
            bool cr;
            uint16_t base = fetch16();
            uint16_t addr = uint16_t(base + x);
            cr = (base & 0xFF00) != (addr & 0xFF00);
            uint8_t hi = uint8_t((base >> 8) + 1);
            uint8_t v = uint8_t(y & hi);
            if (cr) addr = (uint16_t(v) << 8) | (addr & 0xFF);
            wb(addr, v);
            c = 5;
            break;
        }
        case 0x9E: {
            bool cr;
            uint16_t base = fetch16();
            uint16_t addr = uint16_t(base + y);
            cr = (base & 0xFF00) != (addr & 0xFF00);
            uint8_t hi = uint8_t((base >> 8) + 1);
            uint8_t v = uint8_t(x & hi);
            if (cr) addr = (uint16_t(v) << 8) | (addr & 0xFF);
            wb(addr, v);
            c = 5;
            break;
        }
        case 0x9F: {
            bool cr;
            uint16_t base = fetch16();
            uint16_t addr = uint16_t(base + y);
            cr = (base & 0xFF00) != (addr & 0xFF00);
            uint8_t hi = uint8_t((base >> 8) + 1);
            uint8_t v = uint8_t(a & x & hi);
            if (cr) addr = (uint16_t(v) << 8) | (addr & 0xFF);
            wb(addr, v);
            c = 5;
            break;
        }
        case 0x93: {
            bool cr;
            uint16_t addr = am_indy(cr);
            uint8_t hi = uint8_t((addr >> 8) + 1);
            wb(addr, uint8_t(a & x & hi));
            c = 6;
            break;
        }
        case 0x9B: {
            bool cr;
            uint16_t base = fetch16();
            uint16_t addr = uint16_t(base + y);
            cr = (base & 0xFF00) != (addr & 0xFF00);
            sp = uint8_t(a & x);
            uint8_t hi = uint8_t((base >> 8) + 1);
            uint8_t v = uint8_t(sp & hi);
            if (cr) addr = (uint16_t(v) << 8) | (addr & 0xFF);
            wb(addr, v);
            c = 5;
            break;
        }
        case 0xBB: {
            // LAS: A,X,SP = mem & SP
            uint8_t v = uint8_t(rb(am_absy(crossed)) & sp);
            a = x = sp = v;
            set_nz(a);
            c = 4 + (crossed ? 1 : 0);
            break;
        }

        default:
            c = 2;
            break;
    }

    cycles += uint64_t(c + extra);
    return c + extra;
}

}  // namespace mos6502
