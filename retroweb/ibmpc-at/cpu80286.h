// Intel 80286 CPU core, real mode only, plus an 80386 compatibility layer:
// 32-bit registers via the 0x66 prefix, Jcc rel16, SETcc, two-operand IMUL,
// MOVZX/MOVSX. None of that exists on a real 286. BIOS-bochs-legacy assumes
// a 386+ baseline (IBM_PCAT_REVIEW.md §6), so the layer runs only for code
// fetched where firmware_at() says firmware lives; anywhere else it is #UD.
//
// No protected mode. The real-mode-legal system instructions (SGDT/SIDT/
// LGDT/LIDT, SMSW/LMSW, CLTS) work, but LMSW cannot set PE. LLDT/SLDT/LTR/
// STR/VERR/VERW/LAR/LSL/ARPL are #UD in real mode, as on the chip.
//
// The core talks only through Bus. Addresses are 24 bits and never wrapped at
// 1MB here, because the A20 gate is motherboard logic (8042 P21) in the chipset.
//
// Timings and semantics: Intel iAPX 286 Programmer's Reference Manual (1987),
// 80286 data sheet timing appendix, and the 8086/8088 User's Manual.

#ifndef IBMPCAT_CPU80286_H
#define IBMPCAT_CPU80286_H

#include <algorithm>
#include <cstdint>
#include <functional>

namespace cpu80286 {

// FLAGS bits.
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
    FLAG_IOPL = 3 << 12,  // I/O privilege level (2 bits), always 0 in real mode
    FLAG_NT   = 1 << 14,  // nested task, always 0 in real mode
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

    // Clocks per byte cycle by 4KB page and by port; 6+ is an 8-bit device.
    // Unset is the zero-wait bus the published instruction timings assume.
    const uint8_t* mem_clocks = nullptr;
    std::function<uint8_t(uint16_t port)> io_clocks;
    // Pages with kMemSlotted set hold the cycle until the device grants it:
    // returns the clocks a byte access starting at `now` takes.
    static constexpr uint8_t kMemSlotted = 0x80;
    std::function<int(uint32_t addr, uint64_t now)> mem_wait;
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

    // Called with (CS, IP, opcode-word) on an opcode firmware runs that this
    // core skips as a no-op: 0x00xx single-byte, 0x0Fxx two-byte. Also LOADALL
    // and an LMSW that tries to set PE.
    std::function<void(uint16_t cs, uint16_t ip, uint16_t opcode_word)> on_unimplemented;

    // True for physical addresses holding firmware that may use the 386 layer.
    // Unset means none does.
    std::function<bool(uint32_t addr)> firmware_at;

    // A REP string op hands back to the caller after this many cycles so
    // devices advance and interrupts land between iterations. 0 never yields.
    uint32_t rep_yield_cycles = 0;

    explicit Cpu(Bus bus) : bus_(std::move(bus)) {}

    // Reset vector F000:FFF0 (Intel iAPX 286 PRM, "Initialization").
    void reset();

    // Executes one instruction at CS:IP and returns its clock cycles (iAPX 286
    // PRM / 80286 data sheet). Shift, string and MUL/DIV groups compute their
    // own cost. The 5170-339's extra memory wait state is not modeled.
    int step();

    // Real-mode INT n: push FLAGS, CS, IP, clear IF and TF, jump through the
    // vector at vector*4. Wakes HLT. Costs nothing by itself; callers charge it.
    void interrupt(uint8_t vector);

    // INTR: INT n's cost plus the two INTA bus cycles. Returns its clocks. The
    // caller checks IF and the interrupt shadow first.
    int hardware_interrupt(uint8_t vector);

    // STI, MOV SS and POP SS hold off INTR for one instruction.
    bool interrupt_shadow() const { return shadow_; }

    uint16_t msw() const { return msw_; }

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

    // Exception vectors (Intel iAPX 286 PRM, real-address-mode exceptions).
    enum : uint8_t { EXC_DB = 1, EXC_UD = 6, EXC_NM = 7, EXC_SS = 12, EXC_GP = 13 };

