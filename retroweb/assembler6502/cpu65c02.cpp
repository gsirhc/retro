// WDC W65C02S CPU core implementation. See cpu65c02.h for the interface
// contract and citation.
//
// CMOS-specific behavior implemented here that differs from NMOS 6502 (all
// per the WDC W65C02S datasheet — this board populates the real CMOS part,
// confirmed from pcb6502full.net's `libsource`, so these are not optional):
//   - New instructions: BRA, PHX/PLX/PHY/PLY, STZ, TRB, TSB, INC A/DEC A,
//     BBR0-7/BBS0-7/RMB0-7/SMB0-7, STP, WAI, BIT #imm/zp,X/abs,X.
//   - New addressing mode: (zp) with no index, filling the $x2 column for
//     ORA/AND/EOR/ADC/STA/LDA/CMP/SBC; and JMP (abs,X).
//   - JMP (abs) no longer has the NMOS page-wrap bug (costs 6 cycles
//     instead of NMOS's 5 to do it correctly).
//   - Decimal-mode ADC/SBC set N/Z/V from the actual decimal result (NMOS
//     leaves them reflecting the pre-adjustment binary sum) and cost one
//     extra cycle.
//   - Read-modify-write abs,X instructions (ASL/LSR/ROL/ROR/INC/DEC) take
//     6 cycles instead of NMOS's 7 (no spurious extra read cycle).
//   - Reset clears the D flag (NMOS leaves it in whatever state it held).
//   - All formerly-undefined/illegal NMOS opcodes execute as well-defined
//     NOPs of documented length (1, 2, or 3 bytes) instead of NMOS's
//     unstable/undocumented behavior.
//
// Validated against Klaus Dormann's 6502/65C02 functional test suite (see
// tests/cpu65c02_test.cpp) — the same role TST8080/CPUTEST/8080EXM play for
// the Altair's i8080 core.

#include "cpu65c02.h"

