// Intel 80286 CPU core, real mode only, plus an 80386 compatibility layer:
// 32-bit registers via the 0x66 prefix, Jcc rel16, SETcc, two-operand IMUL,
// MOVZX/MOVSX. None of that exists on a real 286. BIOS-bochs-legacy assumes
// a 386+ baseline (IBM_PCAT_REVIEW.md §6), found via on_unimplemented.
//
// No GDT/LDT/IDT/TSS, descriptor caches or privilege checks. Protected-mode
// opcodes (LGDT/SGDT/LIDT/SIDT/LLDT/SLDT/LTR/STR/LMSW/SMSW/ARPL/LAR/LSL/
// VERR/VERW/CLTS) are unimplemented.
//
// The core talks only through Bus. Addresses are 24 bits and never wrapped at
// 1MB here, because the A20 gate is motherboard logic (8042 P21) in the chipset.
//
// Timings and semantics: Intel iAPX 286 Programmer's Reference Manual (1987),
// 80286 data sheet timing appendix, and the 8086/8088 User's Manual.

#ifndef IBMPCAT_CPU80286_H
#define IBMPCAT_CPU80286_H

#include <cstdint>
#include <functional>

namespace cpu80286 {

// FLAGS bits. IOPL/NT are storage only in real mode.
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

// `addr` is a 24-bit physical address, unmasked. in16/out16 are one atomic
// 16-bit bus cycle, needed for the WD1003 data register at 0x1F0 (0x1F1 is
// the Error register, not its high byte). The chipset composes two 8-bit
// accesses for every other port.
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
    // Stored as 32-bit for the 0x66 386 concession. AX/AL/AH are the low bits;
    // writing AX leaves EAX's upper 16 bits alone.
    uint32_t ax = 0, bx = 0, cx = 0, dx = 0;
    uint32_t sp = 0, bp = 0, si = 0, di = 0;

    // Segment registers and IP.
    uint16_t cs = 0xF000, ds = 0, es = 0, ss = 0;
    uint16_t ip = 0;

    uint16_t flags = FLAG_R1;

    bool     halted = false;
    uint64_t cycles = 0;   // total clock cycles executed (includes wait states)

    // Called with (CS, IP, opcode-word) on an unimplemented opcode: 0x00xx for
    // single-byte, 0x0Fxx for 0x0F sub-opcodes outside Jcc rel16.
    std::function<void(uint16_t cs, uint16_t ip, uint16_t opcode_word)> on_unimplemented;

    explicit Cpu(Bus bus) : bus_(std::move(bus)) {}

    // Reset vector F000:FFF0 (Intel iAPX 286 PRM, "Initialization").
    void reset();

    // Executes one instruction at CS:IP and returns its clock cycles (iAPX 286
    // PRM / 80286 data sheet). Shift, string and MUL/DIV groups compute their
    // own cost. The 5170-339's extra memory wait state is not modeled.
    int step();

    // Real-mode INT n: push FLAGS, CS, IP, clear IF and TF, jump through the
    // vector at vector*4. Wakes HLT. The caller checks IF for maskable sources.
    int interrupt(uint8_t vector);

    bool flag(Flag f) const { return (flags & f) != 0; }
    void set_flag(Flag f, bool on) { flags = on ? (flags | f) : (flags & ~f); }

    // 8-bit register by ModR/M encoding (0=AL..3=BL, 4=AH..7=BH).
    uint8_t  get_reg8(int idx) const;
    void     set_reg8(int idx, uint8_t v);
    // 16-bit register by encoding (0=AX,1=CX,2=DX,3=BX,4=SP,5=BP,6=SI,7=DI).
    // Upper 16 bits are preserved.
    uint16_t get_reg16(int idx) const;
    void     set_reg16(int idx, uint16_t v);
    // 32-bit form (0x66 prefix).
    uint32_t get_reg32(int idx) const;
    void     set_reg32(int idx, uint32_t v);

private:
    Bus bus_;

    // Segment override (-1 = none) and REP/REPNE state; reset each step().
    int  seg_override_ = -1;
    enum { SEG_ES = 0, SEG_CS = 1, SEG_SS = 2, SEG_DS = 3 };
    enum RepMode { REP_NONE, REP_Z, REP_NZ };
    RepMode rep_ = REP_NONE;
    // 0x67 address-size is unsupported: only 32-bit operands are observed.
    bool opsize32_ = false;

    uint16_t &seg_reg(int idx);   // ES/CS/SS/DS by the indices above

    // physical = seg*16 + off; the 16-bit offset wraps at 64K.
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