    // A fault found mid-instruction: further bus traffic is dropped, and the
    // end of step() restores the registers and vectors through fault_.
    uint8_t fault_ = 0;
    void raise(uint8_t vec) { if (!fault_) fault_ = vec; }
    bool in_interrupt_ = false;
    // An operand that runs past offset FFFFh: #SS on the stack, #GP otherwise.
    void overrun(bool stack) { if (!in_interrupt_) raise(stack ? EXC_SS : EXC_GP); }

    // physical = seg*16 + off.
    uint32_t phys(uint16_t seg, uint16_t off) const { return (uint32_t(seg) << 4) + off; }
    // Bus time this step's data cycles took, and how much of it the published
    // counts don't already include (2 clocks per cycle).
    int bus_busy_ = 0, bus_extra_ = 0;
    void charge(int clocks) { bus_busy_ += clocks; bus_extra_ += clocks - 2; }
    int  mem_clk(uint32_t a) const { return bus_.mem_clocks ? bus_.mem_clocks[(a >> 12) & 0xFFF] & ~Bus::kMemSlotted : 2; }
    bool slotted(uint32_t a) const { return bus_.mem_clocks && (bus_.mem_clocks[(a >> 12) & 0xFFF] & Bus::kMemSlotted) && bus_.mem_wait; }
    int byte_clocks(uint32_t a, int after = 0) {
        int k = mem_clk(a);
        if (slotted(a)) k = std::max(k, bus_.mem_wait(a, cycles + uint64_t(bus_busy_ + after)));
        return k;
    }
    void charge_byte(uint32_t a) { charge(byte_clocks(a)); }
    int  io_clk(uint16_t p) const { return bus_.io_clocks ? bus_.io_clocks(p) : 2; }
    // A word is one cycle to a 16-bit device at an even address, two at an odd
    // one, and always two byte cycles to an 8-bit device.
    int  word_clocks(int k, bool odd) const { return (k >= 6 || odd) ? 2 * k : k; }
    void charge_word(uint32_t a) {
        if (slotted(a)) { int lo = byte_clocks(a); charge(lo + byte_clocks(a + 1, lo)); return; }
        charge(word_clocks(mem_clk(a), a & 1));
    }

    uint8_t  raw_rb(uint16_t seg, uint16_t off)            { return fault_ ? uint8_t(0xFF) : bus_.read(phys(seg, off)); }
    void     raw_wb(uint16_t seg, uint16_t off, uint8_t v) { if (!fault_) bus_.write(phys(seg, off), v); }
    uint8_t  rb(uint16_t seg, uint16_t off)            { charge_byte(phys(seg, off)); return raw_rb(seg, off); }
    void     wb(uint16_t seg, uint16_t off, uint8_t v) { charge_byte(phys(seg, off)); raw_wb(seg, off, v); }
    uint16_t rw(uint16_t seg, uint16_t off, bool stack = false) {
        if (off == 0xFFFF) overrun(stack);
        charge_word(phys(seg, off));
        return raw_rb(seg, off) | (uint16_t(raw_rb(seg, uint16_t(off + 1))) << 8);
    }
    void     ww(uint16_t seg, uint16_t off, uint16_t v, bool stack = false) {
        if (off == 0xFFFF) overrun(stack);
        charge_word(phys(seg, off));
        raw_wb(seg, off, v & 0xFF); raw_wb(seg, uint16_t(off + 1), v >> 8);
    }
    uint8_t  in8(uint16_t p)  { charge(io_clk(p)); return bus_.in(p); }
    void     out8(uint16_t p, uint8_t v) { charge(io_clk(p)); bus_.out(p, v); }
    uint16_t in16(uint16_t p) { charge(word_clocks(io_clk(p), p & 1)); return bus_.in16(p); }
    void     out16(uint16_t p, uint16_t v) { charge(word_clocks(io_clk(p), p & 1)); bus_.out16(p, v); }