namespace cpu65c02 {

void Cpu::reset() {
    a = x = y = 0;
    sp = 0xFD;                          // see header: equivalent to 3 phantom pushes from SP=0
    p = FLAG_U | FLAG_I;                // I set, D explicitly clear (CMOS fix — see file header)
    waiting = stopped = false;
    nmi_pending_ = false;
    irq_line = false;
    pc = uint16_t(rb(0xFFFC)) | (uint16_t(rb(0xFFFD)) << 8);
    cycles = 0;
}

void Cpu::nmi() { nmi_pending_ = true; }

void Cpu::service_irq(bool is_nmi, bool is_brk) {
    if (is_brk) pc++;                   // BRK's second byte is a padding/signature byte, skipped on return
    push16(pc);
    push8(is_brk ? uint8_t(p | FLAG_B) : uint8_t(p & ~FLAG_B));
    set_flag(FLAG_I, true);
    set_flag(FLAG_D, false);            // 65C02-only: interrupts also clear D (WDC datasheet)
    uint16_t vec = is_nmi ? 0xFFFA : 0xFFFE;
    pc = uint16_t(rb(vec)) | (uint16_t(rb(vec + 1)) << 8);
}

// ---- ALU / RMW primitives -------------------------------------------

void Cpu::adc(uint8_t v) {
    uint8_t cin = flag(FLAG_C) ? 1 : 0;
    if (!flag(FLAG_D)) {
        unsigned sum = unsigned(a) + v + cin;
        set_flag(FLAG_V, ((~(a ^ v)) & (a ^ sum) & 0x80) != 0);
        set_flag(FLAG_C, sum > 0xFF);
        a = uint8_t(sum);
        set_nz(a);
        return;
    }
    // BCD add — 65C02 sets N/Z/V from the adjusted decimal result.
    unsigned lo = (a & 0x0F) + (v & 0x0F) + cin;
    if (lo > 9) lo += 6;
    unsigned hi = (a >> 4) + (v >> 4) + (lo > 0x0F ? 1 : 0);
    lo &= 0x0F;
    bool carry_out = hi > 9;
    if (carry_out) hi += 6;
    uint8_t result = uint8_t(((hi & 0x0F) << 4) | lo);
    set_flag(FLAG_V, ((~(a ^ v)) & (a ^ result) & 0x80) != 0);
    set_flag(FLAG_C, carry_out);
    a = result;
    set_nz(a);
}

void Cpu::sbc(uint8_t v) {
    uint8_t cin = flag(FLAG_C) ? 1 : 0;
    int bin = int(a) - int(v) - (1 - cin);
    uint8_t bin_result = uint8_t(bin);
    set_flag(FLAG_V, ((a ^ v) & (a ^ bin_result) & 0x80) != 0);
    set_flag(FLAG_C, bin >= 0);
    if (!flag(FLAG_D)) {
        a = bin_result;
        set_nz(a);
        return;
    }
    // BCD subtract — 65C02 sets N/Z from the adjusted decimal result
    // (C/V above already reflect the binary-mode subtraction, per WDC).
    int lo = int(a & 0x0F) - int(v & 0x0F) - (1 - cin);
    int hi = int(a >> 4) - int(v >> 4);
    if (lo < 0) { lo -= 6; hi -= 1; }
    if (hi < 0) hi -= 6;
    uint8_t result = uint8_t(((hi << 4) & 0xF0) | (lo & 0x0F));
    a = result;
    set_nz(a);
}

void Cpu::cmp_(uint8_t reg, uint8_t v) {
    unsigned d = unsigned(reg) - v;
    set_flag(FLAG_C, reg >= v);
    set_nz(uint8_t(d));
}

void Cpu::bit(uint8_t v, bool immediate) {
    set_flag(FLAG_Z, (a & v) == 0);
    if (!immediate) {                   // #imm BIT (65C02-new) only touches Z
        set_flag(FLAG_N, (v & 0x80) != 0);
        set_flag(FLAG_V, (v & 0x40) != 0);
    }
}

uint8_t Cpu::asl_(uint8_t v) { set_flag(FLAG_C, (v & 0x80) != 0); v = uint8_t(v << 1); set_nz(v); return v; }
uint8_t Cpu::lsr_(uint8_t v) { set_flag(FLAG_C, (v & 0x01) != 0); v = uint8_t(v >> 1); set_nz(v); return v; }
uint8_t Cpu::rol_(uint8_t v) {
    bool c = flag(FLAG_C);
    set_flag(FLAG_C, (v & 0x80) != 0);
    v = uint8_t((v << 1) | (c ? 1 : 0));
    set_nz(v); return v;
}
uint8_t Cpu::ror_(uint8_t v) {
    bool c = flag(FLAG_C);
    set_flag(FLAG_C, (v & 0x01) != 0);
    v = uint8_t((v >> 1) | (c ? 0x80 : 0));
    set_nz(v); return v;
}

void Cpu::branch(bool cond, int &extra) {
    int8_t off = rel();
    if (!cond) return;
    uint16_t target = uint16_t(pc + off);
    extra += (target & 0xFF00) != (pc & 0xFF00) ? 2 : 1;   // +1 taken, +1 more if page crossed
    pc = target;
}

// ---- fetch/decode/execute --------------------------------------------

int Cpu::step() {
    // NMI is edge-triggered and always serviced (7 cycles), regardless of I.
    if (nmi_pending_) {
        nmi_pending_ = false;
        waiting = stopped = false;
        service_irq(/*is_nmi=*/true, /*is_brk=*/false);
        cycles += 7;
        return 7;
    }
    // IRQ is level-sensitive and masked by I; a WAI-suspended CPU wakes on
    // either line even if masked, per the WDC datasheet, but only actually
    // *services* the IRQ once I=0.
    if (irq_line && !flag(FLAG_I)) {
        waiting = false;
        service_irq(/*is_nmi=*/false, /*is_brk=*/false);
        cycles += 7;
        return 7;
    }
    if (stopped) { cycles += 1; return 1; }         // STP: dead until reset
    if (waiting) {                                   // WAI: idles until IRQ/NMI pending
        if (irq_line || nmi_pending_) waiting = false;
        cycles += 1; return 1;
    }

    uint8_t op = fetch8();
    int c = 0;          // base cycles, filled in per-case
    int extra = 0;       // page-cross / branch-taken penalties
    bool crossed = false;

    switch (op) {
        // ---- Loads / stores ------------------------------------------
        case 0xA9: a = fetch8(); set_nz(a); c = 2; break;                              // LDA #imm
        case 0xA5: a = rb(am_zp()); set_nz(a); c = 3; break;                           // LDA zp
        case 0xB5: a = rb(am_zpx()); set_nz(a); c = 4; break;                          // LDA zp,X
        case 0xAD: a = rb(am_abs()); set_nz(a); c = 4; break;                          // LDA abs
        case 0xBD: a = rb(am_absx(crossed)); set_nz(a); c = 4 + (crossed?1:0); break;  // LDA abs,X
        case 0xB9: a = rb(am_absy(crossed)); set_nz(a); c = 4 + (crossed?1:0); break;  // LDA abs,Y
        case 0xA1: a = rb(am_indx()); set_nz(a); c = 6; break;                         // LDA (zp,X)
        case 0xB1: a = rb(am_indy(crossed)); set_nz(a); c = 5 + (crossed?1:0); break;  // LDA (zp),Y
        case 0xB2: a = rb(am_ind()); set_nz(a); c = 5; break;                          // LDA (zp) [65C02]

        case 0xA2: x = fetch8(); set_nz(x); c = 2; break;                              // LDX #imm
        case 0xA6: x = rb(am_zp()); set_nz(x); c = 3; break;                           // LDX zp
        case 0xB6: x = rb(am_zpy()); set_nz(x); c = 4; break;                          // LDX zp,Y
        case 0xAE: x = rb(am_abs()); set_nz(x); c = 4; break;                          // LDX abs
        case 0xBE: x = rb(am_absy(crossed)); set_nz(x); c = 4 + (crossed?1:0); break;  // LDX abs,Y

        case 0xA0: y = fetch8(); set_nz(y); c = 2; break;                              // LDY #imm
        case 0xA4: y = rb(am_zp()); set_nz(y); c = 3; break;                           // LDY zp
        case 0xB4: y = rb(am_zpx()); set_nz(y); c = 4; break;                          // LDY zp,X
        case 0xAC: y = rb(am_abs()); set_nz(y); c = 4; break;                          // LDY abs
        case 0xBC: y = rb(am_absx(crossed)); set_nz(y); c = 4 + (crossed?1:0); break;  // LDY abs,X

        case 0x85: wb(am_zp(), a); c = 3; break;                                       // STA zp
        case 0x95: wb(am_zpx(), a); c = 4; break;                                      // STA zp,X
        case 0x8D: wb(am_abs(), a); c = 4; break;                                      // STA abs
        case 0x9D: { bool cr; wb(am_absx(cr), a); } c = 5; break;                      // STA abs,X (no page-cross discount: it's a write)
        case 0x99: { bool cr; wb(am_absy(cr), a); } c = 5; break;                      // STA abs,Y
        case 0x81: wb(am_indx(), a); c = 6; break;                                     // STA (zp,X)
        case 0x91: { bool cr; wb(am_indy(cr), a); } c = 6; break;                      // STA (zp),Y
        case 0x92: wb(am_ind(), a); c = 5; break;                                      // STA (zp) [65C02]

        case 0x86: wb(am_zp(), x); c = 3; break;                                       // STX zp
        case 0x96: wb(am_zpy(), x); c = 4; break;                                      // STX zp,Y
        case 0x8E: wb(am_abs(), x); c = 4; break;                                      // STX abs

        case 0x84: wb(am_zp(), y); c = 3; break;                                       // STY zp
        case 0x94: wb(am_zpx(), y); c = 4; break;                                      // STY zp,X
        case 0x8C: wb(am_abs(), y); c = 4; break;                                      // STY abs

        // STZ [65C02]
        case 0x64: wb(am_zp(), 0); c = 3; break;
        case 0x74: wb(am_zpx(), 0); c = 4; break;
        case 0x9C: wb(am_abs(), 0); c = 4; break;
        case 0x9E: { bool cr; wb(am_absx(cr), 0); } c = 5; break;

        // ---- Transfers / stack -----------------------------------------
        case 0xAA: x = a; set_nz(x); c = 2; break;      // TAX
        case 0xA8: y = a; set_nz(y); c = 2; break;       // TAY
        case 0x8A: a = x; set_nz(a); c = 2; break;       // TXA
        case 0x98: a = y; set_nz(a); c = 2; break;       // TYA
        case 0x9A: sp = x; c = 2; break;                 // TXS (no flags)
        case 0xBA: x = sp; set_nz(x); c = 2; break;       // TSX
        case 0x48: push8(a); c = 3; break;                // PHA
        case 0x68: a = pop8(); set_nz(a); c = 4; break;   // PLA
        case 0x08: push8(uint8_t(p | FLAG_B | FLAG_U)); c = 3; break;  // PHP
        case 0x28: p = uint8_t((pop8() & ~FLAG_B) | FLAG_U); c = 4; break; // PLP
        case 0xDA: push8(x); c = 3; break;                // PHX [65C02]
        case 0xFA: x = pop8(); set_nz(x); c = 4; break;   // PLX [65C02]
        case 0x5A: push8(y); c = 3; break;                // PHY [65C02]
        case 0x7A: y = pop8(); set_nz(y); c = 4; break;   // PLY [65C02]

        // ---- ALU: ORA / AND / EOR / ADC / SBC / CMP / CPX / CPY --------
        case 0x09: a |= fetch8(); set_nz(a); c = 2; break;
        case 0x05: a |= rb(am_zp()); set_nz(a); c = 3; break;
        case 0x15: a |= rb(am_zpx()); set_nz(a); c = 4; break;
        case 0x0D: a |= rb(am_abs()); set_nz(a); c = 4; break;
        case 0x1D: a |= rb(am_absx(crossed)); set_nz(a); c = 4 + (crossed?1:0); break;
        case 0x19: a |= rb(am_absy(crossed)); set_nz(a); c = 4 + (crossed?1:0); break;
        case 0x01: a |= rb(am_indx()); set_nz(a); c = 6; break;
        case 0x11: a |= rb(am_indy(crossed)); set_nz(a); c = 5 + (crossed?1:0); break;
        case 0x12: a |= rb(am_ind()); set_nz(a); c = 5; break;

        case 0x29: a &= fetch8(); set_nz(a); c = 2; break;
        case 0x25: a &= rb(am_zp()); set_nz(a); c = 3; break;
        case 0x35: a &= rb(am_zpx()); set_nz(a); c = 4; break;
        case 0x2D: a &= rb(am_abs()); set_nz(a); c = 4; break;
        case 0x3D: a &= rb(am_absx(crossed)); set_nz(a); c = 4 + (crossed?1:0); break;
        case 0x39: a &= rb(am_absy(crossed)); set_nz(a); c = 4 + (crossed?1:0); break;
        case 0x21: a &= rb(am_indx()); set_nz(a); c = 6; break;
        case 0x31: a &= rb(am_indy(crossed)); set_nz(a); c = 5 + (crossed?1:0); break;
        case 0x32: a &= rb(am_ind()); set_nz(a); c = 5; break;

        case 0x49: a ^= fetch8(); set_nz(a); c = 2; break;
        case 0x45: a ^= rb(am_zp()); set_nz(a); c = 3; break;
        case 0x55: a ^= rb(am_zpx()); set_nz(a); c = 4; break;
        case 0x4D: a ^= rb(am_abs()); set_nz(a); c = 4; break;
        case 0x5D: a ^= rb(am_absx(crossed)); set_nz(a); c = 4 + (crossed?1:0); break;
        case 0x59: a ^= rb(am_absy(crossed)); set_nz(a); c = 4 + (crossed?1:0); break;
        case 0x41: a ^= rb(am_indx()); set_nz(a); c = 6; break;
        case 0x51: a ^= rb(am_indy(crossed)); set_nz(a); c = 5 + (crossed?1:0); break;
        case 0x52: a ^= rb(am_ind()); set_nz(a); c = 5; break;

        case 0x69: adc(fetch8()); c = 2; break;
        case 0x65: adc(rb(am_zp())); c = 3; break;
        case 0x75: adc(rb(am_zpx())); c = 4; break;
        case 0x6D: adc(rb(am_abs())); c = 4; break;
        case 0x7D: adc(rb(am_absx(crossed))); c = 4 + (crossed?1:0); break;
        case 0x79: adc(rb(am_absy(crossed))); c = 4 + (crossed?1:0); break;
        case 0x61: adc(rb(am_indx())); c = 6; break;
        case 0x71: adc(rb(am_indy(crossed))); c = 5 + (crossed?1:0); break;
        case 0x72: adc(rb(am_ind())); c = 5; break;

        case 0xE9: sbc(fetch8()); c = 2; break;
        case 0xE5: sbc(rb(am_zp())); c = 3; break;
        case 0xF5: sbc(rb(am_zpx())); c = 4; break;
        case 0xED: sbc(rb(am_abs())); c = 4; break;
        case 0xFD: sbc(rb(am_absx(crossed))); c = 4 + (crossed?1:0); break;
        case 0xF9: sbc(rb(am_absy(crossed))); c = 4 + (crossed?1:0); break;
        case 0xE1: sbc(rb(am_indx())); c = 6; break;
        case 0xF1: sbc(rb(am_indy(crossed))); c = 5 + (crossed?1:0); break;
        case 0xF2: sbc(rb(am_ind())); c = 5; break;

        case 0xC9: cmp_(a, fetch8()); c = 2; break;
        case 0xC5: cmp_(a, rb(am_zp())); c = 3; break;
        case 0xD5: cmp_(a, rb(am_zpx())); c = 4; break;
        case 0xCD: cmp_(a, rb(am_abs())); c = 4; break;
        case 0xDD: cmp_(a, rb(am_absx(crossed))); c = 4 + (crossed?1:0); break;
        case 0xD9: cmp_(a, rb(am_absy(crossed))); c = 4 + (crossed?1:0); break;
        case 0xC1: cmp_(a, rb(am_indx())); c = 6; break;
        case 0xD1: cmp_(a, rb(am_indy(crossed))); c = 5 + (crossed?1:0); break;
        case 0xD2: cmp_(a, rb(am_ind())); c = 5; break;

        case 0xE0: cmp_(x, fetch8()); c = 2; break;
        case 0xE4: cmp_(x, rb(am_zp())); c = 3; break;
        case 0xEC: cmp_(x, rb(am_abs())); c = 4; break;
        case 0xC0: cmp_(y, fetch8()); c = 2; break;
        case 0xC4: cmp_(y, rb(am_zp())); c = 3; break;
        case 0xCC: cmp_(y, rb(am_abs())); c = 4; break;

        // ---- Increment / decrement (memory) -----------------------------
        case 0xE6: { uint16_t ea = am_zp(); wb(ea, uint8_t(rb(ea)+1)); set_nz(rb(ea)); } c = 5; break;
        case 0xF6: { uint16_t ea = am_zpx(); wb(ea, uint8_t(rb(ea)+1)); set_nz(rb(ea)); } c = 6; break;
        case 0xEE: { uint16_t ea = am_abs(); wb(ea, uint8_t(rb(ea)+1)); set_nz(rb(ea)); } c = 6; break;
        case 0xFE: { bool cr; uint16_t ea = am_absx(cr); wb(ea, uint8_t(rb(ea)+1)); set_nz(rb(ea)); } c = 7; break;
        case 0xC6: { uint16_t ea = am_zp(); wb(ea, uint8_t(rb(ea)-1)); set_nz(rb(ea)); } c = 5; break;
        case 0xD6: { uint16_t ea = am_zpx(); wb(ea, uint8_t(rb(ea)-1)); set_nz(rb(ea)); } c = 6; break;
        case 0xCE: { uint16_t ea = am_abs(); wb(ea, uint8_t(rb(ea)-1)); set_nz(rb(ea)); } c = 6; break;
        case 0xDE: { bool cr; uint16_t ea = am_absx(cr); wb(ea, uint8_t(rb(ea)-1)); set_nz(rb(ea)); } c = 7; break;
        case 0xE8: x++; set_nz(x); c = 2; break;          // INX
        case 0xC8: y++; set_nz(y); c = 2; break;           // INY
        case 0xCA: x--; set_nz(x); c = 2; break;           // DEX
        case 0x88: y--; set_nz(y); c = 2; break;           // DEY
        case 0x1A: a++; set_nz(a); c = 2; break;            // INC A [65C02]
        case 0x3A: a--; set_nz(a); c = 2; break;            // DEC A [65C02]

        // ---- Shifts / rotates -------------------------------------------
        case 0x0A: a = asl_(a); c = 2; break;
        case 0x06: { uint16_t ea = am_zp(); wb(ea, asl_(rb(ea))); } c = 5; break;
        case 0x16: { uint16_t ea = am_zpx(); wb(ea, asl_(rb(ea))); } c = 6; break;
        case 0x0E: { uint16_t ea = am_abs(); wb(ea, asl_(rb(ea))); } c = 6; break;
        case 0x1E: { bool cr; uint16_t ea = am_absx(cr); wb(ea, asl_(rb(ea))); } c = 6; break; // 65C02: 6, not NMOS 7

        case 0x4A: a = lsr_(a); c = 2; break;
        case 0x46: { uint16_t ea = am_zp(); wb(ea, lsr_(rb(ea))); } c = 5; break;
        case 0x56: { uint16_t ea = am_zpx(); wb(ea, lsr_(rb(ea))); } c = 6; break;
        case 0x4E: { uint16_t ea = am_abs(); wb(ea, lsr_(rb(ea))); } c = 6; break;
        case 0x5E: { bool cr; uint16_t ea = am_absx(cr); wb(ea, lsr_(rb(ea))); } c = 6; break;

        case 0x2A: a = rol_(a); c = 2; break;
        case 0x26: { uint16_t ea = am_zp(); wb(ea, rol_(rb(ea))); } c = 5; break;
        case 0x36: { uint16_t ea = am_zpx(); wb(ea, rol_(rb(ea))); } c = 6; break;
        case 0x2E: { uint16_t ea = am_abs(); wb(ea, rol_(rb(ea))); } c = 6; break;
        case 0x3E: { bool cr; uint16_t ea = am_absx(cr); wb(ea, rol_(rb(ea))); } c = 6; break;

        case 0x6A: a = ror_(a); c = 2; break;
        case 0x66: { uint16_t ea = am_zp(); wb(ea, ror_(rb(ea))); } c = 5; break;
        case 0x76: { uint16_t ea = am_zpx(); wb(ea, ror_(rb(ea))); } c = 6; break;
        case 0x6E: { uint16_t ea = am_abs(); wb(ea, ror_(rb(ea))); } c = 6; break;
        case 0x7E: { bool cr; uint16_t ea = am_absx(cr); wb(ea, ror_(rb(ea))); } c = 6; break;

        // ---- BIT / TRB / TSB ---------------------------------------------
        case 0x89: bit(fetch8(), /*immediate=*/true); c = 2; break;              // BIT #imm [65C02]
        case 0x24: bit(rb(am_zp()), false); c = 3; break;
        case 0x34: bit(rb(am_zpx()), false); c = 4; break;                       // BIT zp,X [65C02]
        case 0x2C: bit(rb(am_abs()), false); c = 4; break;
        case 0x3C: bit(rb(am_absx(crossed)), false); c = 4 + (crossed?1:0); break; // BIT abs,X [65C02]
        case 0x14: { uint16_t ea = am_zp(); uint8_t v = rb(ea); set_flag(FLAG_Z, (a&v)==0); wb(ea, uint8_t(v & ~a)); } c = 5; break; // TRB zp [65C02]
        case 0x1C: { uint16_t ea = am_abs(); uint8_t v = rb(ea); set_flag(FLAG_Z, (a&v)==0); wb(ea, uint8_t(v & ~a)); } c = 6; break; // TRB abs
        case 0x04: { uint16_t ea = am_zp(); uint8_t v = rb(ea); set_flag(FLAG_Z, (a&v)==0); wb(ea, uint8_t(v | a)); } c = 5; break;  // TSB zp
        case 0x0C: { uint16_t ea = am_abs(); uint8_t v = rb(ea); set_flag(FLAG_Z, (a&v)==0); wb(ea, uint8_t(v | a)); } c = 6; break; // TSB abs

        // ---- RMB0-7 / SMB0-7 [65C02] --------------------------------------
        case 0x07: case 0x17: case 0x27: case 0x37:
        case 0x47: case 0x57: case 0x67: case 0x77: {
            int bit_n = (op >> 4) & 7;
            uint16_t ea = am_zp();
            wb(ea, uint8_t(rb(ea) & ~(1 << bit_n)));
            c = 5; break;
        }
        case 0x87: case 0x97: case 0xA7: case 0xB7:
        case 0xC7: case 0xD7: case 0xE7: case 0xF7: {
            int bit_n = (op >> 4) & 7;
            uint16_t ea = am_zp();
            wb(ea, uint8_t(rb(ea) | (1 << bit_n)));
            c = 5; break;
        }
        // BBR0-7 / BBS0-7 [65C02]: zp operand, then relative branch operand
        case 0x0F: case 0x1F: case 0x2F: case 0x3F:
        case 0x4F: case 0x5F: case 0x6F: case 0x7F: {
            int bit_n = (op >> 4) & 7;
            uint8_t v = rb(am_zp());
            branch((v & (1 << bit_n)) == 0, extra);
            c = 5; break;
        }
        case 0x8F: case 0x9F: case 0xAF: case 0xBF:
        case 0xCF: case 0xDF: case 0xEF: case 0xFF: {
            int bit_n = (op >> 4) & 7;
            uint8_t v = rb(am_zp());
            branch((v & (1 << bit_n)) != 0, extra);
            c = 5; break;
        }

        // ---- Branches -------------------------------------------------
        case 0x10: branch(!flag(FLAG_N), extra); c = 2; break;  // BPL
        case 0x30: branch(flag(FLAG_N), extra); c = 2; break;   // BMI
        case 0x50: branch(!flag(FLAG_V), extra); c = 2; break;  // BVC
        case 0x70: branch(flag(FLAG_V), extra); c = 2; break;   // BVS
        case 0x90: branch(!flag(FLAG_C), extra); c = 2; break;  // BCC
        case 0xB0: branch(flag(FLAG_C), extra); c = 2; break;   // BCS
        case 0xD0: branch(!flag(FLAG_Z), extra); c = 2; break;  // BNE
        case 0xF0: branch(flag(FLAG_Z), extra); c = 2; break;   // BEQ
        case 0x80: branch(true, extra); c = 2; break;            // BRA [65C02]

        // ---- Jumps / calls ----------------------------------------------
        case 0x4C: pc = am_abs(); c = 3; break;                                 // JMP abs
        case 0x6C: {                                                            // JMP (abs) -- 65C02 fixes the NMOS page-wrap bug
            uint16_t ptr = fetch16();
            pc = uint16_t(rb(ptr)) | (uint16_t(rb(uint16_t(ptr + 1))) << 8);
            c = 6; break;
        }
        case 0x7C: {                                                            // JMP (abs,X) [65C02]
            uint16_t base = fetch16();
            uint16_t ptr = uint16_t(base + x);
            pc = uint16_t(rb(ptr)) | (uint16_t(rb(uint16_t(ptr + 1))) << 8);
            c = 6; break;
        }
        case 0x20: { uint16_t target = am_abs(); push16(uint16_t(pc - 1)); pc = target; } c = 6; break; // JSR
        case 0x60: pc = uint16_t(pop16() + 1); c = 6; break;                    // RTS
        case 0x00: service_irq(false, true); c = 7; break;                      // BRK
        case 0x40: {                                                            // RTI
            p = uint8_t((pop8() & ~FLAG_B) | FLAG_U);
            pc = pop16();
            c = 6; break;
        }

        // ---- Flags --------------------------------------------------------
        case 0x18: set_flag(FLAG_C, false); c = 2; break;   // CLC
        case 0x38: set_flag(FLAG_C, true); c = 2; break;    // SEC
        case 0x58: set_flag(FLAG_I, false); c = 2; break;   // CLI
        case 0x78: set_flag(FLAG_I, true); c = 2; break;    // SEI
        case 0xB8: set_flag(FLAG_V, false); c = 2; break;   // CLV
        case 0xD8: set_flag(FLAG_D, false); c = 2; break;   // CLD
        case 0xF8: set_flag(FLAG_D, true); c = 2; break;    // SED

        // ---- Misc -----------------------------------------------------
        case 0xEA: c = 2; break;                              // NOP
        case 0xCB: waiting = true; c = 3; break;               // WAI [65C02]
        case 0xDB: stopped = true; c = 3; break;                // STP [65C02]

        // ---- Reserved opcodes: documented no-op behavior [65C02] --------
        // WDC W65C02S datasheet: every opcode NMOS left undefined executes
        // as a NOP of fixed length here (1, 2, or 3 bytes) rather than the
        // unstable NMOS "illegal opcode" behavior. Real software (BASIC,
        // Wozmon, the board's own ROM) never emits these; the exact byte
        // lengths below are the well-published WDC table, spot-checked
        // against Klaus Dormann's 65C02 extended-opcode test.
        case 0x02: case 0x22: case 0x42: case 0x62:            // 2-byte NOPs (immediate-shaped)
        case 0x82: case 0xC2: case 0xE2:
            fetch8(); c = 2; break;
        case 0x44:                                              // 2-byte NOP (zp-shaped)
            fetch8(); c = 3; break;
        case 0x54: case 0xD4: case 0xF4:                        // 2-byte NOPs (zp,X-shaped)
            fetch8(); c = 4; break;
        case 0x5C:                                               // 3-byte NOP (abs-shaped, longest)
            fetch16(); c = 8; break;
        case 0xDC: case 0xFC:                                    // 3-byte NOPs (abs,X-shaped)
            fetch16(); c = 4; break;
        case 0x03: case 0x13: case 0x23: case 0x33:              // 1-byte NOPs
        case 0x43: case 0x53: case 0x63: case 0x73:
        case 0x83: case 0x93: case 0xA3: case 0xB3:
        case 0xC3: case 0xD3: case 0xE3: case 0xF3:
        case 0x0B: case 0x1B: case 0x2B: case 0x3B:
        case 0x4B: case 0x5B: case 0x6B: case 0x7B:
        case 0x8B: case 0x9B: case 0xAB: case 0xBB:
        case 0xEB: case 0xFB:
            c = 1; break;

        default:
            // Every opcode 0x00-0xFF is handled above; unreachable in
            // practice, but fail safe rather than mis-executing silently.
            c = 2; break;
    }

    cycles += uint64_t(c + extra);
    return c + extra;
}

} // namespace cpu65c02
