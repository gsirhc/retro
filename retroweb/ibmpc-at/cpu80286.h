// Intel 80286 CPU core, real-address-mode only -- plus a deliberate,
// labelled 80386 compatibility layer (32-bit registers via the 0x66
// operand-size prefix, and the 386's most common 0x0F-prefixed additions:
// Jcc rel16, SETcc, two-operand IMUL, MOVZX/MOVSX). NONE of that is
// genuine 80286 behavior -- Intel's real 80286 has no EAX, no 0x66 prefix,
// and none of those 0x0F opcodes. It exists because this machine's actual
// firmware substitute (BIOS-bochs-legacy, IBM_PCAT_REVIEW.md §6) turned out
// to assume a 386+ baseline despite its "legacy"/no-PCI branding -- found
// opcode-by-opcode via the on_unimplemented diagnostic hook against the
// real fetched binary, not guessed at speculatively. Kept as a clearly
// commented, evidence-driven concession, the same way CLAUDE.md requires
// any other realism override to be labelled rather than silent.
//
// Scope: PC-DOS/FreeDOS on a genuine 5170 never leave real mode, so this core
// implements real mode exclusively -- no GDT/LDT/IDT/TSS, no descriptor
// caches, no privilege checking. See IBM_PCAT_REVIEW.md for the full
// rationale and the (small) list of protected-mode-only opcodes therefore
// deliberately unimplemented (LGDT/SGDT/LIDT/SIDT/LLDT/SLDT/LTR/STR/LMSW/
// SMSW/ARPL/LAR/LSL/VERR/VERW/CLTS).
//
// Like i8080::Cpu, this core is host-agnostic: it talks to the outside world
// only through the Bus callbacks below. The address bus is 24 bits wide (the
// real 80286 has 24 physical address lines) -- the CPU always computes the
// full segment:offset -> physical address itself and never wraps it at 1MB.
// On real hardware that wraparound (relied on by some 8086-era software) is
// enforced or not by the A20 gate, which lives on the motherboard (wired
// through the 8042 keyboard controller's P21 output-port bit on a genuine
// AT), not in the CPU. So it belongs in the embedding chipset's Bus::read/
// write, not here -- this core deliberately never masks addr itself.
//
// Cycle counts and instruction semantics are drawn from the Intel iAPX 286
// Programmer's Reference Manual (1987), the 80286 data sheet's timing
// appendix, and (for the shared 8086-legacy subset) the Intel 8086/8088
// User's Manual. Cited by section inline where a specific quirk is being
// preserved rather than guessed at.

#ifndef IBMPCAT_CPU80286_H
#define IBMPCAT_CPU80286_H

#include <cstdint>
#include <functional>