    // 6-byte prefetch queue, filled a word at a time while the bus is idle
    // (Intel iAPX 286 PRM, bus unit). Published counts assume it's full.
    static constexpr int kQueueBytes = 6;
    int  queue_ = 0;
    int  fetch_progress_ = 0;  // clocks already spent on the word being fetched
    bool flushed_ = true;
    int  prefetch(int exec_clocks);
    uint32_t rd(uint16_t seg, uint16_t off, bool stack = false) {
        if (off > 0xFFFC) overrun(stack);
        return uint32_t(rw(seg, off)) | (uint32_t(rw(seg, uint16_t(off + 2))) << 16);
    }
    void     wd(uint16_t seg, uint16_t off, uint32_t v, bool stack = false) {
        if (off > 0xFFFC) overrun(stack);
        ww(seg, off, uint16_t(v & 0xFFFF)); ww(seg, uint16_t(off + 2), uint16_t(v >> 16));
    }

    // The 286 faults an instruction over 10 bytes or one that runs past FFFFh.
    int  fetched_ = 0;
    bool fetch_wrapped_ = false;
    uint8_t fetch8() {
        if (++fetched_ > 10 || fetch_wrapped_) raise(EXC_GP);
        if (ip == 0xFFFF) fetch_wrapped_ = true;
        return fault_ ? uint8_t(0xFF) : bus_.read(code_base() + ip++);
    }
    uint16_t fetch16() { uint16_t lo = fetch8(); return uint16_t(lo | (uint16_t(fetch8()) << 8)); }
    uint32_t fetch32() { uint32_t lo = fetch16(); return lo | (uint32_t(fetch16()) << 16); }

    // Stack is always SS. 32-bit push/pop moves SP by 4.
    void     push16(uint16_t v) { sp = uint16_t(sp - 2); ww(ss, uint16_t(sp), v, true); }
    uint16_t pop16()            { uint16_t v = rw(ss, uint16_t(sp), true); sp = uint16_t(sp + 2); return v; }
    // sp wraps at 64KB even for 32-bit operands (no 0x67 / big real mode).
    void     push32(uint32_t v) { sp = (sp - 4) & 0xFFFF; wd(ss, uint16_t(sp), v, true); }
    uint32_t pop32()            { uint32_t v = rd(ss, uint16_t(sp), true); sp = (sp + 4) & 0xFFFF; return v; }

    // Register state at the start of the instruction (or REP iteration) a
    // fault rolls back to.
    struct Snapshot {
        uint32_t ax, bx, cx, dx, sp, bp, si, di;
        uint16_t cs, ds, es, ss, flags, msw;
        bool cs_high;
        uint32_t gdt_base, idt_base;
        uint16_t gdt_limit, idt_limit;
        bool halted;
    };
    Snapshot snap_{};
    void save_regs();
    void restore_regs();

    bool firmware() const { return firmware_at && firmware_at(code_base() + instr_start_ip_); }

    // After RESET, CS's hidden base is FF0000h until the first far transfer
    // loads CS, so the first fetch is FFFFF0h (Intel iAPX 286 PRM, "Reset").
    bool cs_high_ = false;
    uint32_t code_base() const { return (cs_high_ && cs == 0xF000) ? 0xFF0000u : uint32_t(cs) << 4; }
    void ud() { raise(EXC_UD); }
    // #DE and #BR, found once the operands are in: vectors now and pays INT n like a fault.
    void exception(uint8_t vec) { ip = instr_start_ip_; interrupt(vec); extra_cycles_ += 23 + kQueueRefillTax; }

    // Machine status word and descriptor-table registers. MSW resets to FFF0h
    // and the IDT to base 0, limit 3FFh (Intel iAPX 286 PRM, "Reset").
    uint16_t msw_ = 0xFFF0;
    uint32_t gdt_base_ = 0, idt_base_ = 0;
    uint16_t gdt_limit_ = 0xFFFF, idt_limit_ = 0x03FF;
    enum : uint16_t { MSW_PE = 1, MSW_MP = 2, MSW_EM = 4, MSW_TS = 8 };
    int system_0f01();  // 0x0F 0x01: SGDT/SIDT/LGDT/LIDT/SMSW/LMSW

    bool shadow_ = false;
    bool vectored_ = false;      // this step entered a handler, so no single-step trap
    bool rep_resume_ = false;    // the last step yielded part-way through a REP
    bool rep_resumed_ = false;   // this step continues one, so setup is already paid
    bool step_each_ = false;     // TF set on a REP: one iteration per step
    bool rep_yield(int iterations, int per_iteration);

    int execute(uint8_t op, int c);