    // Stack is always SS. 32-bit push/pop moves SP by 4.
    void     push16(uint16_t v) { sp -= 2; ww(ss, uint16_t(sp), v); }
    uint16_t pop16()            { uint16_t v = rw(ss, uint16_t(sp)); sp += 2; return v; }
    // sp wraps at 64KB even for 32-bit operands (no 0x67 / big real mode).
    void     push32(uint32_t v) { sp = (sp - 4) & 0xFFFF; wd(ss, uint16_t(sp), v); }
    uint32_t pop32()            { uint32_t v = rd(ss, uint16_t(sp)); sp = (sp + 4) & 0xFFFF; return v; }

    // --- ModR/M decode ---
    // Register (is_mem=false) or memory operand with default-segment rules
    // applied (BP-based defaults to SS, 8086 manual table 2-19).
    struct RM {
        bool     is_mem;
        int      reg;      // valid when !is_mem
        uint16_t seg, off;  // valid when is_mem
    };
    // Decodes ModR/M and any displacement at CS:IP, advancing IP.
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

    // ALU primitives set CF/OF/AF/PF/ZF/SF per the Intel flag tables.
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
    // 32-bit forms (0x66).
    uint32_t add32(uint32_t a, uint32_t b, bool carry_in);
    uint32_t sub32(uint32_t a, uint32_t b, bool borrow_in);
    uint32_t and32(uint32_t a, uint32_t b);
    uint32_t or32(uint32_t a, uint32_t b);
    uint32_t xor32(uint32_t a, uint32_t b);

    uint8_t  alu_apply8(int alu, uint8_t a, uint8_t b);   // alu: ADD/OR/ADC/SBB/AND/SUB/XOR/CMP = 0-7
    uint16_t alu_apply16(int alu, uint16_t a, uint16_t b);
    uint32_t alu_apply32(int alu, uint32_t a, uint32_t b);

    // Shift/rotate group; `count` is already masked mod 32.
    uint8_t  shiftrot8(int op, uint8_t v, int count);
    uint16_t shiftrot16(int op, uint16_t v, int count);
    uint32_t shiftrot32(int op, uint32_t v, int count);

    // BCD adjust and single-purpose instructions.
    void daa(); void das(); void aaa(); void aas(); void aam(); void aad();
    void push_reg(int idx);         // PUSH SP pushes the decremented value
    void pusha(); void popa();      // 286-native PUSHA/POPA
    void bound();                   // 286-native BOUND r16, m16&16
    void imul_imm16(int dst_reg, const RM &rm, uint16_t imm);  // 286 IMUL r16,r/m16,imm
    void imul_imm32(int dst_reg, const RM &rm, uint32_t imm);  // 0x66-prefixed 32-bit form
    // enter() returns the nesting level; ENTER cost is 11 / 15 / 12+4*(lex-1) for level 0 / 1 / >1.
    int  enter(); void leave();

    int  extra_cycles_ = 0;  // added to the opcode's base cost by step()

    int      last_reg_ = 0;        // ModR/M reg field of the last decode_modrm(), used by group opcodes
    uint16_t instr_start_ip_ = 0;  // restart IP for DIV/IDIV faults

    // Helpers used by step()'s switch. string_op, io_string_op, grp2_shift and
    // grp3_unary return their own cycle cost, which varies by sub-opcode and
    // iteration count.
    //
    // The 286 prefetch queue is not modeled. Intel publishes ranges for
    // queue-flushing transfers (Jcc taken 7-10, LOOP 8-11, CALL/JMP near 7-10,
    // far 11-14/13-16, RET 11-14/15-18, IRET 17-20; iAPX 286 PRM timing
    // appendix via Art of Assembly App. D), so each adds a flat tax on its
    // floor cost. 2 keeps floor+tax inside every range. Not-taken branches pay none.
    static constexpr int kQueueRefillTax = 2;

    bool cond(int cc) const;          // Jcc/LOOPcc condition, cc = opcode low nibble
    void jcc(bool taken);
    int  loop_group(uint8_t op);       // 0xE0-0xE3: LOOP/LOOPE/LOOPNE/JCXZ
    int  string_op(uint8_t op);       // 0xA4-0xA7, 0xAA-0xAF: MOVS/CMPS/STOS/LODS/SCAS, honors REP/REPE/REPNE
    int  io_string_op(uint8_t op);    // 0x6C-0x6F: INS/OUTS
    int  grp1_immed(uint8_t op);      // 0x80/0x81/0x83: ADD/OR/ADC/SBB/AND/SUB/XOR/CMP r/m,imm
    int  grp2_shift(uint8_t op);      // 0xC0/C1/D0-D3: shift/rotate group
    int  grp3_unary(uint8_t op);      // 0xF6/0xF7: TEST/NOT/NEG/MUL/IMUL/DIV/IDIV
    void grp5(uint8_t op);            // 0xFE/0xFF: INC/DEC/CALL/JMP/PUSH r/m
};

} // namespace cpu80286

#endif // IBMPCAT_CPU80286_H