namespace cpu80286 {

// FLAGS register bit positions (real mode: IOPL/NT exist as storage for
// PUSHF/POPF fidelity but have no privilege-check effect without protected
// mode). Bit 1 always reads 1; bits 3, 5 are reserved/0 on the 8086 lineage.
enum Flag : uint16_t {
    FLAG_CF   = 1 << 0,   // carry
    FLAG_R1   = 1 << 1,   // reserved, always 1
    FLAG_PF   = 1 << 2,   // parity (low byte of result, even = 1)
    FLAG_AF   = 1 << 4,   // auxiliary carry (BCD)
    FLAG_ZF   = 1 << 6,   // zero
    FLAG_SF   = 1 << 7,   // sign
    FLAG_TF   = 1 << 8,   // trap (single-step)
    FLAG_IF   = 1 << 9,   // interrupt enable
    FLAG_DF   = 1 << 10,  // direction (string ops)
    FLAG_OF   = 1 << 11,  // overflow
    FLAG_IOPL = 3 << 12,  // I/O privilege level (2 bits) -- storage only
    FLAG_NT   = 1 << 14,  // nested task -- storage only
};

// Memory and port I/O callbacks supplied by the embedding chipset. `addr` is
// a full 24-bit physical address (segment*16 + offset, unmasked); `port` is
// the 16-bit x86 I/O-space address.
//
// in16/out16 exist because a genuine 80286 can do a 16-bit port access as
// one atomic bus cycle -- and for most ISA devices that's equivalent to
// two adjacent 8-bit accesses (port, port+1), but it is NOT for a device
// whose data register is inherently 16-bit at a single port address (the
// WD1003/ATA data register at 0x1F0 is exactly this: 0x1F1 is a completely
// different register, the Error/Features register, not "the high byte of
// 0x1F0"). The chipset's default in16/out16 compose two 8-bit accesses,
// preserving old behavior for every other port; only the hard disk
// controller's data register needs the true atomic path. See wd1003.h and
// IBM_PCAT_REVIEW.md.
struct Bus {
    std::function<uint8_t(uint32_t addr)>          read;
    std::function<void(uint32_t addr, uint8_t v)>  write;
    std::function<uint8_t(uint16_t port)>          in;
    std::function<void(uint16_t port, uint8_t v)>  out;
    std::function<uint16_t(uint16_t port)>         in16;
    std::function<void(uint16_t port, uint16_t v)> out16;
};

class Cpu {
public:
    // General registers. Stored as full 32-bit values -- a genuine 80286
    // has no 32-bit registers or EAX/0x66 prefix at all (Intel iAPX 286
    // PRM), but this core also runs the 386-targeted BIOS substitute this
    // machine boots (see IBM_PCAT_REVIEW.md's opcode-coverage notes), which
    // does use them even in 16-bit real-mode code via the 0x66 operand-size
    // prefix -- a deliberate, labelled compatibility concession, not
    // genuine 80286 behavior, same as the 0x0F 0x80-0x8F Jcc-rel16 one.
    // AX/BX/... (and AL/AH etc.) are simply the low 16 (or 8) bits of this
    // same storage, matching real hardware: writing AX never disturbs
    // EAX's upper 16 bits.
    uint32_t ax = 0, bx = 0, cx = 0, dx = 0;
    uint32_t sp = 0, bp = 0, si = 0, di = 0;

    // Segment registers and instruction pointer.
    uint16_t cs = 0xF000, ds = 0, es = 0, ss = 0;
    uint16_t ip = 0;

    uint16_t flags = FLAG_R1;

    bool     halted = false;
    uint64_t cycles = 0;   // total clock cycles executed (includes wait states)

    // Diagnostic hook: called with (CS, IP-of-opcode, opcode-word) whenever
    // step() falls through to the "unimplemented opcode" no-op path -- a
    // genuinely unrecognized single-byte opcode (opcode-word = 0x00xx), or
    // an 0x0F sub-opcode outside the 0x80-0x8F Jcc-rel16 concession
    // (opcode-word = 0x0Fxx). Empty by default (costs nothing); set by a
    // diagnostic harness to find real opcode-coverage gaps by evidence
    // instead of by guessing. See IBM_PCAT_REVIEW.md.
    std::function<void(uint16_t cs, uint16_t ip, uint16_t opcode_word)> on_unimplemented;

    explicit Cpu(Bus bus) : bus_(std::move(bus)) {}

    // Power-on/RESET state: CS:IP = F000:FFF0 (the real 80286 reset vector,
    // just below the top of the 16MB address space it can address but
    // aliased so the BIOS's F0000-FFFFF ROM window is reachable even before
    // any segment load) -- Intel iAPX 286 PRM, "Initialization".
    void reset();

    // Decode and execute exactly one instruction at CS:IP.
    // Returns the number of clock cycles consumed (including the 1
    // wait-state-per-access penalty this machine's memory bus imposes,
    // folded into the per-opcode base counts -- see kBaseCycles8/16).
    int step();