    // --- ModR/M decode ---
    // Register (is_mem=false) or memory operand with default-segment rules
    // applied (BP-based defaults to SS, 8086 manual table 2-19).
    struct RM {
        bool     is_mem;
        int      reg;      // valid when !is_mem
        uint16_t seg, off;  // valid when is_mem
        bool     stack;    // seg is SS, so an overrun is #SS
    };
    // Decodes ModR/M and any displacement at CS:IP, advancing IP.
    RM decode_modrm();
    uint8_t  rm_read8(const RM &rm)              { return rm.is_mem ? rb(rm.seg, rm.off) : get_reg8(rm.reg); }
    void     rm_write8(const RM &rm, uint8_t v)  { if (rm.is_mem) wb(rm.seg, rm.off, v); else set_reg8(rm.reg, v); }
    uint16_t rm_read16(const RM &rm)             { return rm.is_mem ? rw(rm.seg, rm.off, rm.stack) : get_reg16(rm.reg); }
    void     rm_write16(const RM &rm, uint16_t v){ if (rm.is_mem) ww(rm.seg, rm.off, v, rm.stack); else set_reg16(rm.reg, v); }
    uint32_t rm_read32(const RM &rm)             { return rm.is_mem ? rd(rm.seg, rm.off, rm.stack) : get_reg32(rm.reg); }
    void     rm_write32(const RM &rm, uint32_t v){ if (rm.is_mem) wd(rm.seg, rm.off, v, rm.stack); else set_reg32(rm.reg, v); }
    // Offset:segment pair (LES/LDS/far CALL/JMP/BOUND). Memory only; a register is #UD.
    bool     far_operand(const RM &rm, uint16_t &lo, uint16_t &hi) {
        if (!rm.is_mem) { ud(); return false; }
        if (rm.off > 0xFFFC) overrun(rm.stack);
        lo = rw(rm.seg, rm.off, rm.stack);
        hi = rw(rm.seg, uint16_t(rm.off + 2), rm.stack);
        return !fault_;
    }

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
    void push_reg(int idx);         // PUSH SP pushes the value from before the push
    void pusha(); void popa();      // 286-native PUSHA/POPA
    void bound();                   // 286-native BOUND r16, m16&16
    void imul_imm16(int dst_reg, const RM &rm, uint16_t imm);  // 286 IMUL r16,r/m16,imm
    void imul_imm32(int dst_reg, const RM &rm, uint32_t imm);  // 0x66-prefixed 32-bit form
    // enter() returns the nesting level; ENTER cost is 11 / 15 / 12+4*(lex-1) for level 0 / 1 / >1.
    int  enter(); void leave();

    int  extra_cycles_ = 0;  // added to the opcode's base cost by step()

    int      last_reg_ = 0;        // ModR/M reg field of the last decode_modrm(), used by group opcodes
    uint16_t instr_start_ip_ = 0;  // first prefix byte: where a fault restarts

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
    // A taken control transfer empties the prefetch queue, even one to the next instruction.
    bool transfer_ = false;
    int flush() { transfer_ = true; return kQueueRefillTax; }

    bool cond(int cc) const;          // Jcc/LOOPcc condition, cc = opcode low nibble
    void jcc(bool taken);
    int  loop_group(uint8_t op);       // 0xE0-0xE3: LOOP/LOOPE/LOOPNE/JCXZ
    int  string_op(uint8_t op);       // 0xA4-0xA7, 0xAA-0xAF: MOVS/CMPS/STOS/LODS/SCAS, honors REP/REPE/REPNE
    int  io_string_op(uint8_t op);    // 0x6C-0x6F: INS/OUTS
    int  grp1_immed(uint8_t op);      // 0x80/0x81/0x83: ADD/OR/ADC/SBB/AND/SUB/XOR/CMP r/m,imm
    int  grp2_shift(uint8_t op);      // 0xC0/C1/D0-D3: shift/rotate group
    int  grp3_unary(uint8_t op);      // 0xF6/0xF7: TEST/NOT/NEG/MUL/IMUL/DIV/IDIV
    int  grp5(uint8_t op);            // 0xFE/0xFF: INC/DEC/CALL/JMP/PUSH r/m
};

} // namespace cpu80286

#endif // IBMPCAT_CPU80286_H
