// Intel 80286 CPU core, real-address-mode only -- implementation.
//
// Flag/timing semantics are cited from the Intel iAPX 286 Programmer's
// Reference Manual (1987) throughout; where the 286 behaves identically to
// the 8086 the Intel 8086/8088 User's Manual is the reference instead.
// Deliberately preserved quirks (not "bugs" -- real, documented silicon
// behavior worth keeping even though it's surprising):
//   - PUSH SP pushes the *decremented* SP value on the 286, unlike the 8086
//     (which pushes the pre-decrement value). See push_reg() below.
//   - Shift/rotate counts are masked mod 32 on the 286 (the 8086 used the
//     full unmasked count, making a shift by e.g. 200 take 200 cycles).
//     See shiftrot8/16.
//   - OF after a multi-bit shift/rotate (count != 1) is left *undefined* by
//     Intel's own documentation; this core simply leaves the flag bit
//     untouched in that case rather than guessing, which is itself the
//     documented contract, not a gap.
#include "cpu80286.h"

namespace cpu80286 {

void Cpu::reset() {
    ax = bx = cx = dx = sp = bp = si = di = 0;
    ds = es = ss = 0;
    // Real 80286 RESET vector: CS:IP = F000:FFF0, with CS's base forced to
    // 0xFF0000 for this one load only so the physical fetch address is
    // 0xFFFFF0 -- the top of the 16MB space, aliased down to the BIOS's
    // F0000-FFFFF ROM window so POST can run before any far jump reloads CS
    // normally. This core doesn't model the hidden base/limit descriptor
    // cache (no protected mode support at all), so it approximates the same
    // observable effect the simple way real-mode-only cores do: start CS at
    // 0xF000, IP at 0xFFF0, giving physical FFFF0 directly -- one hex digit
    // short of the genuine 286's aliased FFFFF0, but equivalent for a BIOS
    // that (like every real AT BIOS) immediately far-jumps to a normal
    // F000:xxxx entry point anyway. Noted as a documented simplification in
    // IBM_PCAT_REVIEW.md.
    cs = 0xF000;
    ip = 0xFFF0;
    flags = FLAG_R1;
    halted = false;
    cycles = 0;
    seg_override_ = -1;
    rep_ = REP_NONE;
    opsize32_ = false;
    extra_cycles_ = 0;
}

int Cpu::interrupt(uint8_t vector) {
    halted = false;
    push16(flags);
    push16(cs);
    push16(ip);
    set_flag(FLAG_IF, false);
    set_flag(FLAG_TF, false);
    uint32_t vec = uint32_t(vector) * 4;
    uint16_t new_ip = bus_.read(vec) | (uint16_t(bus_.read(vec + 1)) << 8);
    uint16_t new_cs = bus_.read(vec + 2) | (uint16_t(bus_.read(vec + 3)) << 8);
    ip = new_ip;
    cs = new_cs;
    cycles += 45;  // iAPX 286 timing appendix: INT (real mode) = 45 cycles
    return 45;
}

// --- register file ----------------------------------------------------

uint16_t Cpu::get_reg16(int idx) const { return uint16_t(get_reg32(idx)); }
void Cpu::set_reg16(int idx, uint16_t v) {
    // Writing a 16-bit sub-register never disturbs the upper 16 bits of the
    // full register -- real hardware behavior once EAX/etc. exist at all.
    switch (idx & 7) {
        case 0: ax = (ax & 0xFFFF0000u) | v; break;
        case 1: cx = (cx & 0xFFFF0000u) | v; break;
        case 2: dx = (dx & 0xFFFF0000u) | v; break;
        case 3: bx = (bx & 0xFFFF0000u) | v; break;
        case 4: sp = (sp & 0xFFFF0000u) | v; break;
        case 5: bp = (bp & 0xFFFF0000u) | v; break;
        case 6: si = (si & 0xFFFF0000u) | v; break;
        default: di = (di & 0xFFFF0000u) | v; break;
    }
}
uint32_t Cpu::get_reg32(int idx) const {
    switch (idx & 7) {
        case 0: return ax;
        case 1: return cx;
        case 2: return dx;
        case 3: return bx;
        case 4: return sp;
        case 5: return bp;
        case 6: return si;
        default: return di;
    }
}
void Cpu::set_reg32(int idx, uint32_t v) {
    switch (idx & 7) {
        case 0: ax = v; break;
        case 1: cx = v; break;
        case 2: dx = v; break;
        case 3: bx = v; break;
        case 4: sp = v; break;
        case 5: bp = v; break;
        case 6: si = v; break;
        default: di = v; break;
    }
}
uint8_t Cpu::get_reg8(int idx) const {
    switch (idx & 7) {
        case 0: return uint8_t(ax);
        case 1: return uint8_t(cx);
        case 2: return uint8_t(dx);
        case 3: return uint8_t(bx);
        case 4: return uint8_t(ax >> 8);
        case 5: return uint8_t(cx >> 8);
        case 6: return uint8_t(dx >> 8);
        default: return uint8_t(bx >> 8);
    }
}
void Cpu::set_reg8(int idx, uint8_t v) {
    // As with set_reg16, only the addressed byte changes -- bits 8-31 (or
    // 16-31 for AH/CH/DH/BH) are left alone.
    switch (idx & 7) {
        case 0: ax = (ax & 0xFFFFFF00u) | v; break;
        case 1: cx = (cx & 0xFFFFFF00u) | v; break;
        case 2: dx = (dx & 0xFFFFFF00u) | v; break;
        case 3: bx = (bx & 0xFFFFFF00u) | v; break;
        case 4: ax = (ax & 0xFFFF00FFu) | (uint32_t(v) << 8); break;
        case 5: cx = (cx & 0xFFFF00FFu) | (uint32_t(v) << 8); break;
        case 6: dx = (dx & 0xFFFF00FFu) | (uint32_t(v) << 8); break;
        default: bx = (bx & 0xFFFF00FFu) | (uint32_t(v) << 8); break;
    }
}

uint16_t &Cpu::seg_reg(int idx) {
    switch (idx & 3) {
        case SEG_ES: return es;
        case SEG_CS: return cs;
        case SEG_SS: return ss;
        default: return ds;
    }
}

// --- ModR/M decode ------------------------------------------------------

Cpu::RM Cpu::decode_modrm() {
    uint8_t modrm = fetch8();
    int mod = (modrm >> 6) & 3;
    int reg = (modrm >> 3) & 7;
    int rm = modrm & 7;
    last_reg_ = reg;

    RM out{};
    if (mod == 3) {
        out.is_mem = false;
        out.reg = rm;
        return out;
    }

    out.is_mem = true;
    uint16_t addr = 0;
    bool uses_bp = false;
    bool has_disp_only = false;  // mod==0, rm==6: disp16 with no base register at all
    switch (rm) {
        case 0: addr = uint16_t(bx + si); break;
        case 1: addr = uint16_t(bx + di); break;
        case 2: addr = uint16_t(bp + si); uses_bp = true; break;
        case 3: addr = uint16_t(bp + di); uses_bp = true; break;
        case 4: addr = si; break;
        case 5: addr = di; break;
        case 6:
            if (mod == 0) { addr = fetch16(); has_disp_only = true; }
            else { addr = bp; uses_bp = true; }
            break;
        default: addr = bx; break;  // rm == 7
    }
    if (!has_disp_only) {
        if (mod == 1) {
            int8_t d = int8_t(fetch8());
            addr = uint16_t(addr + int16_t(d));
        } else if (mod == 2) {
            uint16_t d = fetch16();
            addr = uint16_t(addr + d);
        }
    }
    uint16_t seg = uses_bp ? ss : ds;
    if (seg_override_ >= 0) seg = seg_reg(seg_override_);
    out.seg = seg;
    out.off = addr;
    return out;
}

// --- flags --------------------------------------------------------------

bool Cpu::parity_even(uint8_t v) {
    v = uint8_t(v ^ (v >> 4));
    v = uint8_t(v ^ (v >> 2));
    v = uint8_t(v ^ (v >> 1));
    return !(v & 1);
}
void Cpu::set_pzs8(uint8_t r) {
    set_flag(FLAG_ZF, r == 0);
    set_flag(FLAG_SF, (r & 0x80) != 0);
    set_flag(FLAG_PF, parity_even(r));
}
void Cpu::set_pzs16(uint16_t r) {
    set_flag(FLAG_ZF, r == 0);
    set_flag(FLAG_SF, (r & 0x8000) != 0);
    set_flag(FLAG_PF, parity_even(uint8_t(r & 0xFF)));
}
void Cpu::set_pzs32(uint32_t r) {
    set_flag(FLAG_ZF, r == 0);
    set_flag(FLAG_SF, (r & 0x80000000u) != 0);
    set_flag(FLAG_PF, parity_even(uint8_t(r & 0xFF)));
}

// --- ALU primitives -------------------------------------------------------

uint8_t Cpu::add8(uint8_t a, uint8_t b, bool carry_in) {
    unsigned r = unsigned(a) + unsigned(b) + (carry_in ? 1u : 0u);
    uint8_t res = uint8_t(r);
    set_flag(FLAG_CF, r > 0xFF);
    set_flag(FLAG_AF, ((a ^ b ^ res) & 0x10) != 0);
    set_flag(FLAG_OF, ((~(a ^ b)) & (a ^ res) & 0x80) != 0);
    set_pzs8(res);
    return res;
}
uint16_t Cpu::add16(uint16_t a, uint16_t b, bool carry_in) {
    unsigned r = unsigned(a) + unsigned(b) + (carry_in ? 1u : 0u);
    uint16_t res = uint16_t(r);
    set_flag(FLAG_CF, r > 0xFFFF);
    set_flag(FLAG_AF, ((a ^ b ^ res) & 0x10) != 0);
    set_flag(FLAG_OF, ((~(a ^ b)) & (a ^ res) & 0x8000) != 0);
    set_pzs16(res);
    return res;
}
uint8_t Cpu::sub8(uint8_t a, uint8_t b, bool borrow_in) {
    unsigned bb = unsigned(b) + (borrow_in ? 1u : 0u);
    uint8_t res = uint8_t(unsigned(a) - bb);
    set_flag(FLAG_CF, unsigned(a) < bb);
    set_flag(FLAG_AF, ((a ^ b ^ res) & 0x10) != 0);
    set_flag(FLAG_OF, ((a ^ b) & (a ^ res) & 0x80) != 0);
    set_pzs8(res);
    return res;
}
uint16_t Cpu::sub16(uint16_t a, uint16_t b, bool borrow_in) {
    unsigned bb = unsigned(b) + (borrow_in ? 1u : 0u);
    uint16_t res = uint16_t(unsigned(a) - bb);
    set_flag(FLAG_CF, unsigned(a) < bb);
    set_flag(FLAG_AF, ((a ^ b ^ res) & 0x10) != 0);
    set_flag(FLAG_OF, ((a ^ b) & (a ^ res) & 0x8000) != 0);
    set_pzs16(res);
    return res;
}
uint8_t Cpu::and8(uint8_t a, uint8_t b) {
    uint8_t r = uint8_t(a & b);
    set_flag(FLAG_CF, false);
    set_flag(FLAG_OF, false);
    set_pzs8(r);
    return r;
}
uint16_t Cpu::and16(uint16_t a, uint16_t b) {
    uint16_t r = uint16_t(a & b);
    set_flag(FLAG_CF, false);
    set_flag(FLAG_OF, false);
    set_pzs16(r);
    return r;
}
uint8_t Cpu::or8(uint8_t a, uint8_t b) {
    uint8_t r = uint8_t(a | b);
    set_flag(FLAG_CF, false);
    set_flag(FLAG_OF, false);
    set_pzs8(r);
    return r;
}
uint16_t Cpu::or16(uint16_t a, uint16_t b) {
    uint16_t r = uint16_t(a | b);
    set_flag(FLAG_CF, false);
    set_flag(FLAG_OF, false);
    set_pzs16(r);
    return r;
}
uint8_t Cpu::xor8(uint8_t a, uint8_t b) {
    uint8_t r = uint8_t(a ^ b);
    set_flag(FLAG_CF, false);
    set_flag(FLAG_OF, false);
    set_pzs8(r);
    return r;
}
uint16_t Cpu::xor16(uint16_t a, uint16_t b) {
    uint16_t r = uint16_t(a ^ b);
    set_flag(FLAG_CF, false);
    set_flag(FLAG_OF, false);
    set_pzs16(r);
    return r;
}

uint32_t Cpu::add32(uint32_t a, uint32_t b, bool carry_in) {
    uint64_t r = uint64_t(a) + uint64_t(b) + (carry_in ? 1u : 0u);
    uint32_t res = uint32_t(r);
    set_flag(FLAG_CF, r > 0xFFFFFFFFull);
    set_flag(FLAG_AF, ((a ^ b ^ res) & 0x10) != 0);
    set_flag(FLAG_OF, ((~(a ^ b)) & (a ^ res) & 0x80000000u) != 0);
    set_pzs32(res);
    return res;
}
uint32_t Cpu::sub32(uint32_t a, uint32_t b, bool borrow_in) {
    uint64_t bb = uint64_t(b) + (borrow_in ? 1u : 0u);
    uint32_t res = uint32_t(uint64_t(a) - bb);
    set_flag(FLAG_CF, uint64_t(a) < bb);
    set_flag(FLAG_AF, ((a ^ b ^ res) & 0x10) != 0);
    set_flag(FLAG_OF, ((a ^ b) & (a ^ res) & 0x80000000u) != 0);
    set_pzs32(res);
    return res;
}
uint32_t Cpu::and32(uint32_t a, uint32_t b) {
    uint32_t r = a & b;
    set_flag(FLAG_CF, false); set_flag(FLAG_OF, false); set_pzs32(r);
    return r;
}
uint32_t Cpu::or32(uint32_t a, uint32_t b) {
    uint32_t r = a | b;
    set_flag(FLAG_CF, false); set_flag(FLAG_OF, false); set_pzs32(r);
    return r;
}
uint32_t Cpu::xor32(uint32_t a, uint32_t b) {
    uint32_t r = a ^ b;
    set_flag(FLAG_CF, false); set_flag(FLAG_OF, false); set_pzs32(r);
    return r;
}

uint8_t Cpu::alu_apply8(int alu, uint8_t a, uint8_t b) {
    switch (alu) {
        case 0: return add8(a, b, false);              // ADD
        case 1: return or8(a, b);                       // OR
        case 2: return add8(a, b, flag(FLAG_CF));       // ADC
        case 3: return sub8(a, b, flag(FLAG_CF));       // SBB
        case 4: return and8(a, b);                      // AND
        case 5: return sub8(a, b, false);               // SUB
        case 6: return xor8(a, b);                      // XOR
        default: return sub8(a, b, false);              // CMP (caller discards)
    }
}
uint16_t Cpu::alu_apply16(int alu, uint16_t a, uint16_t b) {
    switch (alu) {
        case 0: return add16(a, b, false);
        case 1: return or16(a, b);
        case 2: return add16(a, b, flag(FLAG_CF));
        case 3: return sub16(a, b, flag(FLAG_CF));
        case 4: return and16(a, b);
        case 5: return sub16(a, b, false);
        case 6: return xor16(a, b);
        default: return sub16(a, b, false);
    }
}

uint32_t Cpu::alu_apply32(int alu, uint32_t a, uint32_t b) {
    switch (alu) {
        case 0: return add32(a, b, false);
        case 1: return or32(a, b);
        case 2: return add32(a, b, flag(FLAG_CF));
        case 3: return sub32(a, b, flag(FLAG_CF));
        case 4: return and32(a, b);
        case 5: return sub32(a, b, false);
        case 6: return xor32(a, b);
        default: return sub32(a, b, false);
    }
}

// --- shift/rotate group ---------------------------------------------------
// reg-field selector: 0=ROL 1=ROR 2=RCL 3=RCR 4=SHL/SAL 5=SHR 6=SAL(alias) 7=SAR

uint8_t Cpu::shiftrot8(int op, uint8_t v, int count) {
    count &= 0x1F;  // 80286 masks the count mod 32 (8086 used the raw count)
    if (count == 0) return v;
    uint8_t orig = v;
    bool cf = flag(FLAG_CF);
    uint8_t res = v;
    for (int i = 0; i < count; ++i) {
        switch (op) {
            case 0: cf = (res & 0x80) != 0; res = uint8_t((res << 1) | (cf ? 1 : 0)); break;
            case 1: cf = (res & 1) != 0; res = uint8_t((res >> 1) | (cf ? 0x80 : 0)); break;
            case 2: { bool nc = (res & 0x80) != 0; res = uint8_t((res << 1) | (cf ? 1 : 0)); cf = nc; break; }
            case 3: { bool nc = (res & 1) != 0; res = uint8_t((res >> 1) | (cf ? 0x80 : 0)); cf = nc; break; }
            case 5: cf = (res & 1) != 0; res = uint8_t(res >> 1); break;
            case 7: cf = (res & 1) != 0; res = uint8_t(uint8_t(int8_t(res) >> 1)); break;
            default: cf = (res & 0x80) != 0; res = uint8_t(res << 1); break;  // 4/6: SHL/SAL
        }
    }
    set_flag(FLAG_CF, cf);
    if (op >= 4) set_pzs8(res);  // rotates (0-3) leave PF/ZF/SF alone
    if (count == 1) {
        bool of;
        switch (op) {
            case 1: case 3: of = ((res & 0x80) != 0) != ((res & 0x40) != 0); break;  // ROR/RCR
            case 5: of = (orig & 0x80) != 0; break;                                  // SHR
            case 7: of = false; break;                                              // SAR
            default: of = ((res & 0x80) != 0) != cf; break;                          // ROL/RCL/SHL
        }
        set_flag(FLAG_OF, of);
    }
    return res;
}
uint16_t Cpu::shiftrot16(int op, uint16_t v, int count) {
    count &= 0x1F;
    if (count == 0) return v;
    uint16_t orig = v;
    bool cf = flag(FLAG_CF);
    uint16_t res = v;
    for (int i = 0; i < count; ++i) {
        switch (op) {
            case 0: cf = (res & 0x8000) != 0; res = uint16_t((res << 1) | (cf ? 1 : 0)); break;
            case 1: cf = (res & 1) != 0; res = uint16_t((res >> 1) | (cf ? 0x8000 : 0)); break;
            case 2: { bool nc = (res & 0x8000) != 0; res = uint16_t((res << 1) | (cf ? 1 : 0)); cf = nc; break; }
            case 3: { bool nc = (res & 1) != 0; res = uint16_t((res >> 1) | (cf ? 0x8000 : 0)); cf = nc; break; }
            case 5: cf = (res & 1) != 0; res = uint16_t(res >> 1); break;
            case 7: cf = (res & 1) != 0; res = uint16_t(uint16_t(int16_t(res) >> 1)); break;
            default: cf = (res & 0x8000) != 0; res = uint16_t(res << 1); break;
        }
    }
    set_flag(FLAG_CF, cf);
    if (op >= 4) set_pzs16(res);
    if (count == 1) {
        bool of;
        switch (op) {
            case 1: case 3: of = ((res & 0x8000) != 0) != ((res & 0x4000) != 0); break;
            case 5: of = (orig & 0x8000) != 0; break;
            case 7: of = false; break;
            default: of = ((res & 0x8000) != 0) != cf; break;
        }
        set_flag(FLAG_OF, of);
    }
    return res;
}

uint32_t Cpu::shiftrot32(int op, uint32_t v, int count) {
    count &= 0x1F;
    if (count == 0) return v;
    uint32_t orig = v;
    bool cf = flag(FLAG_CF);
    uint32_t res = v;
    for (int i = 0; i < count; ++i) {
        switch (op) {
            case 0: cf = (res & 0x80000000u) != 0; res = (res << 1) | (cf ? 1u : 0u); break;
            case 1: cf = (res & 1) != 0; res = (res >> 1) | (cf ? 0x80000000u : 0u); break;
            case 2: { bool nc = (res & 0x80000000u) != 0; res = (res << 1) | (cf ? 1u : 0u); cf = nc; break; }
            case 3: { bool nc = (res & 1) != 0; res = (res >> 1) | (cf ? 0x80000000u : 0u); cf = nc; break; }
            case 5: cf = (res & 1) != 0; res = res >> 1; break;
            case 7: cf = (res & 1) != 0; res = uint32_t(int32_t(res) >> 1); break;
            default: cf = (res & 0x80000000u) != 0; res = res << 1; break;
        }
    }
    set_flag(FLAG_CF, cf);
    if (op >= 4) set_pzs32(res);
    if (count == 1) {
        bool of;
        switch (op) {
            case 1: case 3: of = ((res & 0x80000000u) != 0) != ((res & 0x40000000u) != 0); break;
            case 5: of = (orig & 0x80000000u) != 0; break;
            case 7: of = false; break;
            default: of = ((res & 0x80000000u) != 0) != cf; break;
        }
        set_flag(FLAG_OF, of);
    }
    return res;
}

// --- BCD adjust + misc single instructions -------------------------------

void Cpu::daa() {
    uint8_t al = get_reg8(0);
    bool cf = flag(FLAG_CF), af = flag(FLAG_AF);
    uint8_t old_al = al;
    bool old_cf = cf;
    if (((al & 0x0F) > 9) || af) {
        bool carry = (unsigned(al) + 6) > 0xFF;
        al = uint8_t(al + 6);
        cf = old_cf || carry;
        af = true;
    } else {
        af = false;
    }
    if (old_al > 0x99 || old_cf) {
        al = uint8_t(al + 0x60);
        cf = true;
    }
    set_reg8(0, al);
    set_flag(FLAG_CF, cf);
    set_flag(FLAG_AF, af);
    set_pzs8(al);
}
void Cpu::das() {
    uint8_t al = get_reg8(0);
    bool cf = flag(FLAG_CF), af = flag(FLAG_AF);
    uint8_t old_al = al;
    bool old_cf = cf;
    if (((al & 0x0F) > 9) || af) {
        bool borrow = al < 6;
        al = uint8_t(al - 6);
        cf = old_cf || borrow;
        af = true;
    } else {
        af = false;
    }
    if (old_al > 0x99 || old_cf) {
        al = uint8_t(al - 0x60);
        cf = true;
    }
    set_reg8(0, al);
    set_flag(FLAG_CF, cf);
    set_flag(FLAG_AF, af);
    set_pzs8(al);
}
void Cpu::aaa() {
    uint8_t al = get_reg8(0), ah = get_reg8(4);
    bool af = flag(FLAG_AF);
    if (((al & 0x0F) > 9) || af) {
        al = uint8_t(al + 6);
        ah = uint8_t(ah + 1);
        set_flag(FLAG_AF, true);
        set_flag(FLAG_CF, true);
    } else {
        set_flag(FLAG_AF, false);
        set_flag(FLAG_CF, false);
    }
    al = uint8_t(al & 0x0F);
    set_reg8(0, al);
    set_reg8(4, ah);
}
void Cpu::aas() {
    uint8_t al = get_reg8(0), ah = get_reg8(4);
    bool af = flag(FLAG_AF);
    if (((al & 0x0F) > 9) || af) {
        al = uint8_t(al - 6);
        ah = uint8_t(ah - 1);
        set_flag(FLAG_AF, true);
        set_flag(FLAG_CF, true);
    } else {
        set_flag(FLAG_AF, false);
        set_flag(FLAG_CF, false);
    }
    al = uint8_t(al & 0x0F);
    set_reg8(0, al);
    set_reg8(4, ah);
}
void Cpu::aam() {
    uint8_t base = fetch8();
    uint8_t al = get_reg8(0);
    if (base == 0) { ip = instr_start_ip_; interrupt(0); return; }  // #DE, divide error
    uint8_t ah = uint8_t(al / base);
    al = uint8_t(al % base);
    set_reg8(4, ah);
    set_reg8(0, al);
    set_pzs8(al);
}
void Cpu::aad() {
    uint8_t base = fetch8();
    uint8_t al = get_reg8(0), ah = get_reg8(4);
    uint8_t res = uint8_t(al + ah * base);
    set_reg8(0, res);
    set_reg8(4, 0);
    set_pzs8(res);
}

void Cpu::push_reg(int idx) {
    // Real 8086 pushes SP's value from *before* the decrement; the 286
    // changed this to push the value *after* the decrement -- a commonly
    // cited, deliberately-preserved CPU-generation difference (software of
    // the era used exactly this to detect "8086 or 286+" at runtime).
    // Intel iAPX 286 PRM, "Instruction Set Differences from the 8086".
    // Generalizes the same way at the 0x66-prefixed 32-bit width.
    if (opsize32_) {
        uint32_t v = (idx == 4) ? ((sp - 4) & 0xFFFF) : get_reg32(idx);
        sp = (sp - 4) & 0xFFFF;
        wd(ss, uint16_t(sp), v);
    } else {
        uint16_t v = (idx == 4) ? uint16_t(sp - 2) : get_reg16(idx);
        sp = uint16_t(sp - 2);
        ww(ss, uint16_t(sp), v);
    }
}
void Cpu::pusha() {
    uint16_t orig_sp = sp;
    push16(ax); push16(cx); push16(dx); push16(bx);
    push16(orig_sp);
    push16(bp); push16(si); push16(di);
}
void Cpu::popa() {
    di = pop16(); si = pop16(); bp = pop16();
    pop16();  // the saved-SP slot is discarded -- SP is already correct from the pops themselves
    bx = pop16(); dx = pop16(); cx = pop16(); ax = pop16();
}
void Cpu::bound() {
    RM rm = decode_modrm();
    int16_t idx = int16_t(get_reg16(last_reg_));
    int16_t lo = int16_t(rw(rm.seg, rm.off));
    int16_t hi = int16_t(rw(rm.seg, uint16_t(rm.off + 2)));
    if (idx < lo || idx > hi) { ip = instr_start_ip_; interrupt(5); }  // #BR
}
void Cpu::imul_imm16(int dst_reg, const RM &rm, uint16_t imm) {
    int32_t a = int16_t(rm_read16(rm));
    int32_t b = int16_t(imm);
    int32_t r = a * b;
    uint16_t res = uint16_t(r);
    bool of = (int32_t(int16_t(res)) != r);
    set_flag(FLAG_CF, of);
    set_flag(FLAG_OF, of);
    set_reg16(dst_reg, res);
}
void Cpu::imul_imm32(int dst_reg, const RM &rm, uint32_t imm) {
    int64_t a = int32_t(rm_read32(rm));
    int64_t b = int32_t(imm);
    int64_t r = a * b;
    uint32_t res = uint32_t(r);
    bool of = (int64_t(int32_t(res)) != r);
    set_flag(FLAG_CF, of);
    set_flag(FLAG_OF, of);
    set_reg32(dst_reg, res);
}
void Cpu::enter() {
    uint16_t size = fetch16();
    uint8_t level = uint8_t(fetch8() & 0x1F);
    push16(bp);
    uint16_t frame_ptr = uint16_t(sp);
    if (level > 0) {
        for (int i = 1; i < level; ++i) {
            bp = uint16_t(bp - 2);
            push16(rw(ss, bp));
        }
        push16(frame_ptr);
    }
    bp = frame_ptr;
    sp = uint16_t(sp - size);
}
void Cpu::leave() {
    sp = bp;
    bp = pop16();
}

// --- condition codes, control flow ---------------------------------------

bool Cpu::cond(int cc) const {
    switch (cc & 0xF) {
        case 0x0: return flag(FLAG_OF);
        case 0x1: return !flag(FLAG_OF);
        case 0x2: return flag(FLAG_CF);
        case 0x3: return !flag(FLAG_CF);
        case 0x4: return flag(FLAG_ZF);
        case 0x5: return !flag(FLAG_ZF);
        case 0x6: return flag(FLAG_CF) || flag(FLAG_ZF);
        case 0x7: return !flag(FLAG_CF) && !flag(FLAG_ZF);
        case 0x8: return flag(FLAG_SF);
        case 0x9: return !flag(FLAG_SF);
        case 0xA: return flag(FLAG_PF);
        case 0xB: return !flag(FLAG_PF);
        case 0xC: return flag(FLAG_SF) != flag(FLAG_OF);
        case 0xD: return flag(FLAG_SF) == flag(FLAG_OF);
        case 0xE: return flag(FLAG_ZF) || (flag(FLAG_SF) != flag(FLAG_OF));
        default:  return !flag(FLAG_ZF) && (flag(FLAG_SF) == flag(FLAG_OF));
    }
}
void Cpu::jcc(bool taken) {
    int8_t rel = int8_t(fetch8());
    if (taken) ip = uint16_t(ip + rel);
}
void Cpu::loop_group(uint8_t op) {
    int8_t rel = int8_t(fetch8());
    if (op == 0xE3) {  // JCXZ
        if (cx == 0) ip = uint16_t(ip + rel);
        return;
    }
    cx = uint16_t(cx - 1);
    bool take;
    if (op == 0xE0) take = (cx != 0) && !flag(FLAG_ZF);       // LOOPNE/LOOPNZ
    else if (op == 0xE1) take = (cx != 0) && flag(FLAG_ZF);   // LOOPE/LOOPZ
    else take = (cx != 0);                                    // LOOP
    if (take) ip = uint16_t(ip + rel);
}

// --- string instructions ---------------------------------------------------

void Cpu::string_op(uint8_t op) {
    bool wide = (op & 1) != 0;
    bool is_rep = (rep_ != REP_NONE);
    uint16_t src_seg = (seg_override_ >= 0) ? seg_reg(seg_override_) : ds;
    bool is_cmp_scan = (op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF);
    // A 0x66 prefix on a wide (word) string op widens it to dword (e.g.
    // REP MOVSD) -- byte forms (op&1==0) are never affected.
    bool dword = wide && opsize32_;
    do {
        if (is_rep && cx == 0) break;
        int step = dword ? 4 : (wide ? 2 : 1);
        int dir = flag(FLAG_DF) ? -step : step;
        switch (op) {
            case 0xA4: case 0xA5:  // MOVS
                if (!wide) wb(es, di, rb(src_seg, si));
                else if (dword) wd(es, di, rd(src_seg, si));
                else ww(es, di, rw(src_seg, si));
                si = uint16_t(si + dir); di = uint16_t(di + dir);
                break;
            case 0xA6: case 0xA7:  // CMPS
                if (!wide) sub8(rb(src_seg, si), rb(es, di), false);
                else if (dword) sub32(rd(src_seg, si), rd(es, di), false);
                else sub16(rw(src_seg, si), rw(es, di), false);
                si = uint16_t(si + dir); di = uint16_t(di + dir);
                break;
            case 0xAA: case 0xAB:  // STOS
                if (!wide) wb(es, di, get_reg8(0));
                else if (dword) wd(es, di, ax);
                else ww(es, di, uint16_t(ax));
                di = uint16_t(di + dir);
                break;
            case 0xAC: case 0xAD:  // LODS
                if (!wide) set_reg8(0, rb(src_seg, si));
                else if (dword) ax = rd(src_seg, si);
                else ax = (ax & 0xFFFF0000u) | rw(src_seg, si);
                si = uint16_t(si + dir);
                break;
            case 0xAE: case 0xAF:  // SCAS
                if (!wide) sub8(get_reg8(0), rb(es, di), false);
                else if (dword) sub32(ax, rd(es, di), false);
                else sub16(uint16_t(ax), rw(es, di), false);
                di = uint16_t(di + dir);
                break;
            default: break;
        }
        if (!is_rep) break;
        cx = uint16_t(cx - 1);
        if (is_cmp_scan) {
            bool z = flag(FLAG_ZF);
            if (rep_ == REP_Z && !z) break;   // REPE/REPZ: stop once not-equal
            if (rep_ == REP_NZ && z) break;   // REPNE/REPNZ: stop once equal
        }
    } while (is_rep && cx != 0);
}
void Cpu::io_string_op(uint8_t op) {
    bool wide = (op & 1) != 0;
    bool is_rep = (rep_ != REP_NONE);  // real hardware: only a plain REP prefix is meaningful here
    uint16_t src_seg = (seg_override_ >= 0) ? seg_reg(seg_override_) : ds;
    do {
        if (is_rep && cx == 0) break;
        int step = wide ? 2 : 1;
        int dir = flag(FLAG_DF) ? -step : step;
        if (op == 0x6C || op == 0x6D) {  // INS: port DX -> ES:DI
            if (!wide) wb(es, di, bus_.in(uint16_t(dx)));
            else ww(es, di, bus_.in16(uint16_t(dx)));
            di = uint16_t(di + dir);
        } else {  // OUTS: DS:SI -> port DX
            if (!wide) bus_.out(uint16_t(dx), rb(src_seg, si));
            else {
                uint16_t v = rw(src_seg, si);
                bus_.out16(uint16_t(dx), v);
            }
            si = uint16_t(si + dir);
        }
        if (!is_rep) break;
        cx = uint16_t(cx - 1);
    } while (is_rep && cx != 0);
}

// --- opcode groups (reg field of ModR/M selects the operation) -----------

void Cpu::grp1_immed(uint8_t op) {  // 0x80/0x82: r/m8,imm8  0x81: r/m16/32,imm16/32  0x83: r/m16/32,imm8(sx)
    RM rm = decode_modrm();
    int alu = last_reg_;
    bool wide = (op == 0x81 || op == 0x83);
    if (!wide) {
        uint8_t imm = fetch8();
        uint8_t res = alu_apply8(alu, rm_read8(rm), imm);
        if (alu != 7) rm_write8(rm, res);
    } else if (opsize32_) {
        uint32_t imm;
        if (op == 0x83) { int8_t d = int8_t(fetch8()); imm = uint32_t(int32_t(d)); }
        else imm = fetch32();
        uint32_t res = alu_apply32(alu, rm_read32(rm), imm);
        if (alu != 7) rm_write32(rm, res);
    } else {
        uint16_t imm;
        if (op == 0x83) { int8_t d = int8_t(fetch8()); imm = uint16_t(int16_t(d)); }
        else imm = fetch16();
        uint16_t res = alu_apply16(alu, rm_read16(rm), imm);
        if (alu != 7) rm_write16(rm, res);
    }
}
void Cpu::grp2_shift(uint8_t op) {  // 0xC0/0xD0/0xD2: 8-bit  0xC1/0xD1/0xD3: 16-bit
    RM rm = decode_modrm();
    int alu = last_reg_;
    bool wide = (op == 0xC1 || op == 0xD1 || op == 0xD3);
    int count;
    if (op == 0xC0 || op == 0xC1) count = fetch8();
    else if (op == 0xD0 || op == 0xD1) count = 1;
    else count = get_reg8(1);  // CL
    if (!wide) {
        uint8_t res = shiftrot8(alu, rm_read8(rm), count);
        if (count != 0) rm_write8(rm, res);
    } else if (opsize32_) {
        uint32_t res = shiftrot32(alu, rm_read32(rm), count);
        if (count != 0) rm_write32(rm, res);
    } else {
        uint16_t res = shiftrot16(alu, rm_read16(rm), count);
        if (count != 0) rm_write16(rm, res);
    }
}
void Cpu::grp3_unary(uint8_t op) {  // 0xF6: r/m8  0xF7: r/m16 -- TEST/NOT/NEG/MUL/IMUL/DIV/IDIV
    RM rm = decode_modrm();
    int alu = last_reg_;
    bool wide = (op == 0xF7);
    if (!wide) {
        uint8_t v = rm_read8(rm);
        switch (alu) {
            case 0: case 1: { uint8_t imm = fetch8(); and8(v, imm); break; }  // TEST
            case 2: rm_write8(rm, uint8_t(~v)); break;                        // NOT
            case 3: { bool nz = v != 0; rm_write8(rm, sub8(0, v, false)); set_flag(FLAG_CF, nz); break; }  // NEG
            case 4: {  // MUL
                unsigned r = unsigned(get_reg8(0)) * unsigned(v);
                ax = uint16_t(r);
                bool ov = (ax >> 8) != 0;
                set_flag(FLAG_CF, ov); set_flag(FLAG_OF, ov);
                break;
            }
            case 5: {  // IMUL
                int r = int(int8_t(get_reg8(0))) * int(int8_t(v));
                ax = uint16_t(r);
                bool ov = (int8_t(uint8_t(r)) != r);
                set_flag(FLAG_CF, ov); set_flag(FLAG_OF, ov);
                break;
            }
            case 6: {  // DIV
                if (v == 0) { ip = instr_start_ip_; interrupt(0); break; }
                int q = ax / v, rem = ax % v;
                if (q > 0xFF) { ip = instr_start_ip_; interrupt(0); break; }
                ax = uint16_t((uint16_t(uint8_t(rem)) << 8) | uint8_t(q));
                break;
            }
            default: {  // IDIV
                int16_t dividend = int16_t(ax);
                int8_t divisor = int8_t(v);
                if (divisor == 0) { ip = instr_start_ip_; interrupt(0); break; }
                int q = dividend / divisor, rem = dividend % divisor;
                if (q > 127 || q < -128) { ip = instr_start_ip_; interrupt(0); break; }
                ax = uint16_t((uint16_t(uint8_t(int8_t(rem))) << 8) | uint8_t(int8_t(q)));
                break;
            }
        }
    } else if (opsize32_) {
        uint32_t v = rm_read32(rm);
        switch (alu) {
            case 0: case 1: { uint32_t imm = fetch32(); and32(v, imm); break; }
            case 2: rm_write32(rm, ~v); break;
            case 3: { bool nz = v != 0; rm_write32(rm, sub32(0, v, false)); set_flag(FLAG_CF, nz); break; }
            case 4: {
                uint64_t r = uint64_t(ax) * v;
                ax = uint32_t(r); dx = uint32_t(r >> 32);
                bool ov = dx != 0;
                set_flag(FLAG_CF, ov); set_flag(FLAG_OF, ov);
                break;
            }
            case 5: {
                int64_t r = int64_t(int32_t(ax)) * int64_t(int32_t(v));
                ax = uint32_t(r); dx = uint32_t(uint64_t(r) >> 32);
                bool ov = (int64_t(int32_t(uint32_t(r))) != r);
                set_flag(FLAG_CF, ov); set_flag(FLAG_OF, ov);
                break;
            }
            case 6: {
                uint64_t dividend = (uint64_t(dx) << 32) | ax;
                if (v == 0) { ip = instr_start_ip_; interrupt(0); break; }
                uint64_t q = dividend / v, rem = dividend % v;
                if (q > 0xFFFFFFFFull) { ip = instr_start_ip_; interrupt(0); break; }
                ax = uint32_t(q); dx = uint32_t(rem);
                break;
            }
            default: {
                int64_t dividend = int64_t((uint64_t(dx) << 32) | ax);
                int32_t divisor = int32_t(v);
                if (divisor == 0) { ip = instr_start_ip_; interrupt(0); break; }
                int64_t q = dividend / divisor, rem = dividend % divisor;
                if (q > 2147483647ll || q < -2147483648ll) { ip = instr_start_ip_; interrupt(0); break; }
                ax = uint32_t(int32_t(q)); dx = uint32_t(int32_t(rem));
                break;
            }
        }
    } else {
        uint16_t v = rm_read16(rm);
        switch (alu) {
            case 0: case 1: { uint16_t imm = fetch16(); and16(v, imm); break; }
            case 2: rm_write16(rm, uint16_t(~v)); break;
            case 3: { bool nz = v != 0; rm_write16(rm, sub16(0, v, false)); set_flag(FLAG_CF, nz); break; }
            case 4: {
                uint32_t r = uint32_t(uint16_t(ax)) * v;
                ax = (ax & 0xFFFF0000u) | uint16_t(r); dx = (dx & 0xFFFF0000u) | uint16_t(r >> 16);
                bool ov = uint16_t(dx) != 0;
                set_flag(FLAG_CF, ov); set_flag(FLAG_OF, ov);
                break;
            }
            case 5: {
                int32_t r = int32_t(int16_t(ax)) * int32_t(int16_t(v));
                ax = (ax & 0xFFFF0000u) | uint16_t(r); dx = (dx & 0xFFFF0000u) | uint16_t(uint32_t(r) >> 16);
                bool ov = (int32_t(int16_t(uint16_t(r))) != r);
                set_flag(FLAG_CF, ov); set_flag(FLAG_OF, ov);
                break;
            }
            case 6: {
                uint32_t dividend = (uint32_t(uint16_t(dx)) << 16) | uint16_t(ax);
                if (v == 0) { ip = instr_start_ip_; interrupt(0); break; }
                uint32_t q = dividend / v, rem = dividend % v;
                if (q > 0xFFFF) { ip = instr_start_ip_; interrupt(0); break; }
                ax = (ax & 0xFFFF0000u) | uint16_t(q); dx = (dx & 0xFFFF0000u) | uint16_t(rem);
                break;
            }
            default: {
                int32_t dividend = int32_t((uint32_t(uint16_t(dx)) << 16) | uint16_t(ax));
                int16_t divisor = int16_t(v);
                if (divisor == 0) { ip = instr_start_ip_; interrupt(0); break; }
                int32_t q = dividend / divisor, rem = dividend % divisor;
                if (q > 32767 || q < -32768) { ip = instr_start_ip_; interrupt(0); break; }
                ax = (ax & 0xFFFF0000u) | uint16_t(int16_t(q)); dx = (dx & 0xFFFF0000u) | uint16_t(int16_t(rem));
                break;
            }
        }
    }
}
void Cpu::grp5(uint8_t op) {  // 0xFE: INC/DEC r/m8   0xFF: INC/DEC/CALL/JMP/PUSH r/m16
    RM rm = decode_modrm();
    int alu = last_reg_;
    if (op == 0xFE) {
        bool cf = flag(FLAG_CF);
        if (alu == 0) { uint8_t r = add8(rm_read8(rm), 1, false); set_flag(FLAG_CF, cf); rm_write8(rm, r); }
        else if (alu == 1) { uint8_t r = sub8(rm_read8(rm), 1, false); set_flag(FLAG_CF, cf); rm_write8(rm, r); }
        return;
    }
    switch (alu) {
        case 0: {
            bool cf = flag(FLAG_CF);
            if (opsize32_) { uint32_t r = add32(rm_read32(rm), 1, false); set_flag(FLAG_CF, cf); rm_write32(rm, r); }
            else { uint16_t r = add16(rm_read16(rm), 1, false); set_flag(FLAG_CF, cf); rm_write16(rm, r); }
            break;
        }
        case 1: {
            bool cf = flag(FLAG_CF);
            if (opsize32_) { uint32_t r = sub32(rm_read32(rm), 1, false); set_flag(FLAG_CF, cf); rm_write32(rm, r); }
            else { uint16_t r = sub16(rm_read16(rm), 1, false); set_flag(FLAG_CF, cf); rm_write16(rm, r); }
            break;
        }
        case 2: { uint16_t target = rm_read16(rm); push16(ip); ip = target; break; }               // CALL near indirect (always 16-bit -- no evidence of 32-bit near calls in real mode)
        case 3: {                                                                                    // CALL far indirect (memory only)
            uint16_t off = rm_read16(rm);
            uint16_t seg = rw(rm.seg, uint16_t(rm.off + 2));
            push16(cs); push16(ip);
            cs = seg; ip = off;
            break;
        }
        case 4: ip = rm_read16(rm); break;                                                          // JMP near indirect
        case 5: {                                                                                    // JMP far indirect (memory only)
            uint16_t off = rm_read16(rm);
            uint16_t seg = rw(rm.seg, uint16_t(rm.off + 2));
            cs = seg; ip = off;
            break;
        }
        case 6: { if (opsize32_) push32(rm_read32(rm)); else push16(rm_read16(rm)); break; }         // PUSH r/m16/32
        default: break;
    }
}

// --- main decode loop ------------------------------------------------------

int Cpu::step() {
    if (halted) { cycles += 2; return 2; }  // still "running", just idling for an interrupt

    seg_override_ = -1;
    rep_ = REP_NONE;
    opsize32_ = false;
    extra_cycles_ = 0;
    int c = 0;

    uint8_t op;
    for (;;) {
        op = fetch8();
        bool is_prefix = true;
        switch (op) {
            case 0x26: seg_override_ = SEG_ES; break;
            case 0x2E: seg_override_ = SEG_CS; break;
            case 0x36: seg_override_ = SEG_SS; break;
            case 0x3E: seg_override_ = SEG_DS; break;
            case 0x66: opsize32_ = true; break;  // 386 operand-size override -- see the register-storage comment in cpu80286.h
            case 0xF0: break;             // LOCK -- no-op, this core has no other bus master
            case 0xF2: rep_ = REP_NZ; break;
            case 0xF3: rep_ = REP_Z; break;
            default: is_prefix = false; break;
        }
        if (!is_prefix) break;
        c += 2;
    }
    instr_start_ip_ = uint16_t(ip - 1);  // ip already advanced past `op`

    constexpr int CYC_REG = 2, CYC_MEM = 7, CYC_JMP_TAKEN = 7, CYC_JMP_NOT = 3;

    // Fast path: the dense ADD/OR/ADC/SBB/AND/SUB/XOR/CMP block, 0x00-0x3D,
    // laid out in 8 groups of 6 opcodes (formats rm8,r8 / rm16,r16 /
    // r8,rm8 / r16,rm16 / AL,imm8 / AX,imm16 -- the +6/+7 slots in each
    // group are segment push/pop or BCD adjust, handled in the switch below
    // and excluded here since (op & 7) > 5 for all of them).
    if (op < 0x40 && (op & 7) <= 5) {
        int alu = op >> 3;
        switch (op & 7) {
            case 0: { RM rm = decode_modrm(); uint8_t res = alu_apply8(alu, rm_read8(rm), get_reg8(last_reg_)); if (alu != 7) rm_write8(rm, res); c += rm.is_mem ? CYC_MEM : CYC_REG; break; }
            case 1: {
                RM rm = decode_modrm();
                if (opsize32_) { uint32_t res = alu_apply32(alu, rm_read32(rm), get_reg32(last_reg_)); if (alu != 7) rm_write32(rm, res); }
                else { uint16_t res = alu_apply16(alu, rm_read16(rm), get_reg16(last_reg_)); if (alu != 7) rm_write16(rm, res); }
                c += rm.is_mem ? CYC_MEM : CYC_REG;
                break;
            }
            case 2: { RM rm = decode_modrm(); uint8_t res = alu_apply8(alu, get_reg8(last_reg_), rm_read8(rm)); if (alu != 7) set_reg8(last_reg_, res); c += rm.is_mem ? CYC_MEM : CYC_REG; break; }
            case 3: {
                RM rm = decode_modrm();
                if (opsize32_) { uint32_t res = alu_apply32(alu, get_reg32(last_reg_), rm_read32(rm)); if (alu != 7) set_reg32(last_reg_, res); }
                else { uint16_t res = alu_apply16(alu, get_reg16(last_reg_), rm_read16(rm)); if (alu != 7) set_reg16(last_reg_, res); }
                c += rm.is_mem ? CYC_MEM : CYC_REG;
                break;
            }
            case 4: { uint8_t imm = fetch8(); uint8_t res = alu_apply8(alu, get_reg8(0), imm); if (alu != 7) set_reg8(0, res); c += CYC_REG; break; }
            default: {
                if (opsize32_) { uint32_t imm = fetch32(); uint32_t res = alu_apply32(alu, get_reg32(0), imm); if (alu != 7) set_reg32(0, res); }
                else { uint16_t imm = fetch16(); uint16_t res = alu_apply16(alu, get_reg16(0), imm); if (alu != 7) set_reg16(0, res); }
                c += CYC_REG;
                break;
            }
        }
        cycles += c;
        return c;
    }

    switch (op) {
        case 0x06: push16(es); c += CYC_MEM; break;
        case 0x07: es = pop16(); c += CYC_MEM; break;
        case 0x0E: push16(cs); c += CYC_MEM; break;
        case 0x0F: {
            uint8_t op2 = fetch8();
            if (op2 >= 0x80 && op2 <= 0x8F) {
                // Jcc rel16 -- an 80386 addition (Intel didn't define 0x0F
                // 0x80-0x8F on the 286; only the short Jcc rel8 at
                // 0x70-0x7F exists there). NOT genuine 80286 behavior --
                // supported only as a pragmatic compatibility concession
                // for the prebuilt BIOS substitute this machine boots
                // (BIOS-bochs-legacy), which turned out to assume a 386+
                // baseline despite its "legacy"/no-PCI branding. See
                // IBM_PCAT_REVIEW.md's opcode-coverage notes for the
                // investigation that found this.
                int16_t rel = int16_t(fetch16());
                if (cond(op2 & 0xF)) ip = uint16_t(ip + rel);
                c += 3;
            } else if (op2 >= 0x90 && op2 <= 0x9F) {
                // SETcc r/m8 -- another 386 addition, same compatibility
                // concession as Jcc rel16 above.
                RM rm2 = decode_modrm();
                rm_write8(rm2, cond(op2 & 0xF) ? 1 : 0);
                c += 3;
            } else if (op2 == 0xAF) {
                // IMUL r16/32, r/m16/32 (two-operand form) -- 386 addition;
                // the 286 only has the three-operand imm form (0x69/0x6B).
                RM rm2 = decode_modrm();
                if (opsize32_) {
                    int64_t a = int32_t(get_reg32(last_reg_)), b = int32_t(rm_read32(rm2));
                    int64_t r = a * b;
                    bool of = (int64_t(int32_t(uint32_t(r))) != r);
                    set_flag(FLAG_CF, of); set_flag(FLAG_OF, of);
                    set_reg32(last_reg_, uint32_t(r));
                } else {
                    int32_t a = int16_t(get_reg16(last_reg_)), b = int16_t(rm_read16(rm2));
                    int32_t r = a * b;
                    bool of = (int32_t(int16_t(uint16_t(r))) != r);
                    set_flag(FLAG_CF, of); set_flag(FLAG_OF, of);
                    set_reg16(last_reg_, uint16_t(r));
                }
                c += 3;
            } else if (op2 == 0xB6 || op2 == 0xB7) {
                // MOVZX r16/32, r/m8 (0xB6) or r/m16 (0xB7) -- 386 addition.
                RM rm2 = decode_modrm();
                if (op2 == 0xB6) { uint8_t v = rm_read8(rm2); if (opsize32_) set_reg32(last_reg_, v); else set_reg16(last_reg_, v); }
                else { uint16_t v = rm_read16(rm2); if (opsize32_) set_reg32(last_reg_, v); else set_reg16(last_reg_, v); }
                c += 3;
            } else if (op2 == 0xBE || op2 == 0xBF) {
                // MOVSX r16/32, r/m8 (0xBE) or r/m16 (0xBF) -- 386 addition.
                RM rm2 = decode_modrm();
                if (op2 == 0xBE) { int8_t v = int8_t(rm_read8(rm2)); if (opsize32_) set_reg32(last_reg_, uint32_t(int32_t(v))); else set_reg16(last_reg_, uint16_t(int16_t(v))); }
                else { int16_t v = int16_t(rm_read16(rm2)); if (opsize32_) set_reg32(last_reg_, uint32_t(int32_t(v))); else set_reg16(last_reg_, uint16_t(v)); }
                c += 3;
            } else {
                if (on_unimplemented) on_unimplemented(cs, instr_start_ip_, uint16_t(0x0F00 | op2));
                c += 3;  // other protected-mode-only 0x0F opcodes -- unimplemented, see IBM_PCAT_REVIEW.md
            }
            break;
        }
        case 0x16: push16(ss); c += CYC_MEM; break;
        case 0x17: ss = pop16(); c += CYC_MEM; break;
        case 0x1E: push16(ds); c += CYC_MEM; break;
        case 0x1F: ds = pop16(); c += CYC_MEM; break;
        case 0x27: daa(); c += CYC_REG; break;
        case 0x2F: das(); c += CYC_REG; break;
        case 0x37: aaa(); c += CYC_REG; break;
        case 0x3F: aas(); c += CYC_REG; break;
        default:
            if (op >= 0x40 && op <= 0x47) {
                int r = op - 0x40; bool cf = flag(FLAG_CF);
                if (opsize32_) set_reg32(r, add32(get_reg32(r), 1, false));
                else set_reg16(r, add16(get_reg16(r), 1, false));
                set_flag(FLAG_CF, cf); c += CYC_REG;
            }
            else if (op >= 0x48 && op <= 0x4F) {
                int r = op - 0x48; bool cf = flag(FLAG_CF);
                if (opsize32_) set_reg32(r, sub32(get_reg32(r), 1, false));
                else set_reg16(r, sub16(get_reg16(r), 1, false));
                set_flag(FLAG_CF, cf); c += CYC_REG;
            }
            else if (op >= 0x50 && op <= 0x57) { push_reg(op - 0x50); c += CYC_MEM; }
            else if (op >= 0x58 && op <= 0x5F) {
                int r = op - 0x58;
                if (opsize32_) set_reg32(r, pop32()); else set_reg16(r, pop16());
                c += CYC_MEM;
            }
            else if (op == 0x60) { pusha(); c += CYC_MEM; }
            else if (op == 0x61) { popa(); c += CYC_MEM; }
            else if (op == 0x62) { bound(); c += CYC_MEM; }
            else if (op == 0x68) { if (opsize32_) push32(fetch32()); else push16(fetch16()); c += CYC_REG; }
            else if (op == 0x69) {
                RM rm = decode_modrm(); int r = last_reg_;
                if (opsize32_) imul_imm32(r, rm, fetch32()); else imul_imm16(r, rm, fetch16());
                c += CYC_MEM;
            }
            else if (op == 0x6A) { int8_t imm = int8_t(fetch8()); if (opsize32_) push32(uint32_t(int32_t(imm))); else push16(uint16_t(int16_t(imm))); c += CYC_REG; }
            else if (op == 0x6B) {
                RM rm = decode_modrm(); int r = last_reg_; int8_t imm = int8_t(fetch8());
                if (opsize32_) imul_imm32(r, rm, uint32_t(int32_t(imm))); else imul_imm16(r, rm, uint16_t(int16_t(imm)));
                c += CYC_MEM;
            }
            else if (op >= 0x6C && op <= 0x6F) { io_string_op(op); c += CYC_MEM; }
            else if (op >= 0x70 && op <= 0x7F) { bool taken = cond(op & 0xF); jcc(taken); c += taken ? CYC_JMP_TAKEN : CYC_JMP_NOT; }
            else if (op == 0x80 || op == 0x81 || op == 0x82 || op == 0x83) { grp1_immed(op); c += CYC_MEM; }
            else if (op == 0x84) { RM rm = decode_modrm(); and8(rm_read8(rm), get_reg8(last_reg_)); c += rm.is_mem ? CYC_MEM : CYC_REG; }
            else if (op == 0x85) { RM rm = decode_modrm(); and16(rm_read16(rm), get_reg16(last_reg_)); c += rm.is_mem ? CYC_MEM : CYC_REG; }
            else if (op == 0x86) { RM rm = decode_modrm(); uint8_t a = get_reg8(last_reg_), b = rm_read8(rm); set_reg8(last_reg_, b); rm_write8(rm, a); c += CYC_MEM; }
            else if (op == 0x87) { RM rm = decode_modrm(); uint16_t a = get_reg16(last_reg_), b = rm_read16(rm); set_reg16(last_reg_, b); rm_write16(rm, a); c += CYC_MEM; }
            else if (op == 0x88) { RM rm = decode_modrm(); rm_write8(rm, get_reg8(last_reg_)); c += rm.is_mem ? CYC_MEM : CYC_REG; }
            else if (op == 0x89) { RM rm = decode_modrm(); if (opsize32_) rm_write32(rm, get_reg32(last_reg_)); else rm_write16(rm, get_reg16(last_reg_)); c += rm.is_mem ? CYC_MEM : CYC_REG; }
            else if (op == 0x8A) { RM rm = decode_modrm(); set_reg8(last_reg_, rm_read8(rm)); c += rm.is_mem ? CYC_MEM : CYC_REG; }
            else if (op == 0x8B) { RM rm = decode_modrm(); if (opsize32_) set_reg32(last_reg_, rm_read32(rm)); else set_reg16(last_reg_, rm_read16(rm)); c += rm.is_mem ? CYC_MEM : CYC_REG; }
            else if (op == 0x8C) { RM rm = decode_modrm(); rm_write16(rm, seg_reg(last_reg_ & 3)); c += CYC_MEM; }
            else if (op == 0x8D) { RM rm = decode_modrm(); if (opsize32_) set_reg32(last_reg_, rm.off); else set_reg16(last_reg_, rm.off); c += CYC_REG; }  // LEA (rm should be memory; register-mode encoding is undefined on real hardware too)
            else if (op == 0x8E) { RM rm = decode_modrm(); seg_reg(last_reg_ & 3) = rm_read16(rm); c += CYC_MEM; }
            else if (op == 0x8F) { RM rm = decode_modrm(); if (opsize32_) rm_write32(rm, pop32()); else rm_write16(rm, pop16()); c += CYC_MEM; }
            else if (op == 0x90) { c += CYC_REG; }  // NOP (XCHG AX,AX)
            else if (op >= 0x91 && op <= 0x97) {
                int r = op - 0x90;
                if (opsize32_) { uint32_t t = ax; ax = get_reg32(r); set_reg32(r, t); }
                else { uint16_t t = uint16_t(ax); ax = (ax & 0xFFFF0000u) | get_reg16(r); set_reg16(r, t); }
                c += CYC_REG;
            }
            else if (op == 0x98) { if (opsize32_) ax = uint32_t(int32_t(int16_t(ax))); else ax = (ax & 0xFFFF0000u) | uint16_t(int16_t(int8_t(ax & 0xFF))); c += CYC_REG; }  // CBW / CWDE
            else if (op == 0x99) { if (opsize32_) dx = (ax & 0x80000000u) ? 0xFFFFFFFFu : 0u; else dx = (dx & 0xFFFF0000u) | ((ax & 0x8000) ? 0xFFFFu : 0u); c += CYC_REG; }  // CWD / CDQ
            else if (op == 0x9A) { uint16_t off = fetch16(); uint16_t seg = fetch16(); push16(cs); push16(ip); cs = seg; ip = off; c += 13; }  // CALL far
            else if (op == 0x9B) { c += 3; }  // WAIT: no coprocessor present, no-op
            else if (op == 0x9C) { push16(flags); c += CYC_MEM; }
            else if (op == 0x9D) { flags = uint16_t((pop16() & 0x0FD5) | FLAG_R1); c += CYC_MEM; }
            else if (op == 0x9E) { uint8_t ah = get_reg8(4); flags = uint16_t((flags & 0xFF00) | (ah & 0xD5) | FLAG_R1); c += CYC_REG; }  // SAHF
            else if (op == 0x9F) { set_reg8(4, uint8_t(flags & 0xFF)); c += CYC_REG; }  // LAHF
            else if (op >= 0xA0 && op <= 0xA3) {
                uint16_t seg = (seg_override_ >= 0) ? seg_reg(seg_override_) : ds;
                uint16_t off = fetch16();
                if (op == 0xA0) set_reg8(0, rb(seg, off));
                else if (op == 0xA1) { if (opsize32_) ax = rd(seg, off); else ax = (ax & 0xFFFF0000u) | rw(seg, off); }
                else if (op == 0xA2) wb(seg, off, get_reg8(0));
                else { if (opsize32_) wd(seg, off, ax); else ww(seg, off, uint16_t(ax)); }
                c += CYC_MEM;
            }
            else if (op >= 0xA4 && op <= 0xA7) { string_op(op); c += CYC_MEM; }
            else if (op == 0xA8) { uint8_t imm = fetch8(); and8(get_reg8(0), imm); c += CYC_REG; }
            else if (op == 0xA9) { if (opsize32_) and32(ax, fetch32()); else and16(uint16_t(ax), fetch16()); c += CYC_REG; }
            else if (op >= 0xAA && op <= 0xAF) { string_op(op); c += CYC_MEM; }
            else if (op >= 0xB0 && op <= 0xB7) { set_reg8(op - 0xB0, fetch8()); c += CYC_REG; }
            else if (op >= 0xB8 && op <= 0xBF) { if (opsize32_) set_reg32(op - 0xB8, fetch32()); else set_reg16(op - 0xB8, fetch16()); c += CYC_REG; }
            else if (op == 0xC0 || op == 0xC1) { grp2_shift(op); c += CYC_MEM; }
            else if (op == 0xC2) { uint16_t n = fetch16(); ip = pop16(); sp = uint16_t(sp + n); c += 11; }
            else if (op == 0xC3) { ip = pop16(); c += 11; }
            else if (op == 0xC4) { RM rm = decode_modrm(); int r = last_reg_; uint16_t off = rm_read16(rm); uint16_t seg = rw(rm.seg, uint16_t(rm.off + 2)); set_reg16(r, off); es = seg; c += CYC_MEM; }
            else if (op == 0xC5) { RM rm = decode_modrm(); int r = last_reg_; uint16_t off = rm_read16(rm); uint16_t seg = rw(rm.seg, uint16_t(rm.off + 2)); set_reg16(r, off); ds = seg; c += CYC_MEM; }
            else if (op == 0xC6) { RM rm = decode_modrm(); uint8_t imm = fetch8(); rm_write8(rm, imm); c += rm.is_mem ? CYC_MEM : CYC_REG; }
            else if (op == 0xC7) { RM rm = decode_modrm(); if (opsize32_) rm_write32(rm, fetch32()); else rm_write16(rm, fetch16()); c += rm.is_mem ? CYC_MEM : CYC_REG; }
            else if (op == 0xC8) { enter(); c += 15; }
            else if (op == 0xC9) { leave(); c += CYC_REG; }
            else if (op == 0xCA) { uint16_t n = fetch16(); ip = pop16(); cs = pop16(); sp = uint16_t(sp + n); c += 15; }
            else if (op == 0xCB) { ip = pop16(); cs = pop16(); c += 15; }
            else if (op == 0xCC) { interrupt(3); c += 45; }
            else if (op == 0xCD) { uint8_t n = fetch8(); interrupt(n); c += 45; }
            else if (op == 0xCE) { if (flag(FLAG_OF)) { interrupt(4); c += 45; } else c += 3; }
            else if (op == 0xCF) { ip = pop16(); cs = pop16(); flags = uint16_t((pop16() & 0x0FD5) | FLAG_R1); c += 17; }
            else if (op >= 0xD0 && op <= 0xD3) { grp2_shift(op); c += CYC_MEM; }
            else if (op == 0xD4) { aam(); c += 16; }
            else if (op == 0xD5) { aad(); c += 14; }
            else if (op == 0xD7) { uint16_t seg = (seg_override_ >= 0) ? seg_reg(seg_override_) : ds; set_reg8(0, rb(seg, uint16_t(bx + get_reg8(0)))); c += CYC_MEM; }  // XLAT
            else if (op >= 0xD8 && op <= 0xDF) { decode_modrm(); c += 3; }  // x87 escape: no coprocessor, consume the operand and do nothing
            else if (op == 0xE0 || op == 0xE1 || op == 0xE2 || op == 0xE3) { loop_group(op); c += CYC_JMP_NOT; }
            else if (op == 0xE4) { uint8_t p = fetch8(); set_reg8(0, bus_.in(p)); c += CYC_MEM; }
            else if (op == 0xE5) { uint8_t p = fetch8(); ax = (ax & 0xFFFF0000u) | bus_.in16(p); c += CYC_MEM; }
            else if (op == 0xE6) { uint8_t p = fetch8(); bus_.out(p, get_reg8(0)); c += CYC_MEM; }
            else if (op == 0xE7) { uint8_t p = fetch8(); bus_.out16(p, uint16_t(ax)); c += CYC_MEM; }
            else if (op == 0xE8) { int16_t rel = int16_t(fetch16()); push16(ip); ip = uint16_t(ip + rel); c += 11; }
            else if (op == 0xE9) { int16_t rel = int16_t(fetch16()); ip = uint16_t(ip + rel); c += 7; }
            else if (op == 0xEA) { uint16_t off = fetch16(); uint16_t seg = fetch16(); cs = seg; ip = off; c += 11; }
            else if (op == 0xEB) { int8_t rel = int8_t(fetch8()); ip = uint16_t(ip + rel); c += 7; }
            else if (op == 0xEC) { set_reg8(0, bus_.in(dx)); c += CYC_MEM; }
            else if (op == 0xED) { ax = (ax & 0xFFFF0000u) | bus_.in16(uint16_t(dx)); c += CYC_MEM; }
            else if (op == 0xEE) { bus_.out(dx, get_reg8(0)); c += CYC_MEM; }
            else if (op == 0xEF) { bus_.out16(uint16_t(dx), uint16_t(ax)); c += CYC_MEM; }
            else if (op == 0xF1) { c += 2; }  // undefined/ICEBP -- treated as a no-op
            else if (op == 0xF4) { halted = true; c += 2; }  // HLT
            else if (op == 0xF5) { set_flag(FLAG_CF, !flag(FLAG_CF)); c += CYC_REG; }  // CMC
            else if (op == 0xF6 || op == 0xF7) { grp3_unary(op); c += CYC_MEM; }
            else if (op == 0xF8) { set_flag(FLAG_CF, false); c += CYC_REG; }
            else if (op == 0xF9) { set_flag(FLAG_CF, true); c += CYC_REG; }
            else if (op == 0xFA) { set_flag(FLAG_IF, false); c += CYC_REG; }
            else if (op == 0xFB) { set_flag(FLAG_IF, true); c += CYC_REG; }
            else if (op == 0xFC) { set_flag(FLAG_DF, false); c += CYC_REG; }
            else if (op == 0xFD) { set_flag(FLAG_DF, true); c += CYC_REG; }
            else if (op == 0xFE || op == 0xFF) { grp5(op); c += CYC_MEM; }
            else {
                if (on_unimplemented) on_unimplemented(cs, instr_start_ip_, op);
                c += 2;  // unimplemented opcode -- see IBM_PCAT_REVIEW.md's coverage notes
            }
            break;
    }

    c += extra_cycles_;
    cycles += c;
    return c;
}

}  // namespace cpu80286