    // Deliver a hardware/software interrupt: real-mode INT n semantics --
    // push FLAGS, CS, IP; clear IF and TF; fetch the 4-byte real-mode
    // interrupt vector at physical address vector*4; jump there. `vector` is
    // the already-resolved interrupt number (0-255), e.g. from the PIC's
    // INTA cycle. Always serviced (real mode has no gate/privilege check);
    // it is the caller's job to honor `flags & FLAG_IF` before calling this
    // for a maskable (as opposed to NMI) source. Wakes HLT. Returns cycles.
    int interrupt(uint8_t vector);

    bool flag(Flag f) const { return (flags & f) != 0; }
    void set_flag(Flag f, bool on) { flags = on ? (flags | f) : (flags & ~f); }

    // 8-bit half-register access by the 3-bit ModR/M reg/rm encoding
    // (0=AL,1=CL,2=DL,3=BL,4=AH,5=CH,6=DH,7=BH).
    uint8_t  get_reg8(int idx) const;
    void     set_reg8(int idx, uint8_t v);
    // 16-bit register access by the 3-bit encoding (0=AX,1=CX,2=DX,3=BX,
    // 4=SP,5=BP,6=SI,7=DI). Preserves the register's upper 16 bits, per
    // real hardware.
    uint16_t get_reg16(int idx) const;
    void     set_reg16(int idx, uint16_t v);
    // Same encoding, full 32-bit width (the 0x66-prefixed form).
    uint32_t get_reg32(int idx) const;
    void     set_reg32(int idx, uint32_t v);

private:
    Bus bus_;

    // Segment-override prefix in effect for the instruction being decoded
    // (-1 = none, else one of the seg_* indices below), and the REP/REPNE
    // prefix in effect for string ops. Reset at the start of each step().
    int  seg_override_ = -1;
    enum { SEG_ES = 0, SEG_CS = 1, SEG_SS = 2, SEG_DS = 3 };
    enum RepMode { REP_NONE, REP_Z, REP_NZ };
    RepMode rep_ = REP_NONE;
    // 0x66 (operand-size) prefix -- see the register-storage comment above.
    // 0x67 (address-size) is deliberately NOT supported: nothing this
    // machine boots has been observed to need 32-bit *addressing* (every
    // real-mode offset still fits in 16 bits), only 32-bit operands, so
    // there's no evidence-based reason to add SIB-byte/32-bit-displacement
    // decoding. See IBM_PCAT_REVIEW.md.
    bool opsize32_ = false;

    uint16_t &seg_reg(int idx);   // ES/CS/SS/DS by the indices above

    // memory / immediate fetch helpers -- physical = seg*16 + offset,
    // 16-bit offset wraps mod 0x10000 (a real 80286 segment really is only
    // 64K in real mode; this is not the A20 question, which is about the
    // *linear* seg*16+off sum exceeding 1MB, handled by the chipset).
    uint32_t phys(uint16_t seg, uint16_t off) const { return (uint32_t(seg) << 4) + off; }
    uint8_t  rb(uint16_t seg, uint16_t off)            { return bus_.read(phys(seg, off)); }
    void     wb(uint16_t seg, uint16_t off, uint8_t v) { bus_.write(phys(seg, off), v); }
    uint16_t rw(uint16_t seg, uint16_t off) { return rb(seg, off) | (uint16_t(rb(seg, uint16_t(off + 1))) << 8); }
    void     ww(uint16_t seg, uint16_t off, uint16_t v) { wb(seg, off, v & 0xFF); wb(seg, uint16_t(off + 1), v >> 8); }
    uint32_t rd(uint16_t seg, uint16_t off) { return uint32_t(rw(seg, off)) | (uint32_t(rw(seg, uint16_t(off + 2))) << 16); }
    void     wd(uint16_t seg, uint16_t off, uint32_t v) { ww(seg, off, uint16_t(v & 0xFFFF)); ww(seg, uint16_t(off + 2), uint16_t(v >> 16)); }

    uint8_t  fetch8()  { return rb(cs, ip++); }
    uint16_t fetch16() { uint16_t v = rw(cs, ip); ip += 2; return v; }
    uint32_t fetch32() { uint32_t v = rd(cs, ip); ip += 4; return v; }

    // stack (always in SS). 32-bit push/pop moves SP by 4, matching the
    // 0x66-prefixed forms; the real PUSH-SP-decremented-value quirk
    // (push_reg(), cpu80286.cpp) generalizes the same way at this width.
    void     push16(uint16_t v) { sp -= 2; ww(ss, uint16_t(sp), v); }
    uint16_t pop16()            { uint16_t v = rw(ss, uint16_t(sp)); sp += 2; return v; }
    // sp is kept 16-bit-wrapped even for the 32-bit-operand forms: this
    // core doesn't support a 32-bit *address* size (no 0x67 prefix, no
    // "big real mode"), so the stack pointer's own wraparound stays real
    // mode's normal 64KB regardless of what width of value is being
    // pushed/popped.
    void     push32(uint32_t v) { sp = (sp - 4) & 0xFFFF; wd(ss, uint16_t(sp), v); }
    uint32_t pop32()            { uint32_t v = rd(ss, uint16_t(sp)); sp = (sp + 4) & 0xFFFF; return v; }

    // --- ModR/M decode ----------------------------------------------------
    // An operand resolved by ModR/M: either a register (is_mem=false, reg
    // index in `reg`) or a memory location (is_mem=true, segment+offset
    // already combining any override / default-segment rule, e.g. BP-based
    // addressing defaulting to SS -- Intel 8086 manual table 2-19).
    struct RM {
        bool     is_mem;
        int      reg;      // valid when !is_mem
        uint16_t seg, off;  // valid when is_mem
    };
    // Decodes the ModR/M byte (and any displacement) starting at CS:IP,
    // advancing IP past it. `wide` selects the 16-bit vs 8-bit register
    // field interpretation for a register-mode RM (does not affect memory
    // addressing, which is always 16-bit offsets in this mode).
    RM decode_modrm();
    uint8_t  rm_read8(const RM &rm)              { return rm.is_mem ? rb(rm.seg, rm.off) : get_reg8(rm.reg); }
    void     rm_write8(const RM &rm, uint8_t v)  { if (rm.is_mem) wb(rm.seg, rm.off, v); else set_reg8(rm.reg, v); }
    uint16_t rm_read16(const RM &rm)             { return rm.is_mem ? rw(rm.seg, rm.off) : get_reg16(rm.reg); }
    void     rm_write16(const RM &rm, uint16_t v){ if (rm.is_mem) ww(rm.seg, rm.off, v); else set_reg16(rm.reg, v); }
    uint32_t rm_read32(const RM &rm)             { return rm.is_mem ? rd(rm.seg, rm.off) : get_reg32(rm.reg); }
    void     rm_write32(const RM &rm, uint32_t v){ if (rm.is_mem) wd(rm.seg, rm.off, v); else set_reg32(rm.reg, v); }

    // flag helpers
    void set_pzs8(uint8_t r);
    void set_pzs16(uint16_t r);
    void set_pzs32(uint32_t r);
    static bool parity_even(uint8_t v);

    // ALU primitives -- 8/16-bit pairs, all set CF/OF/AF/PF/ZF/SF per the
    // Intel manual's flag-affected tables and return the result.
    uint8_t  add8(uint8_t a, uint8_t b, bool carry_in);
    uint16_t add16(uint16_t a, uint16_t b, bool carry_in);
    uint8_t  sub8(uint8_t a, uint8_t b, bool borrow_in);
    uint16_t sub16(uint16_t a, uint16_t b, bool borrow_in);
    uint8_t  and8(uint8_t a, uint8_t b);
    uint16_t and16(uint16_t a, uint16_t b);
    uint8_t  or8(uint8_t a, uint8_t b);
    uint16_t or16(uint16_t a, uint16_t b);
    uint8_t  xor8(uint8_t a, uint8_t b);
    uint16_t xor16(uint16_t a, uint16_t b);
    // 32-bit ALU primitives -- the 0x66-prefixed compatibility-concession
    // width (see the register-storage comment above); same flag semantics.
    uint32_t add32(uint32_t a, uint32_t b, bool carry_in);
    uint32_t sub32(uint32_t a, uint32_t b, bool borrow_in);
    uint32_t and32(uint32_t a, uint32_t b);
    uint32_t or32(uint32_t a, uint32_t b);
    uint32_t xor32(uint32_t a, uint32_t b);

    uint8_t  alu_apply8(int alu, uint8_t a, uint8_t b);   // alu = ADD/OR/ADC/SBB/AND/SUB/XOR/CMP selector, 0-7
    uint16_t alu_apply16(int alu, uint16_t a, uint16_t b);
    uint32_t alu_apply32(int alu, uint32_t a, uint32_t b);

    // shift/rotate group (0xD0-D3 by 1/CL, 0xC0/C1 by imm8 -- the 286-new
    // encoding). `count` already masked mod 32 the way real silicon does.
    uint8_t  shiftrot8(int op, uint8_t v, int count);
    uint16_t shiftrot16(int op, uint16_t v, int count);
    uint32_t shiftrot32(int op, uint32_t v, int count);

    // BCD adjust and misc single-purpose instructions, one method each --
    // named directly after the mnemonic since there's no useful grouping.
    void daa(); void das(); void aaa(); void aas(); void aam(); void aad();
    void push_reg(int idx);         // PUSH reg16, with the real PUSH-SP-pushes-decremented-value quirk
    void pusha(); void popa();      // 286-native PUSHA/POPA
    void bound();                   // 286-native BOUND r16, m16&16
    void imul_imm16(int dst_reg, const RM &rm, uint16_t imm);  // 286-native IMUL r16,r/m16,imm
    void imul_imm32(int dst_reg, const RM &rm, uint32_t imm);  // 0x66-prefixed 32-bit form
    void enter(); void leave();     // 286-native stack-frame instructions

    int  extra_cycles_ = 0;  // set by helpers (taken branch, rep iteration count, ...) and added to the opcode's base cost by step()

    int      last_reg_ = 0;        // ModR/M reg field from the most recent decode_modrm() -- read by the opcode-group helpers below, which use that field to select the operation rather than a register operand
    uint16_t instr_start_ip_ = 0;  // CS:IP at the start of the instruction (post-prefixes), so DIV/IDIV faults can restore IP to the faulting instruction the way real hardware does

    // control-flow / string-op / misc helpers used by step()'s big switch
    bool cond(int cc) const;          // Jcc/LOOPcc condition-code evaluation (cc = opcode low nibble)
    void jcc(bool taken);
    void loop_group(uint8_t op);
    void string_op(uint8_t op);       // 0xA4-0xA7, 0xAA-0xAF: MOVS/CMPS/STOS/LODS/SCAS, honors REP/REPE/REPNE
    void io_string_op(uint8_t op);    // 0x6C-0x6F: INS/OUTS
    void grp1_immed(uint8_t op);      // 0x80/0x81/0x83: ADD/OR/ADC/SBB/AND/SUB/XOR/CMP r/m,imm
    void grp2_shift(uint8_t op);      // 0xC0/C1/D0-D3: shift/rotate group
    void grp3_unary(uint8_t op);      // 0xF6/0xF7: TEST/NOT/NEG/MUL/IMUL/DIV/IDIV
    void grp5(uint8_t op);            // 0xFE/0xFF: INC/DEC/CALL/JMP/PUSH r/m
};

} // namespace cpu80286

#endif // IBMPCAT_CPU80286_H
