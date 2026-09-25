// Intel 80486DX2-66 CPU core -- implementation. Real address mode,
// protected mode, paging, and the on-die x87 FPU.
//
// Semantics are cited from the Intel 80486 Programmer's Reference Manual
// (1990/1992); where the 486 behaves identically to the 8086 the Intel
// 8086/8088 User's Manual is the reference instead, and AP-485 ("Intel
// Processor Identification and the CPUID Instruction") is cited for the
// 386-vs-486 detection behavior the AC flag exists to support.
//
// Cycle costs are the 486 column of the Quantasm "80x86 Integer
// Instruction Set (8088 - Pentium)" table, which reproduces Intel's own
// i486 PRM instruction-timing appendix. Three things about that column are
// worth stating plainly rather than implying a precision this core does
// not have:
//
//  1. Most 486 figures are single values, not ranges -- unlike the 286's
//     timing appendix, whose taken-branch entries are all ranges because
//     of prefetch-queue refill. The 486's own published control-transfer
//     costs (Jcc taken 3, JMP near 3, CALL near 3, RET 5, IRET 15) are
//     flat numbers with no "+m" term in the 486 column at all -- the +m
//     appears only in the 286 and 386 columns. So this core needs no
//     equivalent of cpu80286.h's kQueueRefillTax: the published 486
//     numbers are charged directly.
//  2. The one published per-addressing-mode adjustment is real and is
//     implemented: the table's legend says for the 286-486, an effective
//     address of the form base+index+displacement costs "+1" and all other
//     modes cost nothing extra. decode_modrm() adds exactly that, which is
//     also what makes LEA land on its published "1-2" range.
//  3. Four instruction families publish a data-dependent *range* instead
//     of a value, because the silicon's own work is data-dependent:
//     MUL/IMUL (early-out multiply), BSF/BSR (bit-at-a-time scan),
//     RCL/RCR by a count, and CMPXCHG with a memory operand. Each is
//     modelled by a named helper below, fitted to both published endpoints
//     and to the mechanism Intel documents -- labelled approximations of a
//     published range, not invented figures and not a claim of
//     cycle-accurate pipeline simulation. Everything else in this file is
//     a directly cited number.
//
// Deliberately preserved real-silicon behavior (documented, not bugs):
//   - PUSH SP/ESP pushes the value from *before* the decrement on the 286
//     and every later part, including the 486 -- the opposite of the 8086,
//     which pushes the already-decremented value. This is the classic
//     "push sp / pop ax / cmp ax,sp" 8086-vs-286+ runtime CPU check, so
//     getting the direction right matters. (Note for anyone comparing:
//     ibmpc-at's cpu80286.cpp implements this inverted, pushing the
//     decremented value.)
//   - Shift/rotate counts are masked mod 32.
//   - OF after a multi-bit shift/rotate (count != 1) is left *undefined*
//     by Intel's own documentation; this core leaves the flag untouched in
//     that case rather than guessing, which is itself the documented
//     contract rather than a gap.
//   - INC/DEC never affect CF.
//
// Protected mode, paging, task switching and the FPU are Milestone 2; see
// cpu80486.h's header for the scope, and PC486_REVIEW.md §6 for the design
// and the bugs building it turned up.
#include "cpu80486.h"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace cpu80486 {

namespace {

// Writable-bit masks for the flag-load instructions (Intel 80486 PRM,
// EFLAGS layout + POPF/POPFD description). Bits 0-11 hold the arithmetic
// and control flags; IOPL (12-13) and NT (14) are real in protected mode
// and storage-only in real mode; bit 15 and bits 19-31 are reserved and
// read 0 on a 486, except ID (21) on the parts that carry CPUID; AC (18) is
// the genuine 486 addition. RF (16) and VM (17) are not affected by
// POPF/POPFD on real hardware.
constexpr uint32_t kPopfMask  = 0x00007FD5u;  // 16-bit POPF: CF PF AF ZF SF TF IF DF OF IOPL NT
// 32-bit POPFD: the above plus AC (18) and ID (21) -- ID is writable exactly
// because this part has CPUID, which is what AP-485 has software test for.
constexpr uint32_t kPopfdMask = 0x00247FD5u;
constexpr uint32_t kSahfMask  = 0x000000D5u;  // SAHF loads only the low byte's defined flags
// IRETD additionally restores RF and VM from the stack image.
constexpr uint32_t kIretdMask = 0x00277FD5u;

// Descriptor access-byte (descriptor byte 5) decoding: P DPL S TYPE.
// Intel 80486 PRM, "Segment Descriptors".
inline bool acc_present(uint8_t a)     { return (a & 0x80) != 0; }
inline int  acc_dpl(uint8_t a)         { return (a >> 5) & 3; }
inline bool acc_system(uint8_t a)      { return (a & 0x10) == 0; }
inline bool acc_conforming(uint8_t a)  { return acc_code(a) && (a & 0x04) != 0; }
inline int  sys_type(uint8_t a)        { return a & 0x0F; }

// System-descriptor / gate type codes (Intel 80486 PRM, table
// "System Segment and Gate Descriptor Types").
enum SysType {
    SYS_TSS16_AVAIL = 1, SYS_LDT = 2, SYS_TSS16_BUSY = 3, SYS_CALL_GATE16 = 4,
    SYS_TASK_GATE = 5, SYS_INT_GATE16 = 6, SYS_TRAP_GATE16 = 7,
    SYS_TSS32_AVAIL = 9, SYS_TSS32_BUSY = 11, SYS_CALL_GATE32 = 12,
    SYS_INT_GATE32 = 14, SYS_TRAP_GATE32 = 15,
};

// Page-table entry bits.
constexpr uint32_t kPtePresent  = 1u << 0;
constexpr uint32_t kPteWritable = 1u << 1;
constexpr uint32_t kPteUser     = 1u << 2;
constexpr uint32_t kPteAccessed = 1u << 5;
constexpr uint32_t kPteDirty    = 1u << 6;

}  // namespace

void Cpu::init_state() {
    for (int i = 0; i < 8; ++i) {
        sd_[i] = SegDesc{};
        dr_[i] = 0;
    }
    for (int i = 0; i < 4; ++i) cr_[i] = 0;
    // CR0.ET is hardwired to 1 on an Intel486: the FPU is on-die, so the
    // "what kind of coprocessor is installed" question the 386 asked has
    // one permanent answer here (Intel 80486 PRM, "Control Registers").
    // SMSW and MOV EAX,CR0 therefore report ET=1 on this machine, unlike
    // Milestone 1's FPU-less core, which correctly reported 0.
    cr_[0] = CR0_ET;
    gdtr_ = DescTableReg{0, 0};
    // A 486 comes out of RESET with IDTR base 0 and limit 03FFh -- exactly
    // the 8086 interrupt-vector table, which is what makes real mode work
    // before any software has touched IDTR (Intel 80486 PRM, "Processor
    // Initialization", table of initial register values).
    idtr_ = DescTableReg{0, 0x03FF};
    ldtr_ = DescTableReg{0, 0};
    tr_ = DescTableReg{0, 0};
    ldt_sel_ = tr_sel_ = 0;
    tr_access_ = 0;
    cpl_ = 0;
    tlb_flush();
    page_map_flush();
    fpu_init();
    // CS is a code segment, not data: fetching reads it and a write through
    // a CS: override must fault once protection is on.
    sd_[SEG_CS].access = 0x9B;   // present, DPL 0, code, readable, accessed
    instr_start_ss_desc_ = sd_[SEG_SS];
}

void Cpu::reset() {
    eax = ebx = ecx = edx = esp = ebp = esi = edi = 0;
    ds = es = ss = fs = gs = 0;
    // Real 486 RESET state: CS:IP = F000:FFF0 (Intel 80486 PRM, "Processor
    // Initialization"). On real hardware CS's hidden descriptor base is
    // forced to FFFF0000h for this one load, so the first fetch is at
    // physical FFFFFFF0 -- the top of the 4GB space, which the
    // motherboard aliases down into the BIOS ROM window so POST can run
    // before any far jump reloads CS normally. This core sets CS=F000,
    // EIP=FFF0, giving physical FFFF0 directly. Equivalent for any BIOS
    // that, like every real one, immediately far-jumps to a normal
    // F000:xxxx entry point. Documented as a simplification in
    // PC486_REVIEW.md.
    cs = 0xF000;
    eip = 0xFFF0;
    eflags = FLAG_R1;
    halted = false;
    cycles = 0;
    seg_override_ = -1;
    rep_ = REP_NONE;
    opsize32_ = false;
    addrsize32_ = false;
    extra_cycles_ = 0;
    init_state();
    refresh_real_bases();
}

// --- faults ---------------------------------------------------------------

void Cpu::raise(int vector) { throw Fault{vector, 0, false}; }
void Cpu::raise_err(int vector, uint32_t error) { throw Fault{vector, error, true}; }
void Cpu::raise_sel(int vector, uint16_t selector) {
    // Selector-shaped error code: the selector's index and table bits with
    // RPL cleared, i.e. the selector with its low two bits zeroed (Intel
    // 80486 PRM, "Error Code"). The EXT bit is left clear here -- every
    // caller in this core is reporting a selector the *instruction* named.
    throw Fault{vector, uint32_t(selector & 0xFFFCu), true};
}

// --- interrupts and exceptions --------------------------------------------

void Cpu::do_interrupt(uint8_t vector, bool software, bool has_error, uint32_t error) {
    halted = false;
    if (protected_mode()) protected_mode_interrupt(vector, software, has_error, error);
    else real_mode_interrupt(vector);
}

void Cpu::real_mode_interrupt(uint8_t vector) {
    // Real-mode interrupt frame: 16-bit FLAGS, CS, IP. The frame width
    // follows the *mode*, not the CPU generation -- a 486 in real mode
    // pushes the same 6 bytes an 8086 does. (A 0x66-prefixed INT in real
    // mode would push a 32-bit frame on real hardware; that form is a
    // labelled gap here, since nothing in real-mode DOS uses it and the
    // matching IRETD would then have to agree.)
    //
    // The vector is bounds-checked against IDTR, not against a hardcoded
    // 1KB: a 386+ has a real IDTR in real mode too (base 0, limit 03FFh
    // out of RESET, so the default is exactly the 8086 IVT), and LIDT can
    // shrink it -- which is precisely how a DOS extender arms itself to
    // catch a stray real-mode interrupt.
    uint32_t off = uint32_t(vector) * 4;
    if (off + 3 > idtr_.limit) raise_err(EXC_GP, uint32_t(vector) * 8 + 2);
    push16(uint16_t(eflags));
    push16(cs);
    push16(uint16_t(eip));
    set_flag(FLAG_IF, false);
    set_flag(FLAG_TF, false);
    uint32_t vec = idtr_.base + off;
    uint16_t new_ip = phys_read8(vec) | (uint16_t(phys_read8(vec + 1)) << 8);
    uint16_t new_cs = phys_read8(vec + 2) | (uint16_t(phys_read8(vec + 3)) << 8);
    eip = new_ip;
    load_seg_real(SEG_CS, new_cs);
}

void Cpu::protected_mode_interrupt(uint8_t vector, bool software, bool has_error, uint32_t error) {
    // Intel 80486 PRM, "Interrupts and Exceptions" / "Protected-Mode
    // Interrupt Table". The IDT holds gates, not vectors: each entry is an
    // 8-byte task, interrupt or trap gate.
    uint32_t idx = uint32_t(vector) * 8;
    // The EXT ("external event") bit of a selector error code marks a
    // fault that was not caused by the instruction itself. A software INT
    // is the instruction's own doing; a hardware interrupt or a processor
    // exception is not.
    uint32_t ext = software ? 0u : 1u;
    if (idx + 7 > idtr_.limit) raise_err(EXC_GP, idx + 2 + ext);
    uint32_t glo = lin_read32(idtr_.base + idx, false);
    uint32_t ghi = lin_read32(idtr_.base + idx + 4, false);
    uint8_t gate_access = uint8_t((ghi >> 8) & 0xFF);
    if (!acc_present(gate_access)) raise_err(EXC_NP, idx + 2 + ext);
    if (!acc_system(gate_access)) raise_err(EXC_GP, idx + 2 + ext);
    int gtype = sys_type(gate_access);
    // "To prevent user programs from simulating interrupts with the INT
    // instruction, the DPL of an interrupt or trap gate must be greater
    // than or equal to CPL... This check is not made for exceptions or
    // external interrupts" (Intel 80486 PRM).
    if (software && acc_dpl(gate_access) < cpl()) raise_err(EXC_GP, idx + 2);

    if (gtype == SYS_TASK_GATE) {
        task_switch(uint16_t(glo >> 16), TaskLink::Call, has_error, error);
        return;
    }
    bool gate32;
    if (gtype == SYS_INT_GATE32 || gtype == SYS_TRAP_GATE32) gate32 = true;
    else if (gtype == SYS_INT_GATE16 || gtype == SYS_TRAP_GATE16) gate32 = false;
    else { raise_err(EXC_GP, idx + 2 + ext); }
    bool is_interrupt_gate = (gtype == SYS_INT_GATE16 || gtype == SYS_INT_GATE32);

    uint16_t target_sel = uint16_t(glo >> 16);
    uint32_t target_off = (glo & 0xFFFFu) | (gate32 ? (ghi & 0xFFFF0000u) : 0u);
    if ((target_sel & 0xFFFCu) == 0) raise_err(EXC_GP, ext);
    RawDesc cd = read_desc(target_sel, EXC_GP);
    uint8_t ca = uint8_t((cd.hi >> 8) & 0xFF);
    if (!acc_code(ca) || acc_dpl(ca) > cpl()) raise_sel(EXC_GP, target_sel);
    if (!acc_present(ca)) raise_sel(EXC_NP, target_sel);

    uint32_t old_eflags = eflags;
    uint16_t old_cs = cs, old_ss = ss;
    uint32_t old_eip = eip, old_esp = esp;
    uint16_t old_es = es, old_ds = ds, old_fs = fs, old_gs = gs;
    int target_dpl = acc_dpl(ca);

    // Leaving V86. The gate must be a 32-bit trap or interrupt gate pointing
    // at "a nonconforming, privilege-level zero, code segment" (Intel 80386
    // PRM, "Entering and Leaving Virtual 8086 Mode"), so the handler always
    // runs as ordinary protected-mode ring 0 on the TSS's ring-0 stack.
    bool from_v86 = v86_mode();
    if (from_v86) {
        if (!gate32 || acc_conforming(ca) || target_dpl != 0) raise_sel(EXC_GP, target_sel);
        // VM is cleared in the *live* flags -- "the processor stores the
        // current setting of EFLAGS on the stack, then clears the VM bit"
        // (same section). old_eflags, captured above, keeps VM = 1 so the
        // frame pushed below still says the interrupted code was an 8086
        // program and the matching IRETD can restore V86. Done here, before
        // any stack work, so every push below goes through the ordinary
        // protected-mode segmentation path.
        set_flag(FLAG_VM, false);
    }

    if (!acc_conforming(ca) && target_dpl < cpl()) {
        // Inter-privilege interrupt: the new stack comes from the current
        // TSS's SS/ESP slot for the target ring, and the old SS:ESP is
        // pushed onto it so IRET can restore the interrupted stack.
        uint16_t new_ss;
        uint32_t new_esp;
        if (tr_access_ == 0) raise_sel(EXC_TS, tr_sel_);
        bool tss32 = (sys_type(tr_access_) == SYS_TSS32_AVAIL || sys_type(tr_access_) == SYS_TSS32_BUSY);
        if (tss32) {
            new_esp = read_tss_dword(tr_.base, uint32_t(4 + target_dpl * 8));
            new_ss  = read_tss_word(tr_.base, uint32_t(8 + target_dpl * 8));
        } else {
            new_esp = read_tss_word(tr_.base, uint32_t(2 + target_dpl * 4));
            new_ss  = read_tss_word(tr_.base, uint32_t(4 + target_dpl * 4));
        }
        if ((new_ss & 0xFFFCu) == 0) raise_sel(EXC_TS, new_ss);
        RawDesc sdsc = read_desc(new_ss, EXC_TS);
        uint8_t sa = uint8_t((sdsc.hi >> 8) & 0xFF);
        if ((new_ss & 3) != target_dpl || acc_dpl(sa) != target_dpl || !acc_writable(sa))
            raise_sel(EXC_TS, new_ss);
        if (!acc_present(sa)) raise_sel(EXC_SS, new_ss);
        ss = new_ss;
        sd_[SEG_SS] = decode_desc(sdsc);
        sd_[SEG_SS].sel = new_ss;
        esp = new_esp;
        // CPL changes with CS, which is loaded below; the pushes here are
        // already on the new, more-privileged stack.
        cs = uint16_t((target_sel & 0xFFFCu) | uint16_t(target_dpl));
        sd_[SEG_CS] = decode_desc(cd);
        sd_[SEG_CS].sel = cs;
        cpl_ = uint8_t(target_dpl);
        // "The contents of all the 8086 segment registers are stored on the
        // PL 0 stack" (Intel 80386 PRM, "Entering and Leaving Virtual 8086
        // Mode"), below the SS:ESP pair, so the ring-0 frame reads
        // GS FS DS ES SS ESP EFLAGS CS EIP from high address to low -- the
        // nine doublewords IRETD's own return-to-V86 path pops back (Intel
        // 80486 PRM, IRET; the 386 PRM checks "the top 36 bytes" of the
        // stack for exactly this frame).
        if (from_v86) { push32(old_gs); push32(old_fs); push32(old_ds); push32(old_es); }
        if (gate32) { push32(old_ss); push32(old_esp); }
        else        { push16(old_ss); push16(uint16_t(old_esp)); }
    } else {
        cs = uint16_t((target_sel & 0xFFFCu) | uint16_t(cpl()));
        sd_[SEG_CS] = decode_desc(cd);
        sd_[SEG_CS].sel = cs;
    }
    if (gate32) { push32(old_eflags); push32(old_cs); push32(old_eip); }
    else        { push16(uint16_t(old_eflags)); push16(old_cs); push16(uint16_t(old_eip)); }
    if (has_error) { if (gate32) push32(error); else push16(uint16_t(error)); }

    if (from_v86) {
        // "After the processor stores all the 8086 segment registers on the
        // PL 0 stack, it loads all the segment registers with zeros before
        // starting to execute the handler procedure" (Intel 80386 PRM,
        // "Entering and Leaving Virtual 8086 Mode"). Unconditional, unlike
        // far_return()'s DPL-conditional sweep: there the four registers hold
        // real descriptors and only the over-privileged ones are unsafe,
        // while an 8086 segment value is not a usable selector at all.
        for (int si : {SEG_ES, SEG_DS, SEG_FS, SEG_GS}) load_seg(si, 0);
    }

    // An interrupt gate clears IF (the handler runs with interrupts off);
    // a trap gate leaves it alone. Both clear TF, NT and RF.
    if (is_interrupt_gate) set_flag(FLAG_IF, false);
    set_flag(FLAG_TF, false);
    set_flag(FLAG_NT, false);
    set_flag(FLAG_RF, false);
    set_ip(target_off);
}

int Cpu::interrupt(uint8_t vector) {
    // Externally delivered (hardware) interrupt: not a software INT, so
    // the gate-DPL-vs-CPL check does not apply.
    //
    // Delivery can itself fault -- a missing or malformed IDT gate, or a
    // stack that cannot take the frame -- and that fault must turn into a
    // #GP/#DF like any other, never escape to the embedding chipset, which
    // calls this from its instruction loop and has no idea what a
    // cpu80486::Fault is.
    uint32_t start_eip = eip;
    instr_start_esp_ = esp;
    instr_start_ss_ = ss;
    instr_start_ss_desc_ = sd_[SEG_SS];
    try {
        do_interrupt(vector, false);
    } catch (const Fault &f) {
        return deliver_fault(f, start_eip);
    }
    // A hardware-delivered interrupt does the same vectoring work INT3
    // does, without an immediate operand to fetch, so INT3's published 26
    // is the best-anchored figure for it (INT imm8 is 30). Charged here
    // and only here -- step()'s own INT/INT3/INTO paths call
    // do_interrupt() instead and charge their own published cost, so no
    // path is billed twice.
    cycles += 26;
    return 26;
}

// --- descriptors ----------------------------------------------------------

uint32_t Cpu::desc_base(const RawDesc &d) {
    // Descriptor bytes 2-4 and 7 hold the base, split the way they are
    // purely for 286 compatibility (Intel 80486 PRM, "Segment Descriptors").
    return ((d.lo >> 16) & 0x0000FFFFu) | ((d.hi & 0x000000FFu) << 16) | (d.hi & 0xFF000000u);
}

uint32_t Cpu::desc_limit(const RawDesc &d) {
    uint32_t lim = (d.lo & 0x0000FFFFu) | (d.hi & 0x000F0000u);
    // G (granularity, byte 6 bit 7): a 4KB-granular limit counts *pages*,
    // and the low 12 bits of the byte limit are then all ones -- which is
    // why a G=1 limit of 0 is a 4KB segment, not an empty one.
    if (d.hi & 0x00800000u) lim = (lim << 12) | 0x00000FFFu;
    return lim;
}

SegDesc Cpu::decode_desc(const RawDesc &d) {
    SegDesc s;
    s.base  = desc_base(d);
    s.limit = desc_limit(d);
    s.access = uint8_t((d.hi >> 8) & 0xFF);
    s.big   = ((d.hi & 0x00400000u) != 0);   // D/B, byte 6 bit 6
    s.null  = false;
    return s;
}

Cpu::RawDesc Cpu::read_desc(uint16_t selector, int fault_vector) {
    // Selector bit 2 (TI) picks the LDT over the GDT; bits 3-15 are the
    // index, bits 0-1 the RPL (Intel 80486 PRM, "Selectors").
    bool use_ldt = (selector & 0x0004u) != 0;
    uint32_t base  = use_ldt ? ldtr_.base : gdtr_.base;
    uint32_t limit = use_ldt ? ldtr_.limit : gdtr_.limit;
    if (use_ldt && (ldt_sel_ & 0xFFFCu) == 0) raise_sel(fault_vector, selector);
    uint32_t index = selector & 0xFFF8u;
    // The descriptor's last byte must be inside the table; a table limit is
    // the offset of its last valid byte, so a table holding N descriptors
    // has limit N*8-1.
    if (index + 7u > limit) raise_sel(fault_vector, selector);
    RawDesc d;
    d.lo = lin_read32(base + index, false);
    d.hi = lin_read32(base + index + 4, false);
    return d;
}

void Cpu::set_accessed(uint16_t selector) {
    // A successful segment load sets the descriptor's A bit in the table
    // itself -- the one write a segment load performs (Intel 80486 PRM,
    // "Accessed Bit").
    bool use_ldt = (selector & 0x0004u) != 0;
    uint32_t base = use_ldt ? ldtr_.base : gdtr_.base;
    uint32_t addr = base + (selector & 0xFFF8u) + 4;
    uint32_t hi = lin_read32(addr, true);
    if (!(hi & 0x00000100u)) lin_write32(addr, hi | 0x00000100u);
}

// --- segment register loading ---------------------------------------------

void Cpu::load_seg_real(int si, uint16_t selector) {
    // Real-mode segment load: the base becomes selector*16 and the cached
    // limit and access rights are left exactly as they were. That is
    // genuine hardware behavior, and it is the entire mechanism behind
    // "unreal mode" -- a memory manager loads a 4GB-limit descriptor during
    // a brief protected-mode excursion, drops back to real mode, and the
    // big limit survives the next segment load (PC486_REVIEW.md §5.4).
    seg_reg(si) = selector;
    sd_[si].sel = selector;
    sd_[si].base = uint32_t(selector) << 4;
    sd_[si].null = false;
    // A real-mode CS load leaves the internal CPL register at 0 regardless of
    // the segment value's low two bits. This is what lets a memory manager
    // set CR0.PE while running from an arbitrary real-mode CS -- FreeDOS's
    // HimemX does it from CS=0291h -- and then immediately load a DPL-0 data
    // selector without faulting (PC486_REVIEW.md §6.5). The same load in V86
    // lands at CPL 3 instead, which is what makes the IOPL-sensitive and
    // privileged instructions there trap to the monitor.
    if (si == SEG_CS) cpl_ = v86_mode() ? 3 : 0;
}

void Cpu::refresh_real_bases() {
    // The selector fields are public, so an embedding host or a test can
    // assign one directly -- and in real mode that is exactly a segment
    // load, so it must re-derive the base. Comparing against the selector
    // the cache was last loaded from is what distinguishes "software wrote
    // DS" (re-derive) from "DS still holds the selector a protected-mode
    // descriptor load put there" (keep the base, which is what lets a
    // protected-mode base as well as a limit survive into real mode).
    for (int i = 0; i < 6; ++i) {
        uint16_t cur = seg_sel(i);
        if (sd_[i].sel != cur) {
            sd_[i].sel = cur;
            sd_[i].base = uint32_t(cur) << 4;
            sd_[i].null = false;
            if (i == SEG_CS) cpl_ = 0;
        }
    }
}

void Cpu::load_seg(int si, uint16_t selector) {
    if (real_addressing()) { load_seg_real(si, selector); return; }
    if ((selector & 0xFFFCu) == 0) {
        // A null selector may be loaded into DS/ES/FS/GS: the register
        // becomes unusable and only an actual *access* through it faults,
        // which is how a protected-mode OS parks a segment register it is
        // not using. SS can never be null (Intel 80486 PRM, "Null
        // Selector").
        if (si == SEG_SS) raise_err(EXC_GP, 0);
        sd_[si] = SegDesc{};
        sd_[si].access = 0;
        sd_[si].null = true;
        sd_[si].sel = selector;
        seg_reg(si) = selector;
        return;
    }
    RawDesc d = read_desc(selector, (si == SEG_SS) ? EXC_SS : EXC_GP);
    uint8_t a = uint8_t((d.hi >> 8) & 0xFF);
    int rpl = selector & 3;
    if (si == SEG_SS) {
        // The stack segment is the strictest load in the architecture: it
        // must be a writable data segment, its RPL must *equal* CPL, and
        // its DPL must equal CPL too -- a stack at the wrong privilege
        // level is exactly the hole inter-ring transfers exist to close.
        if (acc_system(a) || !acc_writable(a) || rpl != cpl() || acc_dpl(a) != cpl())
            raise_sel(EXC_GP, selector);
        if (!acc_present(a)) raise_sel(EXC_SS, selector);
    } else {
        // DS/ES/FS/GS take a data segment or a *readable* code segment.
        if (acc_system(a) || (acc_code(a) && !acc_readable(a))) raise_sel(EXC_GP, selector);
        // Privilege: for anything but a conforming code segment, the less
        // privileged of CPL and the selector's own RPL must still be at
        // least as privileged as the descriptor's DPL.
        if (!acc_conforming(a)) {
            int eff = (cpl() > rpl) ? cpl() : rpl;
            if (eff > acc_dpl(a)) raise_sel(EXC_GP, selector);
        }
        if (!acc_present(a)) raise_sel(EXC_NP, selector);
    }
    set_accessed(selector);
    seg_reg(si) = selector;
    sd_[si] = decode_desc(d);
    sd_[si].sel = selector;
}

// --- paging ---------------------------------------------------------------

void Cpu::tlb_flush() {
    for (int i = 0; i < kTlbEntries; ++i) tlb_[i].valid = false;
    ++tlb_gen_;
}

void Cpu::tlb_invalidate(uint32_t linear) {
    uint32_t vpn = linear >> 12;
    TlbEntry &e = tlb_[vpn & (kTlbEntries - 1)];
    if (e.valid && e.tag == vpn) e.valid = false;
    ++tlb_gen_;
}

uint32_t Cpu::translate_slow(uint32_t linear, bool write, bool user) {
    if (!paging_enabled()) return linear;
    uint32_t vpn = linear >> 12;
    TlbEntry &e = tlb_[vpn & (kTlbEntries - 1)];
    if (e.valid && e.tag == vpn) {
        // The cached rights are the page's own U/S and R/W bits, so the same
        // rule the walk below applies has to be applied here too -- including
        // CR0.WP's supervisor exemption. Leaving it out made a multi-byte
        // supervisor write to a read-only page succeed on its first byte
        // (which walks) and fault on its second (which hits the TLB); see
        // PC486_REVIEW.md §6.5.
        bool denied = (user && !(e.rights & 2)) ||
                      (write && !(e.rights & 1) && (user || (cr_[0] & CR0_WP)));
        // A cached entry answers a read outright, and a write only once the
        // PTE's D bit is already set -- otherwise the walk has to run again
        // to set it, because software genuinely watches that bit.
        if (!denied && (!write || e.dirty)) return e.frame | (linear & 0x00000FFFu);
        if (denied) {
            cr_[2] = linear;
            raise_err(EXC_PF, 1u | (write ? 2u : 0u) | (user ? 4u : 0u));
        }
    }
    // Two-level walk: CR3 -> page directory -> page table (Intel 80486 PRM,
    // "Page Translation"). Both levels are indexed 10 bits at a time and
    // both tables are themselves page-aligned physical addresses, so no
    // recursion into segment translation happens here.
    uint32_t pde_addr = (cr_[3] & 0xFFFFF000u) + ((linear >> 22) << 2);
    uint32_t pde = phys_read32(pde_addr);
    if (!(pde & kPtePresent)) {
        cr_[2] = linear;
        raise_err(EXC_PF, (write ? 2u : 0u) | (user ? 4u : 0u));
    }
    uint32_t pte_addr = (pde & 0xFFFFF000u) + (((linear >> 12) & 0x3FFu) << 2);
    uint32_t pte = phys_read32(pte_addr);
    if (!(pte & kPtePresent)) {
        cr_[2] = linear;
        raise_err(EXC_PF, (write ? 2u : 0u) | (user ? 4u : 0u));
    }
    // U/S and R/W are the AND of the two levels: a directory entry's rights
    // bound every page under it, which is what makes a whole 4MB region
    // protectable with one write.
    bool user_ok  = (pde & kPteUser) && (pte & kPteUser);
    bool writable = (pde & kPteWritable) && (pte & kPteWritable);
    if (user && !user_ok) {
        cr_[2] = linear;
        raise_err(EXC_PF, 1u | (write ? 2u : 0u) | 4u);
    }
    if (write && !writable) {
        // A *supervisor* write to a read-only page succeeds unless CR0.WP
        // is set. WP is a genuine Intel486 addition: on a 386 a supervisor
        // write always bypassed the R/W bit, which is precisely why
        // copy-on-write could not be implemented without it.
        if (user || (cr_[0] & CR0_WP)) {
            cr_[2] = linear;
            raise_err(EXC_PF, 1u | 2u | (user ? 4u : 0u));
        }
    }
    if (!(pde & kPteAccessed)) phys_write32(pde_addr, pde | kPteAccessed);
    uint32_t new_pte = pte | kPteAccessed | (write ? kPteDirty : 0u);
    if (new_pte != pte) { phys_write32(pte_addr, new_pte); pte = new_pte; }
    e.valid = true;
    e.tag = vpn;
    e.frame = pte & 0xFFFFF000u;
    e.rights = uint8_t((writable ? 1 : 0) | (user_ok ? 2 : 0));
    e.dirty = (pte & kPteDirty) != 0;
    return e.frame | (linear & 0x00000FFFu);
}

// --- memory access --------------------------------------------------------

uint32_t Cpu::seg_linear_slow(int si, uint32_t off, int size, bool write) {
    const SegDesc &s = sd_[si & 7];
    // Real mode has no descriptors to enforce, so there is no limit check
    // at all -- the deliberate decision §4.3/§5.4 of PC486_REVIEW.md
    // records, and the reason a 32-bit offset in real mode reaches the bus
    // untruncated.
    if (!protected_mode()) return s.base + off;
    int fault = (si == SEG_SS) ? EXC_SS : EXC_GP;
    if (s.null) raise_err(fault, 0);
    if (write) {
        if (!acc_writable(s.access)) raise_err(fault, 0);
    } else if (!acc_readable(s.access)) {
        raise_err(fault, 0);
    }
    uint32_t last = off + uint32_t(size) - 1u;
    if (last < off) raise_err(fault, 0);   // the access wrapped past 2^32
    if (acc_expand_down(s.access)) {
        // Expand-down segment (a stack meant to grow downward): the valid
        // offsets are limit+1 up to the segment's maximum, the inverse of
        // the ordinary rule (Intel 80486 PRM, "Expand-Down Data Segments").
        uint32_t top = s.big ? 0xFFFFFFFFu : 0x0000FFFFu;
        if (off <= s.limit || last > top) raise_err(fault, 0);
    } else if (last > s.limit) {
        raise_err(fault, 0);
    }
    return s.base + off;
}

void Cpu::page_map_flush() {
    for (int i = 0; i < kPageMapEntries; ++i) {
        rmap_[i] = PageMap{};
        wmap_[i] = PageMap{};
    }
    map_epoch_seen_ = bus_.map_epoch != nullptr ? *bus_.map_epoch : 0;
    prefetch_clear();
}

uint8_t Cpu::read8(int si, uint32_t off) {
    uint32_t p = translate(seg_linear(si, off, 1, false), false, cpl() == 3);
    if (const uint8_t *h = host_ptr(p, false)) return *h;
    return bus_read(p);
}
void Cpu::write8(int si, uint32_t off, uint8_t v) {
    uint32_t p = translate(seg_linear(si, off, 1, true), true, cpl() == 3);
    if (uint8_t *h = host_ptr(p, true)) { *h = v; return; }
    bus_write(p, v);
}
bool Cpu::access_phys(int si, uint32_t off, int size, bool write, uint32_t &phys) {
    uint32_t last = off + uint32_t(size) - 1u;
    if (last < off) return false;                                  // wraps past 2^32
    if (seg_off(off, uint32_t(size) - 1u) != last) return false;   // real mode's 64KB wrap
    uint32_t lin = seg_linear(si, off, size, write);
    if ((lin & 0xFFFu) > 0x1000u - uint32_t(size)) return false;   // straddles two pages
    phys = translate(lin, write, cpl() == 3);
    return true;
}

uint16_t Cpu::read16(int si, uint32_t off) {
    uint32_t p;
    if (access_phys(si, off, 2, false, p)) {
        if (const uint8_t *h = host_ptr(p, false))
            return uint16_t(uint32_t(h[0]) | (uint32_t(h[1]) << 8));
        return uint16_t(uint32_t(bus_read(p)) | (uint32_t(bus_read(p + 1)) << 8));
    }
    return uint16_t(uint32_t(read8(si, off)) | (uint32_t(read8(si, seg_off(off, 1))) << 8));
}
uint32_t Cpu::read32(int si, uint32_t off) {
    uint32_t p;
    if (access_phys(si, off, 4, false, p)) {
        if (const uint8_t *h = host_ptr(p, false))
            return uint32_t(h[0]) | (uint32_t(h[1]) << 8) |
                   (uint32_t(h[2]) << 16) | (uint32_t(h[3]) << 24);
        return uint32_t(bus_read(p)) | (uint32_t(bus_read(p + 1)) << 8) |
               (uint32_t(bus_read(p + 2)) << 16) | (uint32_t(bus_read(p + 3)) << 24);
    }
    return uint32_t(read16(si, off)) | (uint32_t(read16(si, seg_off(off, 2))) << 16);
}
uint64_t Cpu::read64(int si, uint32_t off) {
    uint32_t p;
    if (access_phys(si, off, 8, false, p)) {
        uint64_t v = 0;
        if (const uint8_t *h = host_ptr(p, false)) {
            for (int i = 0; i < 8; ++i) v |= uint64_t(h[i]) << (8 * i);
            return v;
        }
        for (int i = 0; i < 8; ++i) v |= uint64_t(bus_read(p + uint32_t(i))) << (8 * i);
        return v;
    }
    return uint64_t(read32(si, off)) | (uint64_t(read32(si, seg_off(off, 4))) << 32);
}
void Cpu::write16(int si, uint32_t off, uint16_t v) {
    // Both halves are limit-checked and translated *before* either byte is
    // stored, so a 16-bit write straddling a page or segment boundary
    // cannot leave one byte written and then fault -- which is what real
    // hardware guarantees, and what makes a #PF restartable.
    uint32_t p;
    if (access_phys(si, off, 2, true, p)) {
        if (uint8_t *h = host_ptr(p, true)) {
            h[0] = uint8_t(v & 0xFF);
            h[1] = uint8_t(v >> 8);
            return;
        }
        bus_write(p, uint8_t(v & 0xFF));
        bus_write(p + 1, uint8_t(v >> 8));
        return;
    }
    uint32_t o1 = seg_off(off, 1);
    uint32_t p0 = translate(seg_linear(si, off, 1, true), true, cpl() == 3);
    uint32_t p1 = translate(seg_linear(si, o1, 1, true), true, cpl() == 3);
    bus_write(p0, uint8_t(v & 0xFF));
    bus_write(p1, uint8_t(v >> 8));
}
void Cpu::write32(int si, uint32_t off, uint32_t v) {
    uint32_t p;
    if (access_phys(si, off, 4, true, p)) {
        if (uint8_t *h = host_ptr(p, true)) {
            for (int i = 0; i < 4; ++i) h[i] = uint8_t((v >> (8 * i)) & 0xFF);
            return;
        }
        for (int i = 0; i < 4; ++i) bus_write(p + uint32_t(i), uint8_t((v >> (8 * i)) & 0xFF));
        return;
    }
    uint32_t o[4] = {off, seg_off(off, 1), seg_off(off, 2), seg_off(off, 3)};
    uint32_t q[4];
    for (int i = 0; i < 4; ++i) q[i] = translate(seg_linear(si, o[i], 1, true), true, cpl() == 3);
    for (int i = 0; i < 4; ++i) bus_write(q[i], uint8_t((v >> (8 * i)) & 0xFF));
}
void Cpu::write64(int si, uint32_t off, uint64_t v) {
    uint32_t p;
    if (access_phys(si, off, 8, true, p)) {
        if (uint8_t *h = host_ptr(p, true)) {
            for (int i = 0; i < 8; ++i) h[i] = uint8_t((v >> (8 * i)) & 0xFF);
            return;
        }
        for (int i = 0; i < 8; ++i) bus_write(p + uint32_t(i), uint8_t((v >> (8 * i)) & 0xFF));
        return;
    }
    uint32_t o[8];
    uint32_t q[8];
    for (int i = 0; i < 8; ++i) o[i] = seg_off(off, uint32_t(i));
    for (int i = 0; i < 8; ++i) q[i] = translate(seg_linear(si, o[i], 1, true), true, cpl() == 3);
    for (int i = 0; i < 8; ++i) bus_write(q[i], uint8_t((v >> (8 * i)) & 0xFF));
}

uint32_t Cpu::phys_read32(uint32_t a) {
    return uint32_t(bus_read(a)) | (uint32_t(bus_read(a + 1)) << 8) |
           (uint32_t(bus_read(a + 2)) << 16) | (uint32_t(bus_read(a + 3)) << 24);
}
void Cpu::phys_write32(uint32_t a, uint32_t v) {
    bus_write(a, uint8_t(v & 0xFF));
    bus_write(a + 1, uint8_t((v >> 8) & 0xFF));
    bus_write(a + 2, uint8_t((v >> 16) & 0xFF));
    bus_write(a + 3, uint8_t((v >> 24) & 0xFF));
}
// The descriptor tables, page tables and TSS are addressed linearly by the
// CPU itself, never through a segment -- and always as supervisor accesses,
// whatever CPL the interrupted program was running at.
uint16_t Cpu::lin_read16(uint32_t linear, bool write_access) {
    uint8_t lo = bus_read(translate(linear, write_access, false));
    uint8_t hi = bus_read(translate(linear + 1, write_access, false));
    return uint16_t(lo | (uint16_t(hi) << 8));
}
uint32_t Cpu::lin_read32(uint32_t linear, bool write_access) {
    return uint32_t(lin_read16(linear, write_access)) |
           (uint32_t(lin_read16(linear + 2, write_access)) << 16);
}
void Cpu::lin_write16(uint32_t linear, uint16_t v) {
    uint32_t p0 = translate(linear, true, false);
    uint32_t p1 = translate(linear + 1, true, false);
    bus_write(p0, uint8_t(v & 0xFF));
    bus_write(p1, uint8_t(v >> 8));
}
void Cpu::lin_write32(uint32_t linear, uint32_t v) {
    lin_write16(linear, uint16_t(v & 0xFFFF));
    lin_write16(linear + 2, uint16_t(v >> 16));
}

// Resolves the run of EIPs that share the current code page, so the rest of
// the instruction's bytes come out of a host pointer instead of repeating
// the limit check and the page walk per byte. Declines (leaving an empty
// window, so every fetch takes the full path) whenever the run is not a
// plain forward run inside one page of an ordinary readable code segment.
void Cpu::prefetch_fill() {
    prefetch_clear();
    // Both halves of the optional bulk-access path or neither: the window's
    // revalidation reads *bus_.map_epoch unconditionally, so a Bus without it
    // never gets a window.
    if (bus_.page == nullptr || bus_.map_epoch == nullptr) return;
    uint32_t lin = seg_linear(SEG_CS, eip, 1, false);  // still faults past the limit
    uint32_t span = 0x1000u - (lin & 0xFFFu);
    if (real_addressing()) {
        // Real mode's (and V86's) fixed 64KB limit: the window must stop at the wrap.
        if (eip <= 0xFFFFu && span > 0x10000u - eip) span = 0x10000u - eip;
    } else {
        const SegDesc &s = sd_[SEG_CS];
        if (s.null || acc_expand_down(s.access) || !acc_readable(s.access)) return;
        if (eip > s.limit) return;
        if (span > s.limit - eip + 1u) span = s.limit - eip + 1u;
    }
    if (span == 0 || eip + span < eip) return;  // no wrap past 2^32 inside a window
    uint8_t *h = host_ptr(translate(lin, false, cpl() == 3), false);
    if (h == nullptr) return;  // code on the VGA window or in open bus
    pf_base_ = h;
    pf_lo_ = eip;
    pf_hi_ = eip + span;
    pf_cs_ = sd_[SEG_CS];
    pf_cr0_ = cr_[0];
    pf_cr3_ = cr_[3];
    pf_cpl_ = cpl_;
    pf_tlb_gen_ = tlb_gen_;
    pf_map_epoch_ = *bus_.map_epoch;
}

uint8_t Cpu::fetch8_slow() {
    prefetch_fill();
    if (eip >= pf_lo_ && eip < pf_hi_) return fetch8();
    uint8_t v = read8(SEG_CS, eip);
    eip = (eip + 1) & ip_mask();
    return v;
}
uint16_t Cpu::fetch16_slow() {
    prefetch_fill();
    if (eip >= pf_lo_ && eip < pf_hi_ && pf_hi_ - eip >= 2u) return fetch16();
    uint16_t v = read16(SEG_CS, eip);
    eip = (eip + 2) & ip_mask();
    return v;
}
uint32_t Cpu::fetch32_slow() {
    prefetch_fill();
    if (eip >= pf_lo_ && eip < pf_hi_ && pf_hi_ - eip >= 4u) return fetch32();
    uint32_t v = read32(SEG_CS, eip);
    eip = (eip + 4) & ip_mask();
    return v;
}

// --- stack ----------------------------------------------------------------

void Cpu::add_sp(int32_t delta) {
    if (stack32()) esp = uint32_t(esp + uint32_t(delta));
    else set_sp(uint16_t(int32_t(sp()) + delta));
}
void Cpu::push16(uint16_t v) {
    if (stack32()) { esp = esp - 2; write16(SEG_SS, esp, v); }
    else { set_sp(uint16_t(sp() - 2)); write16(SEG_SS, sp(), v); }
}
uint16_t Cpu::pop16() {
    if (stack32()) { uint16_t v = read16(SEG_SS, esp); esp = esp + 2; return v; }
    uint16_t v = read16(SEG_SS, sp());
    set_sp(uint16_t(sp() + 2));
    return v;
}
void Cpu::push32(uint32_t v) {
    if (stack32()) { esp = esp - 4; write32(SEG_SS, esp, v); }
    else { set_sp(uint16_t(sp() - 4)); write32(SEG_SS, sp(), v); }
}
uint32_t Cpu::pop32() {
    if (stack32()) { uint32_t v = read32(SEG_SS, esp); esp = esp + 4; return v; }
    uint32_t v = read32(SEG_SS, sp());
    set_sp(uint16_t(sp() + 4));
    return v;
}

// --- I/O privilege --------------------------------------------------------

void Cpu::check_io_permission(uint16_t port, int size) {
    // IN/OUT/INS/OUTS are unrestricted at CPL <= IOPL. Above that the TSS's
    // I/O permission bitmap decides, one bit per port, and a bit that is
    // *set* denies access (Intel 80486 PRM, "I/O Permission Bit Map").
    // Real mode has no privilege at all, so nothing to check.
    if (!protected_mode()) return;
    // V86 is the exception to the IOPL shortcut: "the protection mechanism
    // does not consult IOPL when executing the I/O instructions IN, INS, OUT,
    // OUTS. Only the I/O permission bit map controls the right for V86 tasks
    // to execute these I/O instructions" (Intel 80386 PRM, "Virtual I/O"), so
    // even at IOPL 3 a V86 task's ports go through the map below.
    if (!v86_mode() && cpl() <= iopl()) return;
    if (tr_access_ == 0) raise_err(EXC_GP, 0);
    bool tss32 = (sys_type(tr_access_) == SYS_TSS32_AVAIL || sys_type(tr_access_) == SYS_TSS32_BUSY);
    if (!tss32) raise_err(EXC_GP, 0);   // a 16-bit TSS has no I/O map at all
    uint16_t map_base = read_tss_word(tr_.base, 102);
    if (map_base == 0 || uint32_t(map_base) >= tr_.limit) raise_err(EXC_GP, 0);
    for (int i = 0; i < size; ++i) {
        uint32_t bit = uint32_t(port) + uint32_t(i);
        uint32_t off = uint32_t(map_base) + (bit >> 3);
        if (off > tr_.limit) raise_err(EXC_GP, 0);
        uint8_t byte = bus_read(translate(tr_.base + off, false, false));
        if (byte & (1u << (bit & 7))) raise_err(EXC_GP, 0);
    }
}

// --- far transfers --------------------------------------------------------
//
// Published 486 costs (Quantasm 486 column, which carries separate real-
// mode and protected-mode rows for the far control transfers): JMP far 17
// real / 19 protected, CALL far 18 real / 20 protected, RET far 13 and 17
// when the privilege level changes, IRET 15 real / 36 protected. The
// call-gate and task-switch paths are an order of magnitude more expensive
// because of the descriptor and TSS work; the single figures charged for
// them below are this core's labelled readings covering every sub-case,
// not separately cited numbers per sub-case.
namespace {
constexpr int kCallGateCost   = 35;
constexpr int kJmpGateCost    = 32;
// A task switch saves and reloads the entire register file plus six
// segment descriptors and (with paging on) CR3, which makes it the single
// most expensive operation in the instruction set. Charged as one labelled
// figure for every task-switch path -- JMP, CALL, interrupt through a task
// gate, and IRET's return -- rather than asserting a cited number this
// core cannot separate into its published sub-cases.
constexpr int kTaskSwitchCost = 199;
}  // namespace

int Cpu::far_transfer(uint16_t selector, uint32_t offset, bool is_call) {
    if (real_addressing()) {   // real mode, and V86, where CS behaves as on an 8086
        if (is_call) { push16(cs); push16(uint16_t(eip)); }
        load_seg_real(SEG_CS, selector);
        eip = offset & 0xFFFFu;
        return is_call ? 18 : 17;
    }
    if ((selector & 0xFFFCu) == 0) raise_err(EXC_GP, 0);
    RawDesc d = read_desc(selector, EXC_GP);
    uint8_t a = uint8_t((d.hi >> 8) & 0xFF);

    if (!acc_system(a)) {
        if (!acc_code(a)) raise_sel(EXC_GP, selector);   // a data segment is not executable
        int dpl = acc_dpl(a);
        int rpl = selector & 3;
        // A conforming code segment may be entered from any less privileged
        // level and keeps the caller's CPL; a non-conforming one demands an
        // exact CPL match (Intel 80486 PRM, "Direct Calls or Jumps to Code
        // Segments").
        if (acc_conforming(a)) {
            if (dpl > cpl()) raise_sel(EXC_GP, selector);
        } else {
            if (rpl > cpl() || dpl != cpl()) raise_sel(EXC_GP, selector);
        }
        if (!acc_present(a)) raise_sel(EXC_NP, selector);
        if (is_call) {
            if (opsize32_) { push32(cs); push32(eip); }
            else { push16(cs); push16(uint16_t(eip)); }
        }
        set_accessed(selector);
        // CS's RPL field *is* CPL, so a same-privilege transfer keeps the
        // current CPL rather than whatever RPL the selector carried.
        cs = uint16_t((selector & 0xFFFCu) | uint16_t(cpl()));
        sd_[SEG_CS] = decode_desc(d);
        sd_[SEG_CS].sel = cs;
        // CPL is unchanged by a same-privilege transfer, but re-stating it
        // keeps the internal register and CS's RPL field in agreement, which
        // is the invariant every later check relies on.
        cpl_ = uint8_t(cs & 3);
        set_ip(offset);
        return is_call ? 20 : 19;
    }

    int t = sys_type(a);
    if (t == SYS_TSS16_AVAIL || t == SYS_TSS32_AVAIL) {
        task_switch(selector, is_call ? TaskLink::Call : TaskLink::Jmp, false, 0);
        return kTaskSwitchCost;
    }
    if (t == SYS_TASK_GATE) {
        if (acc_dpl(a) < cpl() || acc_dpl(a) < (selector & 3)) raise_sel(EXC_GP, selector);
        if (!acc_present(a)) raise_sel(EXC_NP, selector);
        task_switch(uint16_t(d.lo >> 16), is_call ? TaskLink::Call : TaskLink::Jmp, false, 0);
        return kTaskSwitchCost;
    }
    if (t == SYS_CALL_GATE16 || t == SYS_CALL_GATE32) {
        bool gate32 = (t == SYS_CALL_GATE32);
        // The gate itself is the access-controlled object: its DPL must
        // admit both the caller's CPL and the selector's RPL.
        if (acc_dpl(a) < cpl() || acc_dpl(a) < (selector & 3)) raise_sel(EXC_GP, selector);
        if (!acc_present(a)) raise_sel(EXC_NP, selector);
        uint16_t target_sel = uint16_t(d.lo >> 16);
        uint32_t target_off = (d.lo & 0xFFFFu) | (gate32 ? (d.hi & 0xFFFF0000u) : 0u);
        // Descriptor byte 4 (the low byte of the high dword) holds the
        // gate's dword/word parameter count in its low 5 bits.
        int param_count = int(d.hi & 0x1Fu);
        if ((target_sel & 0xFFFCu) == 0) raise_err(EXC_GP, 0);
        RawDesc cd = read_desc(target_sel, EXC_GP);
        uint8_t ca = uint8_t((cd.hi >> 8) & 0xFF);
        if (!acc_code(ca)) raise_sel(EXC_GP, target_sel);
        int target_dpl = acc_dpl(ca);
        if (target_dpl > cpl()) raise_sel(EXC_GP, target_sel);
        if (!acc_present(ca)) raise_sel(EXC_NP, target_sel);
        bool to_inner = is_call && !acc_conforming(ca) && target_dpl < cpl();
        if (!is_call && !acc_conforming(ca) && target_dpl != cpl()) {
            // A JMP through a call gate may not change privilege -- only a
            // CALL can, because only a CALL leaves a return path behind.
            raise_sel(EXC_GP, target_sel);
        }
        uint16_t old_cs = cs, old_ss = ss;
        uint32_t old_eip = eip, old_esp = esp;
        if (to_inner) {
            if (tr_access_ == 0) raise_sel(EXC_TS, tr_sel_);
            bool tss32 = (sys_type(tr_access_) == SYS_TSS32_AVAIL || sys_type(tr_access_) == SYS_TSS32_BUSY);
            uint32_t new_esp;
            uint16_t new_ss;
            if (tss32) {
                new_esp = read_tss_dword(tr_.base, uint32_t(4 + target_dpl * 8));
                new_ss  = read_tss_word(tr_.base, uint32_t(8 + target_dpl * 8));
            } else {
                new_esp = read_tss_word(tr_.base, uint32_t(2 + target_dpl * 4));
                new_ss  = read_tss_word(tr_.base, uint32_t(4 + target_dpl * 4));
            }
            if ((new_ss & 0xFFFCu) == 0) raise_sel(EXC_TS, new_ss);
            RawDesc sdsc = read_desc(new_ss, EXC_TS);
            uint8_t sa = uint8_t((sdsc.hi >> 8) & 0xFF);
            if ((new_ss & 3) != target_dpl || acc_dpl(sa) != target_dpl || !acc_writable(sa))
                raise_sel(EXC_TS, new_ss);
            if (!acc_present(sa)) raise_sel(EXC_SS, new_ss);
            // Read the parameters off the *old* stack before switching, then
            // copy them onto the new one -- the whole point of a call gate's
            // parameter count.
            uint32_t params[31];
            for (int i = 0; i < param_count; ++i) {
                params[i] = gate32 ? read32(SEG_SS, old_esp + uint32_t(i) * 4)
                                   : read16(SEG_SS, old_esp + uint32_t(i) * 2);
            }
            ss = new_ss;
            sd_[SEG_SS] = decode_desc(sdsc);
            sd_[SEG_SS].sel = new_ss;
            esp = new_esp;
            cs = uint16_t((target_sel & 0xFFFCu) | uint16_t(target_dpl));
            sd_[SEG_CS] = decode_desc(cd);
            sd_[SEG_CS].sel = cs;
            cpl_ = uint8_t(target_dpl);
            if (gate32) { push32(old_ss); push32(old_esp); }
            else { push16(old_ss); push16(uint16_t(old_esp)); }
            for (int i = param_count - 1; i >= 0; --i) {
                if (gate32) push32(params[i]); else push16(uint16_t(params[i]));
            }
            if (gate32) { push32(old_cs); push32(old_eip); }
            else { push16(old_cs); push16(uint16_t(old_eip)); }
        } else {
            if (is_call) {
                if (gate32) { push32(old_cs); push32(old_eip); }
                else { push16(old_cs); push16(uint16_t(old_eip)); }
            }
            cs = uint16_t((target_sel & 0xFFFCu) | uint16_t(cpl()));
            sd_[SEG_CS] = decode_desc(cd);
            sd_[SEG_CS].sel = cs;
            cpl_ = uint8_t(cs & 3);
        }
        set_ip(target_off);
        return is_call ? kCallGateCost : kJmpGateCost;
    }
    raise_sel(EXC_GP, selector);
}

int Cpu::far_return(uint32_t stack_adjust, bool is_iret) {
    if (real_addressing()) {
        // Real mode, and V86, where a far return is an 8086 far return. IRET
        // itself is IOPL-sensitive in V86 -- "CPL is always three in V86 mode;
        // therefore, if IOPL < 3, these instructions will trigger a
        // general-protection exception" -- specifically so the monitor can
        // control the interrupted routine's interrupt-enable flag (Intel 80386
        // PRM, "Additional Sensitive Instructions" / "Virtualizing the
        // Interrupt-Enable Flag").
        if (is_iret && v86_mode() && iopl() != 3) raise_err(EXC_GP, 0);
        // A V86 task cannot change VM or IOPL: the 8086 flags image it pops
        // has neither, and honoring bit 17 out of one would drop the task out
        // of the mode it is running in.
        uint32_t keep = v86_mode() ? uint32_t(FLAG_VM | FLAG_IOPL) : 0u;
        if (is_iret) {
            if (opsize32_) {
                set_ip(pop32());
                load_seg_real(SEG_CS, uint16_t(pop32()));
                eflags = ((pop32() & kIretdMask & ~keep) | (eflags & keep)) | FLAG_R1;
            } else {
                set_ip(pop16());
                load_seg_real(SEG_CS, pop16());
                eflags = (eflags & 0xFFFF0000u) |
                         ((uint32_t(pop16()) & kPopfMask & ~keep) | (eflags & keep & 0xFFFFu)) | FLAG_R1;
            }
        } else {
            uint32_t off = opsize32_ ? pop32() : pop16();
            uint16_t sel = opsize32_ ? uint16_t(pop32()) : pop16();
            load_seg_real(SEG_CS, sel);
            set_ip(off);
        }
        if (stack_adjust) add_sp(int32_t(stack_adjust));
        // Published real-mode 486: IRET 15, RET far 13, RET far imm16 14.
        return is_iret ? 15 : (stack_adjust ? 14 : 13);
    }
    // IRET with NT set does not return from a procedure at all -- it
    // returns from a *task*, through the back-link the switch that got here
    // wrote into the current TSS (Intel 80486 PRM, "Returning from a Nested
    // Task").
    if (is_iret && flag(FLAG_NT)) {
        uint16_t back_link = read_tss_word(tr_.base, 0);
        task_switch(back_link, TaskLink::Iret, false, 0);
        return kTaskSwitchCost;
    }
    uint32_t new_eip;
    uint16_t new_cs;
    uint32_t new_flags = eflags;
    const int entry_cpl = cpl();   // the level that executed the return; see the flag load below
    if (opsize32_) {
        new_eip = pop32();
        new_cs = uint16_t(pop32());
        if (is_iret) new_flags = pop32();
    } else {
        new_eip = pop16();
        new_cs = pop16();
        if (is_iret) new_flags = (eflags & 0xFFFF0000u) | pop16();
    }
    // IRETD from CPL 0 with VM set in the popped flags image returns to a
    // virtual-8086 task: "a value of one in VM in this case indicates that the
    // procedure to which control is being returned is an 8086 procedure. The
    // CPL at the time the IRET is executed must be zero, else the processor
    // does not change VM" (Intel 80386 PRM, "Entering and Leaving Virtual 8086
    // Mode"). A separate path from the returns below, because new_cs is an
    // 8086 segment value rather than a selector, so none of their descriptor
    // checks can run on it. Pops ESP, SS, ES, DS, FS, GS -- the mirror of the
    // frame protected_mode_interrupt() pushed (Intel 80486 PRM, IRET).
    if (is_iret && opsize32_ && (new_flags & FLAG_VM) && entry_cpl == 0) {
        uint32_t new_esp = pop32();
        uint16_t n_ss = uint16_t(pop32());
        uint16_t n_es = uint16_t(pop32()), n_ds = uint16_t(pop32());
        uint16_t n_fs = uint16_t(pop32()), n_gs = uint16_t(pop32());
        // VM goes live before the segment loads, so each one takes the 8086
        // path and CS's load lands at CPL 3.
        eflags = (new_flags & kIretdMask) | FLAG_R1;
        const int order[6] = {SEG_CS, SEG_SS, SEG_ES, SEG_DS, SEG_FS, SEG_GS};
        const uint16_t sel[6] = {new_cs, n_ss, n_es, n_ds, n_fs, n_gs};
        for (int i = 0; i < 6; ++i) {
            load_seg_real(order[i], sel[i]);
            SegDesc &s = sd_[order[i]];
            // load_seg_real() keeps the cached limit and D/B bit, which is the
            // "unreal mode" quirk real mode depends on (PC486_REVIEW.md §5.4)
            // and exactly what a V86 task must not inherit from the monitor:
            // it is an 8086, so every segment is a 16-bit 64KB one at
            // privilege level 3.
            s.limit = 0xFFFFu;
            s.big = false;
            s.access = uint8_t(order[i] == SEG_CS ? 0xFB : 0xF3);
        }
        esp = new_esp;
        set_ip(new_eip);
        return 36;   // published protected-mode IRET
    }
    if ((new_cs & 0xFFFCu) == 0) raise_err(EXC_GP, 0);
    RawDesc d = read_desc(new_cs, EXC_GP);
    uint8_t a = uint8_t((d.hi >> 8) & 0xFF);
    int rpl = new_cs & 3;
    if (!acc_code(a) || rpl < cpl()) raise_sel(EXC_GP, new_cs);
    if (acc_conforming(a) ? (acc_dpl(a) > rpl) : (acc_dpl(a) != rpl)) raise_sel(EXC_GP, new_cs);
    if (!acc_present(a)) raise_sel(EXC_NP, new_cs);

    bool outward = (rpl > cpl());
    if (outward) {
        // Returning outward: the caller's own SS:ESP is further up this
        // stack, past the parameters the gate copied, and must be popped
        // too.
        if (stack_adjust) add_sp(int32_t(stack_adjust));
        uint32_t new_esp;
        uint16_t new_ss;
        if (opsize32_) { new_esp = pop32(); new_ss = uint16_t(pop32()); }
        else { new_esp = pop16(); new_ss = pop16(); }
        if ((new_ss & 0xFFFCu) == 0) raise_sel(EXC_GP, new_ss);
        RawDesc sdsc = read_desc(new_ss, EXC_GP);
        uint8_t sa = uint8_t((sdsc.hi >> 8) & 0xFF);
        if ((new_ss & 3) != rpl || !acc_writable(sa) || acc_dpl(sa) != rpl)
            raise_sel(EXC_GP, new_ss);
        if (!acc_present(sa)) raise_sel(EXC_SS, new_ss);
        cs = new_cs;
        sd_[SEG_CS] = decode_desc(d);
        sd_[SEG_CS].sel = new_cs;
        cpl_ = uint8_t(rpl);   // before the segment-nulling sweep below, which reads cpl()
        ss = new_ss;
        sd_[SEG_SS] = decode_desc(sdsc);
        sd_[SEG_SS].sel = new_ss;
        esp = new_esp;
        set_ip(new_eip);
        // Any segment register still holding a segment the *inner*, more
        // privileged code could reach but this outer level cannot must be
        // nulled, or the return would silently hand out privilege (Intel
        // 80486 PRM, "Returning from a Procedure").
        for (int si : {SEG_ES, SEG_DS, SEG_FS, SEG_GS}) {
            const SegDesc &s = sd_[si];
            if (s.null || (s.access == 0)) continue;
            if (acc_conforming(s.access)) continue;
            if (acc_dpl(s.access) < cpl()) {
                sd_[si] = SegDesc{};
                sd_[si].access = 0;
                sd_[si].null = true;
                sd_[si].sel = 0;
                seg_reg(si) = 0;
            }
        }
    } else {
        cs = new_cs;
        sd_[SEG_CS] = decode_desc(d);
        sd_[SEG_CS].sel = new_cs;
        cpl_ = uint8_t(rpl);
        set_ip(new_eip);
        if (stack_adjust) add_sp(int32_t(stack_adjust));
    }
    if (is_iret) {
        // IOPL is only writable at CPL 0, and IF only when CPL <= IOPL --
        // an IRET from a less privileged level silently keeps the old
        // values rather than faulting (Intel 80486 PRM, "IRET"). VM goes with
        // IOPL: outside CPL 0 the processor leaves it alone, which is why the
        // V86-entry path above is the only way into the mode.
        //
        // Both rules test the CPL that *executed* the IRET, not the one being
        // returned to, so an outward return from ring 0 does load IOPL out of
        // the stack image -- which is how a monitor hands a ring-3 task an
        // IOPL in the first place. (Same as Bochs' iret_protected, which
        // builds its flag change mask from `prev_cpl`.)
        uint32_t mask = opsize32_ ? kIretdMask : kPopfMask;
        uint32_t keep = 0;
        if (entry_cpl > 0) keep |= FLAG_IOPL | FLAG_VM;
        if (entry_cpl > iopl()) keep |= FLAG_IF;
        eflags = ((new_flags & mask & ~keep) | (eflags & keep) |
                  (opsize32_ ? 0u : (eflags & 0xFFFF0000u))) | FLAG_R1;
    }
    // Published protected-mode 486: IRET 36, RET far 13 within a privilege
    // level and 17 across one (18 with an immediate).
    if (is_iret) return 36;
    if (outward) return stack_adjust ? 18 : 17;
    return stack_adjust ? 14 : 13;
}

// --- task switching -------------------------------------------------------

uint32_t Cpu::read_tss_dword(uint32_t tss_base, uint32_t off) { return lin_read32(tss_base + off, false); }
uint16_t Cpu::read_tss_word(uint32_t tss_base, uint32_t off) { return lin_read16(tss_base + off, false); }

void Cpu::task_switch(uint16_t tss_selector, TaskLink link, bool has_error, uint32_t error) {
    // Intel 80486 PRM, "Task Switching". A task switch is one indivisible
    // operation: it saves the whole outgoing register file into the
    // outgoing TSS, marks the descriptors' busy bits, loads the incoming
    // TSS, and only then reloads the segment registers with full
    // protection checks.
    if (tss_selector & 0x0004u) raise_sel(EXC_GP, tss_selector);  // a TSS lives in the GDT, never the LDT
    if ((tss_selector & 0xFFFCu) == 0) raise_sel(EXC_GP, tss_selector);
    RawDesc nd = read_desc(tss_selector, EXC_GP);
    uint8_t na = uint8_t((nd.hi >> 8) & 0xFF);
    if (!acc_system(na)) raise_sel(EXC_GP, tss_selector);
    int nt = sys_type(na);
    bool new_is32;
    if (link == TaskLink::Iret) {
        // The task being returned to is by definition still marked busy.
        if (nt == SYS_TSS32_BUSY) new_is32 = true;
        else if (nt == SYS_TSS16_BUSY) new_is32 = false;
        else { raise_sel(EXC_TS, tss_selector); }
    } else {
        if (nt == SYS_TSS32_AVAIL) new_is32 = true;
        else if (nt == SYS_TSS16_AVAIL) new_is32 = false;
        else { raise_sel(EXC_GP, tss_selector); }
    }
    if (!acc_present(na)) raise_sel(EXC_NP, tss_selector);
    uint32_t new_base = desc_base(nd);
    uint32_t new_limit = desc_limit(nd);
    // A 32-bit TSS is 104 bytes of architectural state, a 16-bit one 44;
    // anything smaller cannot hold a task (Intel 80486 PRM: "#TS if TSS
    // segment limit less than 67h" for the 32-bit form).
    if (new_limit < (new_is32 ? 0x67u : 0x2Bu)) raise_sel(EXC_TS, tss_selector);
    // A TSS whose saved EFLAGS has VM set describes a virtual-8086 task, and
    // entering V86 through a task gate is a deliberate, documented gap (see
    // cpu80486.h's header): the CS load below is a bespoke descriptor lookup
    // that would read an 8086 segment value as a selector and take CPL from
    // its low two bits. Reporting the TSS as invalid stops here instead, with
    // nothing yet committed -- a period V86 memory manager enters the mode
    // with an IRETD inside one task, which far_return() implements. (A 16-bit
    // TSS has no room for EFLAGS' upper half, so it cannot ask for this.)
    if (new_is32 && (read_tss_dword(new_base, 36) & FLAG_VM)) raise_sel(EXC_TS, tss_selector);

    // 1. Save the outgoing task's state. TR must already be valid -- a task
    // switch out of "no task at all" is a software error, not a bootstrap
    // case: LTR is what makes the CPU consider itself inside a task.
    if (tr_access_ == 0) raise_sel(EXC_TS, tr_sel_);
    bool old_is32 = (sys_type(tr_access_) == SYS_TSS32_AVAIL || sys_type(tr_access_) == SYS_TSS32_BUSY);
    uint32_t ob = tr_.base;
    // The NT flag saved into the outgoing TSS is cleared on an IRET-driven
    // return, because the nesting that flag recorded is exactly what is
    // being unwound.
    uint32_t save_flags = eflags;
    if (link == TaskLink::Iret) save_flags &= ~uint32_t(FLAG_NT);
    if (old_is32) {
        lin_write32(ob + 32, eip);
        lin_write32(ob + 36, save_flags);
        lin_write32(ob + 40, eax);  lin_write32(ob + 44, ecx);
        lin_write32(ob + 48, edx);  lin_write32(ob + 52, ebx);
        lin_write32(ob + 56, esp);  lin_write32(ob + 60, ebp);
        lin_write32(ob + 64, esi);  lin_write32(ob + 68, edi);
        lin_write16(ob + 72, es);   lin_write16(ob + 76, cs);
        lin_write16(ob + 80, ss);   lin_write16(ob + 84, ds);
        lin_write16(ob + 88, fs);   lin_write16(ob + 92, gs);
    } else {
        lin_write16(ob + 14, uint16_t(eip));
        lin_write16(ob + 16, uint16_t(save_flags));
        lin_write16(ob + 18, uint16_t(eax)); lin_write16(ob + 20, uint16_t(ecx));
        lin_write16(ob + 22, uint16_t(edx)); lin_write16(ob + 24, uint16_t(ebx));
        lin_write16(ob + 26, uint16_t(esp)); lin_write16(ob + 28, uint16_t(ebp));
        lin_write16(ob + 30, uint16_t(esi)); lin_write16(ob + 32, uint16_t(edi));
        lin_write16(ob + 34, es); lin_write16(ob + 36, cs);
        lin_write16(ob + 38, ss); lin_write16(ob + 40, ds);
    }

    // 2. Busy bits and the back link. A JMP hands the CPU over and clears
    // the outgoing task's busy bit; a CALL (or an interrupt through a task
    // gate) nests, leaving the outgoing task busy and recording it in the
    // incoming TSS's back link plus EFLAGS.NT; an IRET unwinds, clearing
    // the outgoing task's busy bit as it leaves.
    auto set_busy = [&](uint16_t sel, bool busy) {
        uint32_t addr = gdtr_.base + (sel & 0xFFF8u) + 4;
        uint32_t hi = lin_read32(addr, true);
        uint32_t type = (hi >> 8) & 0x0Fu;
        type = busy ? (type | 0x2u) : (type & ~0x2u);
        lin_write32(addr, (hi & ~0x00000F00u) | (type << 8));
    };
    if (link == TaskLink::Jmp || link == TaskLink::Iret) set_busy(tr_sel_, false);
    if (link == TaskLink::Call) lin_write16(new_base + 0, tr_sel_);
    if (link != TaskLink::Iret) set_busy(tss_selector, true);

    // 3. TR now points at the incoming task, and CR0.TS is set so the first
    // FPU instruction in it traps #NM and the handler can swap FPU state.
    tr_sel_ = tss_selector;
    tr_.base = new_base;
    tr_.limit = uint16_t(new_limit > 0xFFFFu ? 0xFFFFu : new_limit);
    tr_access_ = uint8_t((na & 0xF0u) | uint8_t(new_is32 ? SYS_TSS32_BUSY : SYS_TSS16_BUSY));
    cr_[0] |= CR0_TS;

    // 4. Load the incoming task's state.
    uint16_t new_ldt, n_es, n_cs, n_ss, n_ds, n_fs = 0, n_gs = 0;
    uint32_t new_eip, new_flags;
    if (new_is32) {
        uint32_t new_cr3 = read_tss_dword(new_base, 28);
        new_eip   = read_tss_dword(new_base, 32);
        new_flags = read_tss_dword(new_base, 36);
        eax = read_tss_dword(new_base, 40); ecx = read_tss_dword(new_base, 44);
        edx = read_tss_dword(new_base, 48); ebx = read_tss_dword(new_base, 52);
        uint32_t new_esp = read_tss_dword(new_base, 56);
        ebp = read_tss_dword(new_base, 60);
        esi = read_tss_dword(new_base, 64); edi = read_tss_dword(new_base, 68);
        n_es = read_tss_word(new_base, 72); n_cs = read_tss_word(new_base, 76);
        n_ss = read_tss_word(new_base, 80); n_ds = read_tss_word(new_base, 84);
        n_fs = read_tss_word(new_base, 88); n_gs = read_tss_word(new_base, 92);
        new_ldt = read_tss_word(new_base, 96);
        // CR3 is per-task: each task can have its own page tables, which is
        // the whole reason CR3 lives in the TSS. Only reloaded with paging
        // actually enabled.
        if (paging_enabled()) { cr_[3] = new_cr3; tlb_flush(); }
        esp = new_esp;
    } else {
        new_eip   = read_tss_word(new_base, 14);
        new_flags = (eflags & 0xFFFF0000u) | read_tss_word(new_base, 16);
        set_reg16(0, read_tss_word(new_base, 18)); set_reg16(1, read_tss_word(new_base, 20));
        set_reg16(2, read_tss_word(new_base, 22)); set_reg16(3, read_tss_word(new_base, 24));
        uint16_t new_sp = read_tss_word(new_base, 26);
        set_reg16(5, read_tss_word(new_base, 28));
        set_reg16(6, read_tss_word(new_base, 30)); set_reg16(7, read_tss_word(new_base, 32));
        n_es = read_tss_word(new_base, 34); n_cs = read_tss_word(new_base, 36);
        n_ss = read_tss_word(new_base, 38); n_ds = read_tss_word(new_base, 40);
        new_ldt = read_tss_word(new_base, 42);
        set_sp(new_sp);
    }
    if (link == TaskLink::Call) new_flags |= FLAG_NT;
    eflags = (new_flags & 0x003F7FD5u) | FLAG_R1;

    // 5. LDTR first: the incoming task's segment selectors may well be LDT
    // selectors, so the LDT has to be in place before any of them load.
    ldt_sel_ = new_ldt;
    if ((new_ldt & 0xFFFCu) != 0) {
        RawDesc ld = read_desc(new_ldt, EXC_TS);
        uint8_t la = uint8_t((ld.hi >> 8) & 0xFF);
        if (!acc_system(la) || sys_type(la) != SYS_LDT) raise_sel(EXC_TS, new_ldt);
        if (!acc_present(la)) raise_sel(EXC_TS, new_ldt);
        ldtr_.base = desc_base(ld);
        ldtr_.limit = uint16_t(desc_limit(ld) > 0xFFFFu ? 0xFFFFu : desc_limit(ld));
    } else {
        ldtr_.base = 0;
        ldtr_.limit = 0;
    }

    // 6. CS before the data segments, because CPL comes from CS and every
    // data-segment privilege check reads it.
    if ((n_cs & 0xFFFCu) == 0) raise_sel(EXC_TS, n_cs);
    RawDesc cd = read_desc(n_cs, EXC_TS);
    uint8_t ca = uint8_t((cd.hi >> 8) & 0xFF);
    if (!acc_code(ca)) raise_sel(EXC_TS, n_cs);
    if (acc_conforming(ca) ? (acc_dpl(ca) > (n_cs & 3)) : (acc_dpl(ca) != (n_cs & 3)))
        raise_sel(EXC_TS, n_cs);
    if (!acc_present(ca)) raise_sel(EXC_NP, n_cs);
    cs = n_cs;
    sd_[SEG_CS] = decode_desc(cd);
    sd_[SEG_CS].sel = n_cs;
    cpl_ = uint8_t(n_cs & 3);   // before the data-segment loads, which check against CPL
    set_ip(new_eip);

    load_seg(SEG_SS, n_ss);
    load_seg(SEG_DS, n_ds);
    load_seg(SEG_ES, n_es);
    if (new_is32) { load_seg(SEG_FS, n_fs); load_seg(SEG_GS, n_gs); }

    // 7. An exception delivered through a task gate still owes its handler
    // the error code, pushed on the *new* task's stack.
    if (has_error) {
        if (new_is32) push32(error); else push16(uint16_t(error));
    }
}

// --- register file ----------------------------------------------------

// get_reg8/16/32 and set_reg8/16/32 are defined at the bottom of
// cpu80486.h -- see the general-register file table there.

uint16_t &Cpu::seg_reg(int idx) {
    switch (idx) {
        case SEG_ES: return es;
        case SEG_CS: return cs;
        case SEG_SS: return ss;
        case SEG_DS: return ds;
        case SEG_FS: return fs;
        default:     return gs;
    }
}
uint16_t Cpu::seg_sel(int idx) const {
    switch (idx) {
        case SEG_ES: return es;
        case SEG_CS: return cs;
        case SEG_SS: return ss;
        case SEG_DS: return ds;
        case SEG_FS: return fs;
        default:     return gs;
    }
}

// --- ModR/M decode ------------------------------------------------------

Cpu::RM Cpu::decode_modrm_slow(uint8_t modrm) {
    int mod = (modrm >> 6) & 3;
    int rm = modrm & 7;

    RM out;
    out.reg = 0;
    out.is_mem = true;
    uint32_t ea = 0;
    bool default_ss = false;
    bool has_base = false, has_index = false, has_disp = false;

    if (addrsize32_) {
        // 32-bit addressing (Intel 80486 PRM, "Addressing Modes" / the
        // 32-bit ModR/M and SIB tables). Reached via the 0x67 prefix,
        // which a 486 honors in real mode exactly as in protected mode --
        // the prefix selects the *addressing form*, and has nothing to do
        // with what mode the CPU is in. cpu80286.h deliberately omitted
        // all of this; here it is native behavior.
        int base_reg = -1, index_reg = -1, scale = 0;
        int disp_size = 0;  // 0, 1 or 4 bytes
        if (rm == 4) {
            uint8_t sib = fetch8();
            scale = (sib >> 6) & 3;
            int idx = (sib >> 3) & 7;
            int b = sib & 7;
            // index == 4 (ESP) encodes "no index register" -- ESP can
            // never be an index on real hardware.
            if (idx != 4) index_reg = idx;
            if (b == 5 && mod == 0) disp_size = 4;  // no base register, disp32 only
            else base_reg = b;
        } else if (rm == 5 && mod == 0) {
            disp_size = 4;  // disp32 with no base register
        } else {
            base_reg = rm;
        }
        if (mod == 1) disp_size = 1;
        else if (mod == 2) disp_size = 4;

        uint32_t disp = 0;
        if (disp_size == 1) disp = uint32_t(int32_t(int8_t(fetch8())));
        else if (disp_size == 4) disp = fetch32();

        if (base_reg >= 0) { ea += get_reg32(base_reg); has_base = true; }
        if (index_reg >= 0) { ea += get_reg32(index_reg) << scale; has_index = true; }
        ea += disp;
        has_disp = (disp_size != 0);
        // ESP- or EBP-based addressing defaults to SS, everything else to
        // DS (Intel 80486 PRM, "Default Segment Attribute").
        default_ss = has_base && (base_reg == 4 || base_reg == 5);
    } else {
        // 16-bit addressing (Intel 8086 manual table 2-19, unchanged
        // through the 486). Each intermediate sum wraps mod 64KB.
        uint16_t a16 = 0;
        bool disp_only = false;
        switch (rm) {
            case 0: a16 = uint16_t(ebx + esi); has_base = has_index = true; break;
            case 1: a16 = uint16_t(ebx + edi); has_base = has_index = true; break;
            case 2: a16 = uint16_t(ebp + esi); has_base = has_index = true; default_ss = true; break;
            case 3: a16 = uint16_t(ebp + edi); has_base = has_index = true; default_ss = true; break;
            case 4: a16 = uint16_t(esi); has_base = true; break;
            case 5: a16 = uint16_t(edi); has_base = true; break;
            case 6:
                if (mod == 0) { a16 = fetch16(); disp_only = true; has_disp = true; }
                else { a16 = uint16_t(ebp); has_base = true; default_ss = true; }
                break;
            default: a16 = uint16_t(ebx); has_base = true; break;  // rm == 7
        }
        if (!disp_only) {
            if (mod == 1) { a16 = uint16_t(a16 + int16_t(int8_t(fetch8()))); has_disp = true; }
            else if (mod == 2) { a16 = uint16_t(a16 + fetch16()); has_disp = true; }
        }
        ea = a16;
    }

    // Published 486 effective-address penalty: the base+index+displacement
    // form costs one extra clock, every other form costs nothing extra
    // (Quantasm table legend, "286 - 486: base+index+disp = +1, all
    // others, no penalty"). Charged once per decoded memory operand.
    if (has_base && has_index && has_disp) extra_cycles_ += 1;

    int seg = default_ss ? int(SEG_SS) : int(SEG_DS);
    if (seg_override_ >= 0) seg = seg_override_;
    out.seg = uint8_t(seg);
    // The offset reaches the bus as computed, untruncated -- see RM::off.
    out.off = ea;
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
uint32_t Cpu::add32(uint32_t a, uint32_t b, bool carry_in) {
    uint64_t r = uint64_t(a) + uint64_t(b) + (carry_in ? 1u : 0u);
    uint32_t res = uint32_t(r);
    set_flag(FLAG_CF, r > 0xFFFFFFFFull);
    set_flag(FLAG_AF, ((a ^ b ^ res) & 0x10) != 0);
    set_flag(FLAG_OF, ((~(a ^ b)) & (a ^ res) & 0x80000000u) != 0);
    set_pzs32(res);
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
uint32_t Cpu::sub32(uint32_t a, uint32_t b, bool borrow_in) {
    uint64_t bb = uint64_t(b) + (borrow_in ? 1u : 0u);
    uint32_t res = uint32_t(uint64_t(a) - bb);
    set_flag(FLAG_CF, uint64_t(a) < bb);
    set_flag(FLAG_AF, ((a ^ b ^ res) & 0x10) != 0);
    set_flag(FLAG_OF, ((a ^ b) & (a ^ res) & 0x80000000u) != 0);
    set_pzs32(res);
    return res;
}
uint8_t Cpu::and8(uint8_t a, uint8_t b) {
    uint8_t r = uint8_t(a & b);
    set_flag(FLAG_CF, false); set_flag(FLAG_OF, false); set_pzs8(r);
    return r;
}
uint16_t Cpu::and16(uint16_t a, uint16_t b) {
    uint16_t r = uint16_t(a & b);
    set_flag(FLAG_CF, false); set_flag(FLAG_OF, false); set_pzs16(r);
    return r;
}
uint32_t Cpu::and32(uint32_t a, uint32_t b) {
    uint32_t r = a & b;
    set_flag(FLAG_CF, false); set_flag(FLAG_OF, false); set_pzs32(r);
    return r;
}
uint8_t Cpu::or8(uint8_t a, uint8_t b) {
    uint8_t r = uint8_t(a | b);
    set_flag(FLAG_CF, false); set_flag(FLAG_OF, false); set_pzs8(r);
    return r;
}
uint16_t Cpu::or16(uint16_t a, uint16_t b) {
    uint16_t r = uint16_t(a | b);
    set_flag(FLAG_CF, false); set_flag(FLAG_OF, false); set_pzs16(r);
    return r;
}
uint32_t Cpu::or32(uint32_t a, uint32_t b) {
    uint32_t r = a | b;
    set_flag(FLAG_CF, false); set_flag(FLAG_OF, false); set_pzs32(r);
    return r;
}
uint8_t Cpu::xor8(uint8_t a, uint8_t b) {
    uint8_t r = uint8_t(a ^ b);
    set_flag(FLAG_CF, false); set_flag(FLAG_OF, false); set_pzs8(r);
    return r;
}
uint16_t Cpu::xor16(uint16_t a, uint16_t b) {
    uint16_t r = uint16_t(a ^ b);
    set_flag(FLAG_CF, false); set_flag(FLAG_OF, false); set_pzs16(r);
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
        case 1: return or8(a, b);                      // OR
        case 2: return add8(a, b, flag(FLAG_CF));      // ADC
        case 3: return sub8(a, b, flag(FLAG_CF));      // SBB
        case 4: return and8(a, b);                     // AND
        case 5: return sub8(a, b, false);              // SUB
        case 6: return xor8(a, b);                     // XOR
        default: return sub8(a, b, false);             // CMP (caller discards)
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

int Cpu::alu_cost(int alu, bool dst_is_mem, bool any_mem) {
    // Published 486 costs (Quantasm 486 column, ADD/ADC/SUB/SBB/AND/OR/XOR
    // vs CMP): reg,reg and reg,imm and acc,imm = 1; reg,mem = 2;
    // mem,reg and mem,imm = 3 -- except for CMP, which only *reads* memory
    // and so costs 2 in every memory form. Charging one flat number for
    // the whole group would either overcost every CMP against memory (the
    // single most common instruction in a compare-and-branch loop) or
    // undercost every read-modify-write.
    if (!any_mem) return 1;
    if (alu == 7) return 2;         // CMP: no write-back
    return dst_is_mem ? 3 : 2;
}

// --- data-dependent cost models -------------------------------------------

int Cpu::mul_cost(uint32_t multiplier, int width_bits) {
    // Published 486 MUL/IMUL: r/m8 13-18, r/m16 13-26, r/m32 13-42, with
    // register and memory operands costing the same. The range exists
    // because the 486 uses an early-out multiply whose clock count depends
    // on the position of the most significant bit of the multiplier.
    // Intel publishes the mechanism and both endpoints but not a per-bit
    // formula, so this is a model fitted to exactly those endpoints: the
    // floor is 13, the ceiling is 13 + (width - 3) -- which reproduces
    // 18/26/42 for widths 8/16/32 -- and the cost rises one clock per
    // significant bit past the third. A multiplier of 0-3 costs the floor;
    // one with its top bit set costs the ceiling. Labelled approximation
    // of a published range, not an invented constant.
    const int floor_cost = 13;
    const int ceil_cost = 13 + (width_bits - 3);
    int msb = 0;
    for (int i = width_bits - 1; i >= 0; --i) {
        if (multiplier & (1u << i)) { msb = i + 1; break; }
    }
    int c = floor_cost + (msb > 3 ? msb - 3 : 0);
    return c > ceil_cost ? ceil_cost : c;
}

int Cpu::bitscan_cost(int bits_examined, bool is_mem) {
    // Published 486: BSF 6-42 (register source) / 7-43 (memory source);
    // BSR 6-103 / 7-104. Both scan one bit position at a time, so the cost
    // charged is the published floor plus one clock per bit actually
    // examined -- which stays inside BSF's range for every operand width
    // and at the low end of BSR's (wider) one. Labelled model of a
    // published range; Intel gives the endpoints, not a per-bit figure.
    return (is_mem ? 7 : 6) + bits_examined;
}

int Cpu::rotate_carry_cost(int count, bool is_mem) {
    // Published 486 RCL/RCR by CL or by imm8: 8-30 (register operand),
    // 9-31 (memory operand) -- unlike the plain shifts and rotates, which
    // the 486's barrel shifter makes count-independent, rotate-through-
    // carry is iterative. Linear in the count and anchored to both
    // published endpoints: count 1 costs the floor, and the cost saturates
    // at the ceiling. (The fixed by-1 encodings D0/D1 are a separate,
    // cheaper published case -- 3/4 -- handled in grp2_shift().)
    int c = (is_mem ? 8 : 7) + count;
    int cap = is_mem ? 31 : 30;
    return c > cap ? cap : c;
}

// --- shift/rotate group ---------------------------------------------------
// reg-field selector: 0=ROL 1=ROR 2=RCL 3=RCR 4=SHL/SAL 5=SHR 6=SAL(alias) 7=SAR

uint8_t Cpu::shiftrot8(int op, uint8_t v, int count) {
    count &= 0x1F;
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
            case 7: of = false; break;                                               // SAR
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

void Cpu::shld(const RM &rm, int src_reg, int count, bool right) {
    // SHLD/SHRD r/m, r, count (386+, Intel 80486 PRM): shifts the
    // destination by `count`, filling the vacated bits from the *source
    // register* rather than with zeros or sign. A count of 0 changes
    // nothing and leaves every flag alone; counts are masked mod 32.
    count &= 0x1F;
    if (count == 0) return;
    if (opsize32_) {
        uint32_t dst = rm_read32(rm), src = get_reg32(src_reg);
        uint32_t res;
        bool cf;
        if (!right) {
            cf = (dst >> (32 - count)) & 1;
            res = (count == 32) ? src : ((dst << count) | (src >> (32 - count)));
        } else {
            cf = (dst >> (count - 1)) & 1;
            res = (count == 32) ? src : ((dst >> count) | (src << (32 - count)));
        }
        set_flag(FLAG_CF, cf);
        set_pzs32(res);
        rm_write32(rm, res);
    } else {
        // Intel calls a 16-bit double shift with a count above 15
        // "undefined" (Intel 80486 PRM, SHLD/SHRD), but the 486 has one
        // 32-bit shifter and Borland's runtime `unsigned long` shift
        // helpers -- shipped inside CWSDPMI, called with cl=24 -- are only
        // correct across the full 0-31 range because of what it actually
        // does: shift the 32-bit dest:src concatenation and keep the named
        // half, so above 15 the source register's bits reach the
        // destination. See PC486_REVIEW.md §9.
        //
        // 64-bit because a 32-bit `pair << count` would overflow; narrowed
        // back to 32, the real shifter's width, so bits shifted off the top
        // are lost exactly as hardware loses them.
        uint16_t dst = rm_read16(rm), src = get_reg16(src_reg);
        uint64_t pair = right ? ((uint64_t(src) << 16) | dst) : ((uint64_t(dst) << 16) | src);
        uint16_t res;
        bool cf;
        if (!right) {
            // CF is the last bit shifted out of the 32-bit pair. For a
            // documented count (1-16) that bit is dest's bit 16-count,
            // exactly as the manual specifies.
            cf = ((pair >> (32 - count)) & 1) != 0;
            res = uint16_t(uint32_t(pair << count) >> 16);
        } else {
            cf = ((pair >> (count - 1)) & 1) != 0;
            res = uint16_t(pair >> count);
        }
        set_flag(FLAG_CF, cf);
        set_pzs16(res);
        rm_write16(rm, res);
    }
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
    if (((al & 0x0F) > 9) || flag(FLAG_AF)) {
        al = uint8_t(al + 6);
        ah = uint8_t(ah + 1);
        set_flag(FLAG_AF, true);
        set_flag(FLAG_CF, true);
    } else {
        set_flag(FLAG_AF, false);
        set_flag(FLAG_CF, false);
    }
    set_reg8(0, uint8_t(al & 0x0F));
    set_reg8(4, ah);
}
void Cpu::aas() {
    uint8_t al = get_reg8(0), ah = get_reg8(4);
    if (((al & 0x0F) > 9) || flag(FLAG_AF)) {
        al = uint8_t(al - 6);
        ah = uint8_t(ah - 1);
        set_flag(FLAG_AF, true);
        set_flag(FLAG_CF, true);
    } else {
        set_flag(FLAG_AF, false);
        set_flag(FLAG_CF, false);
    }
    set_reg8(0, uint8_t(al & 0x0F));
    set_reg8(4, ah);
}
void Cpu::aam() {
    uint8_t base = fetch8();
    uint8_t al = get_reg8(0);
    if (base == 0) { eip = instr_start_eip_; do_interrupt(0); return; }  // #DE, divide error
    set_reg8(4, uint8_t(al / base));
    set_reg8(0, uint8_t(al % base));
    set_pzs8(get_reg8(0));
}
void Cpu::aad() {
    uint8_t base = fetch8();
    uint8_t res = uint8_t(get_reg8(0) + get_reg8(4) * base);
    set_reg8(0, res);
    set_reg8(4, 0);
    set_pzs8(res);
}

void Cpu::pusha() {
    // PUSHA/PUSHAD push AX/EAX, CX, DX, BX, the *original* SP/ESP, BP, SI,
    // DI in that order (Intel 80486 PRM).
    if (opsize32_) {
        uint32_t orig = esp;
        push32(eax); push32(ecx); push32(edx); push32(ebx);
        push32(orig);
        push32(ebp); push32(esi); push32(edi);
    } else {
        uint16_t orig = sp();
        push16(uint16_t(eax)); push16(uint16_t(ecx)); push16(uint16_t(edx)); push16(uint16_t(ebx));
        push16(orig);
        push16(uint16_t(ebp)); push16(uint16_t(esi)); push16(uint16_t(edi));
    }
}
void Cpu::popa() {
    if (opsize32_) {
        edi = pop32(); esi = pop32(); ebp = pop32();
        pop32();  // the saved SP slot is discarded -- SP is already correct from the pops themselves
        ebx = pop32(); edx = pop32(); ecx = pop32(); eax = pop32();
    } else {
        set_reg16(7, pop16()); set_reg16(6, pop16()); set_reg16(5, pop16());
        pop16();
        set_reg16(3, pop16()); set_reg16(2, pop16()); set_reg16(1, pop16()); set_reg16(0, pop16());
    }
}
void Cpu::bound() {
    RM rm = decode_modrm();
    if (opsize32_) {
        int32_t idx = int32_t(get_reg32(last_reg_));
        int32_t lo = int32_t(read32(rm.seg, rm.off));
        int32_t hi = int32_t(read32(rm.seg, seg_off(rm.off, 4)));
        if (idx < lo || idx > hi) { eip = instr_start_eip_; do_interrupt(5); }  // #BR
    } else {
        int16_t idx = int16_t(get_reg16(last_reg_));
        int16_t lo = int16_t(read16(rm.seg, rm.off));
        int16_t hi = int16_t(read16(rm.seg, seg_off(rm.off, 2)));
        if (idx < lo || idx > hi) { eip = instr_start_eip_; do_interrupt(5); }
    }
}
void Cpu::imul_imm(int dst_reg, const RM &rm, uint32_t imm) {
    if (opsize32_) {
        int64_t r = int64_t(int32_t(rm_read32(rm))) * int64_t(int32_t(imm));
        uint32_t res = uint32_t(r);
        bool of = (int64_t(int32_t(res)) != r);
        set_flag(FLAG_CF, of); set_flag(FLAG_OF, of);
        set_reg32(dst_reg, res);
    } else {
        int32_t r = int32_t(int16_t(rm_read16(rm))) * int32_t(int16_t(uint16_t(imm)));
        uint16_t res = uint16_t(r);
        bool of = (int32_t(int16_t(res)) != r);
        set_flag(FLAG_CF, of); set_flag(FLAG_OF, of);
        set_reg16(dst_reg, res);
    }
}
int Cpu::enter() {
    uint16_t size = fetch16();
    uint8_t level = uint8_t(fetch8() & 0x1F);
    if (opsize32_) {
        push32(ebp);
        uint32_t frame_ptr = esp;
        for (int i = 1; i < level; ++i) { ebp = ebp - 4; push32(read32(SEG_SS, ebp)); }
        if (level > 0) push32(frame_ptr);
        ebp = frame_ptr;
        add_sp(-int32_t(size));
    } else {
        push16(uint16_t(ebp));
        uint16_t frame_ptr = sp();
        for (int i = 1; i < level; ++i) {
            set_reg16(5, uint16_t(ebp - 2));
            push16(read16(SEG_SS, uint16_t(ebp)));
        }
        if (level > 0) push16(frame_ptr);
        set_reg16(5, frame_ptr);
        add_sp(-int32_t(size));
    }
    return level;
}
void Cpu::leave() {
    if (opsize32_) { esp = ebp; ebp = pop32(); }
    else { set_sp(uint16_t(ebp)); set_reg16(5, pop16()); }
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
void Cpu::jcc_rel8(bool taken) {
    int8_t rel = int8_t(fetch8());
    // A relative branch's target wraps inside the code segment's own width:
    // mod 64KB in a 16-bit segment, mod 4GB in a 32-bit one.
    if (taken) set_ip(uint32_t(eip + uint32_t(int32_t(rel))));
}
int Cpu::loop_group(uint8_t op) {
    int8_t rel = int8_t(fetch8());
    // The 0x67 address-size prefix selects CX vs ECX as the loop counter
    // (LOOPW/LOOPD, JCXZ/JECXZ) -- genuine 386+/486 behavior.
    bool use_ecx = addrsize32_;
    bool take;
    if (op == 0xE3) {  // JCXZ / JECXZ
        take = use_ecx ? (ecx == 0) : (uint16_t(ecx) == 0);
    } else {
        if (use_ecx) ecx = ecx - 1; else set_reg16(1, uint16_t(ecx - 1));
        bool counter_nz = use_ecx ? (ecx != 0) : (uint16_t(ecx) != 0);
        if (op == 0xE0) take = counter_nz && !flag(FLAG_ZF);      // LOOPNE/LOOPNZ
        else if (op == 0xE1) take = counter_nz && flag(FLAG_ZF);  // LOOPE/LOOPZ
        else take = counter_nz;                                  // LOOP
    }
    if (take) set_ip(uint32_t(eip + uint32_t(int32_t(rel))));
    // Published 486 costs (Quantasm 486 column, "no jump/jump"):
    // LOOP 6/7, LOOPE and LOOPNE 6/9, JCXZ 5/8. Charged per-op and
    // per-outcome rather than one flat number, because a decrement-and-
    // branch counting loop is exactly what a CPU-speed benchmark's inner
    // loop is built from -- ibmpc-at's own Landmark Speed Test
    // investigation (IBM_PCAT_REVIEW.md) turned on precisely this opcode.
    if (op == 0xE3) return take ? 8 : 5;
    if (op == 0xE2) return take ? 7 : 6;
    return take ? 9 : 6;
}

// --- string instructions ---------------------------------------------------

int Cpu::string_op(uint8_t op) {
    bool wide = (op & 1) != 0;
    bool is_rep = (rep_ != REP_NONE);
    // The source segment honors an override prefix; the destination is
    // always ES and never can be overridden (Intel 8086 manual onward).
    int src_seg = (seg_override_ >= 0) ? seg_override_ : int(SEG_DS);
    bool is_cmp_scan = (op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF);
    // A 0x66 prefix widens a word string op to a dword one (MOVSD, STOSD,
    // ...); byte forms are never affected. A 0x67 prefix selects
    // ESI/EDI/ECX over SI/DI/CX as the pointers and counter -- genuine
    // 386+/486 behavior, and the same addressing width decode_modrm()
    // honors, so the pointers can legitimately run past 0FFFFh in the
    // "unreal mode" a period DOS memory manager sets up (see cpu80486.h's
    // flat 32-bit accessors and PC486_REVIEW.md §5.4). FreeDOS 1.3's
    // HimemX copies XMS blocks with exactly `F3 67 66 A5` (REP MOVSD,
    // addr32), so advancing only the 16-bit halves here silently moved
    // every block to a wrapped, wrong address.
    bool dword = wide && opsize32_;
    bool addr32 = addrsize32_;
    auto counter = [&]() -> uint32_t { return addr32 ? ecx : uint32_t(uint16_t(ecx)); };
    auto dec_counter = [&]() { if (addr32) ecx = ecx - 1; else set_reg16(1, uint16_t(ecx - 1)); };
    int iterations = 0;  // actually executed -- REPE/REPNE can stop short of the original CX
    do {
        if (is_rep && counter() == 0) break;
        int step_bytes = dword ? 4 : (wide ? 2 : 1);
        int dir = flag(FLAG_DF) ? -step_bytes : step_bytes;
        uint32_t si = addr32 ? esi : uint32_t(uint16_t(esi));
        uint32_t di = addr32 ? edi : uint32_t(uint16_t(edi));
        auto advance_si = [&]() {
            if (addr32) esi = uint32_t(esi + dir); else set_reg16(6, uint16_t(si + dir));
        };
        auto advance_di = [&]() {
            if (addr32) edi = uint32_t(edi + dir); else set_reg16(7, uint16_t(di + dir));
        };
        switch (op) {
            case 0xA4: case 0xA5:  // MOVS
                if (!wide) write8(SEG_ES, di, read8(src_seg, si));
                else if (dword) write32(SEG_ES, di, read32(src_seg, si));
                else write16(SEG_ES, di, read16(src_seg, si));
                advance_si(); advance_di();
                break;
            case 0xA6: case 0xA7:  // CMPS
                if (!wide) sub8(read8(src_seg, si), read8(SEG_ES, di), false);
                else if (dword) sub32(read32(src_seg, si), read32(SEG_ES, di), false);
                else sub16(read16(src_seg, si), read16(SEG_ES, di), false);
                advance_si(); advance_di();
                break;
            case 0xAA: case 0xAB:  // STOS
                if (!wide) write8(SEG_ES, di, get_reg8(0));
                else if (dword) write32(SEG_ES, di, eax);
                else write16(SEG_ES, di, uint16_t(eax));
                advance_di();
                break;
            case 0xAC: case 0xAD:  // LODS
                if (!wide) set_reg8(0, read8(src_seg, si));
                else if (dword) eax = read32(src_seg, si);
                else set_reg16(0, read16(src_seg, si));
                advance_si();
                break;
            default:               // SCAS (0xAE/0xAF)
                if (!wide) sub8(get_reg8(0), read8(SEG_ES, di), false);
                else if (dword) sub32(eax, read32(SEG_ES, di), false);
                else sub16(uint16_t(eax), read16(SEG_ES, di), false);
                advance_di();
                break;
        }
        ++iterations;
        if (!is_rep) break;
        dec_counter();
        if (is_cmp_scan) {
            bool z = flag(FLAG_ZF);
            if (rep_ == REP_Z && !z) break;   // REPE/REPZ: stop once not-equal
            if (rep_ == REP_NZ && z) break;   // REPNE/REPNZ: stop once equal
        }
    } while (is_rep && counter() != 0);
    // Published 486 string costs (Quantasm 486 column). The REP forms are
    // formulas in n, the number of iterations that actually ran (which for
    // REPE/REPNE can be fewer than the original CX), plus two documented
    // special cases carried verbatim from the table's footnote: "5 if n=0,
    // 13 if n=1" for REP MOVS and REP STOS, and "5 if n=0" for the
    // compare/scan forms. Charging a flat per-instruction number instead
    // would bill a REP MOVSD copying a 512-byte sector the same as copying
    // one byte -- the bulk-copy undercount ibmpc-at hit.
    const int n = iterations;
    switch (op) {
        case 0xA4: case 0xA5:  // MOVS 7; REP MOVS 12+3n
            if (!is_rep) return 7;
            return n == 0 ? 5 : (n == 1 ? 13 : 12 + 3 * n);
        case 0xA6: case 0xA7:  // CMPS 8; REP(N)E CMPS 7+7n
            if (!is_rep) return 8;
            return n == 0 ? 5 : 7 + 7 * n;
        case 0xAA: case 0xAB:  // STOS 5; REP STOS 7+4n
            if (!is_rep) return 5;
            return n == 0 ? 5 : (n == 1 ? 13 : 7 + 4 * n);
        case 0xAC: case 0xAD:
            // LODS 5. The 486 table publishes no REP LODS formula, because
            // real software never REPs it -- every iteration but the last
            // just clobbers AL/EAX -- so the single-iteration cost stands
            // even for the degenerate REP-prefixed form.
            return 5;
        default:               // SCAS 6; REP(N)E SCAS 7+5n
            if (!is_rep) return 6;
            return n == 0 ? 5 : 7 + 5 * n;
    }
}
int Cpu::io_string_op(uint8_t op) {
    bool wide = (op & 1) != 0;
    bool is_rep = (rep_ != REP_NONE);  // only a plain REP is meaningful here on real hardware
    int src_seg = (seg_override_ >= 0) ? seg_override_ : int(SEG_DS);
    int iterations = 0;
    do {
        if (is_rep && uint16_t(ecx) == 0) break;
        int step_bytes = wide ? 2 : 1;
        int dir = flag(FLAG_DF) ? -step_bytes : step_bytes;
        if (op == 0x6C || op == 0x6D) {  // INS: port DX -> ES:DI
            uint16_t di = uint16_t(edi);
            if (!wide) write8(SEG_ES, di, bus_in(uint16_t(edx)));
            else write16(SEG_ES, di, bus_in16(uint16_t(edx)));
            set_reg16(7, uint16_t(di + dir));
        } else {                          // OUTS: DS:SI -> port DX
            uint16_t si = uint16_t(esi);
            if (!wide) bus_out(uint16_t(edx), read8(src_seg, si));
            else bus_out16(uint16_t(edx), read16(src_seg, si));
            set_reg16(6, uint16_t(si + dir));
        }
        ++iterations;
        if (!is_rep) break;
        set_reg16(1, uint16_t(ecx - 1));
    } while (is_rep && uint16_t(ecx) != 0);
    // Published 486: INS and OUTS both cost 17 in real mode. The table
    // publishes no separate REP INS/REP OUTS formula for the 486 column,
    // so this core charges the published single-iteration cost once per
    // iteration actually run, and the same documented "5 if n=0" empty-rep
    // cost the other string forms use. Labelled inference from the
    // published per-iteration figure, not a cited REP formula.
    if (!is_rep) return 17;
    return iterations == 0 ? 5 : 17 * iterations;
}

// --- opcode groups (reg field of ModR/M selects the operation) -----------

int Cpu::grp1_immed(uint8_t op) {  // 0x80/0x82: r/m8,imm8  0x81: r/m16/32,imm16/32  0x83: r/m16/32,imm8(sx)
    RM rm = decode_modrm();
    int alu = last_reg_;
    bool wide = (op == 0x81 || op == 0x83);
    if (!wide) {
        uint8_t imm = fetch8();
        uint8_t res = alu_apply8(alu, rm_read8(rm), imm);
        if (alu != 7) rm_write8(rm, res);
    } else if (opsize32_) {
        uint32_t imm = (op == 0x83) ? uint32_t(int32_t(int8_t(fetch8()))) : fetch32();
        uint32_t res = alu_apply32(alu, rm_read32(rm), imm);
        if (alu != 7) rm_write32(rm, res);
    } else {
        uint16_t imm = (op == 0x83) ? uint16_t(int16_t(int8_t(fetch8()))) : fetch16();
        uint16_t res = alu_apply16(alu, rm_read16(rm), imm);
        if (alu != 7) rm_write16(rm, res);
    }
    return alu_cost(alu, rm.is_mem, rm.is_mem);
}
int Cpu::grp2_shift(uint8_t op) {  // 0xC0/0xD0/0xD2: 8-bit  0xC1/0xD1/0xD3: 16/32-bit
    RM rm = decode_modrm();
    int alu = last_reg_;
    bool wide = (op == 0xC1 || op == 0xD1 || op == 0xD3);
    bool by_one = (op == 0xD0 || op == 0xD1);
    bool by_imm = (op == 0xC0 || op == 0xC1);
    int count;
    if (by_imm) count = fetch8();
    else if (by_one) count = 1;
    else count = get_reg8(1);  // CL
    if (!wide) {
        uint8_t res = shiftrot8(alu, rm_read8(rm), count);
        if ((count & 0x1F) != 0) rm_write8(rm, res);
    } else if (opsize32_) {
        uint32_t res = shiftrot32(alu, rm_read32(rm), count);
        if ((count & 0x1F) != 0) rm_write32(rm, res);
    } else {
        uint16_t res = shiftrot16(alu, rm_read16(rm), count);
        if ((count & 0x1F) != 0) rm_write16(rm, res);
    }
    // Published 486 costs (Quantasm 486 column). The 486's barrel shifter
    // makes SHL/SHR/SAR/ROL/ROR count-INDEPENDENT -- a real departure from
    // the 286, whose cost was 5+n/8+n. Only RCL/RCR remain iterative (see
    // rotate_carry_cost). The by-1 encodings (D0/D1) and the by-imm8 ones
    // (C0/C1) are separately published cases, not the same number: a
    // register shift by an immediate is 2, by 1 or by CL it is 3.
    bool rc = (alu == 2 || alu == 3);  // RCL/RCR
    if (by_one) return rm.is_mem ? 4 : 3;
    if (rc) return rotate_carry_cost(count & 0x1F, rm.is_mem);
    if (by_imm) return rm.is_mem ? 4 : 2;
    return rm.is_mem ? 4 : 3;  // by CL
}
int Cpu::grp3_unary(uint8_t op) {  // 0xF6: r/m8  0xF7: r/m16/32 -- TEST/NOT/NEG/MUL/IMUL/DIV/IDIV
    RM rm = decode_modrm();
    int alu = last_reg_;
    bool wide = (op == 0xF7);
    int width = !wide ? 8 : (opsize32_ ? 32 : 16);
    uint32_t multiplier = 0;   // captured for mul_cost()
    bool faulted = false;
    if (!wide) {
        uint8_t v = rm_read8(rm);
        multiplier = v;
        switch (alu) {
            case 0: case 1: { uint8_t imm = fetch8(); and8(v, imm); break; }  // TEST
            case 2: rm_write8(rm, uint8_t(~v)); break;                        // NOT
            case 3: { bool nz = v != 0; rm_write8(rm, sub8(0, v, false)); set_flag(FLAG_CF, nz); break; }  // NEG
            case 4: {  // MUL
                unsigned r = unsigned(get_reg8(0)) * unsigned(v);
                set_reg16(0, uint16_t(r));
                bool ov = (r >> 8) != 0;
                set_flag(FLAG_CF, ov); set_flag(FLAG_OF, ov);
                break;
            }
            case 5: {  // IMUL
                int r = int(int8_t(get_reg8(0))) * int(int8_t(v));
                set_reg16(0, uint16_t(r));
                bool ov = (int(int8_t(uint8_t(r))) != r);
                set_flag(FLAG_CF, ov); set_flag(FLAG_OF, ov);
                break;
            }
            case 6: {  // DIV
                if (v == 0) { eip = instr_start_eip_; do_interrupt(0); faulted = true; break; }
                uint16_t ax = get_reg16(0);
                unsigned q = ax / v, rem = ax % v;
                if (q > 0xFF) { eip = instr_start_eip_; do_interrupt(0); faulted = true; break; }
                set_reg16(0, uint16_t((rem << 8) | q));
                break;
            }
            default: {  // IDIV
                int16_t dividend = int16_t(get_reg16(0));
                int8_t divisor = int8_t(v);
                if (divisor == 0) { eip = instr_start_eip_; do_interrupt(0); faulted = true; break; }
                int q = dividend / divisor, rem = dividend % divisor;
                if (q > 127 || q < -128) { eip = instr_start_eip_; do_interrupt(0); faulted = true; break; }
                set_reg16(0, uint16_t((uint16_t(uint8_t(int8_t(rem))) << 8) | uint8_t(int8_t(q))));
                break;
            }
        }
    } else if (opsize32_) {
        uint32_t v = rm_read32(rm);
        multiplier = v;
        switch (alu) {
            case 0: case 1: { uint32_t imm = fetch32(); and32(v, imm); break; }
            case 2: rm_write32(rm, ~v); break;
            case 3: { bool nz = v != 0; rm_write32(rm, sub32(0, v, false)); set_flag(FLAG_CF, nz); break; }
            case 4: {
                uint64_t r = uint64_t(eax) * v;
                eax = uint32_t(r); edx = uint32_t(r >> 32);
                bool ov = edx != 0;
                set_flag(FLAG_CF, ov); set_flag(FLAG_OF, ov);
                break;
            }
            case 5: {
                int64_t r = int64_t(int32_t(eax)) * int64_t(int32_t(v));
                eax = uint32_t(r); edx = uint32_t(uint64_t(r) >> 32);
                bool ov = (int64_t(int32_t(uint32_t(r))) != r);
                set_flag(FLAG_CF, ov); set_flag(FLAG_OF, ov);
                break;
            }
            case 6: {
                if (v == 0) { eip = instr_start_eip_; do_interrupt(0); faulted = true; break; }
                uint64_t dividend = (uint64_t(edx) << 32) | eax;
                uint64_t q = dividend / v, rem = dividend % v;
                if (q > 0xFFFFFFFFull) { eip = instr_start_eip_; do_interrupt(0); faulted = true; break; }
                eax = uint32_t(q); edx = uint32_t(rem);
                break;
            }
            default: {
                int32_t divisor = int32_t(v);
                if (divisor == 0) { eip = instr_start_eip_; do_interrupt(0); faulted = true; break; }
                int64_t dividend = int64_t((uint64_t(edx) << 32) | eax);
                int64_t q = dividend / divisor, rem = dividend % divisor;
                if (q > 2147483647ll || q < -2147483648ll) { eip = instr_start_eip_; do_interrupt(0); faulted = true; break; }
                eax = uint32_t(int32_t(q)); edx = uint32_t(int32_t(rem));
                break;
            }
        }
    } else {
        uint16_t v = rm_read16(rm);
        multiplier = v;
        switch (alu) {
            case 0: case 1: { uint16_t imm = fetch16(); and16(v, imm); break; }
            case 2: rm_write16(rm, uint16_t(~v)); break;
            case 3: { bool nz = v != 0; rm_write16(rm, sub16(0, v, false)); set_flag(FLAG_CF, nz); break; }
            case 4: {
                uint32_t r = uint32_t(get_reg16(0)) * v;
                set_reg16(0, uint16_t(r)); set_reg16(2, uint16_t(r >> 16));
                bool ov = get_reg16(2) != 0;
                set_flag(FLAG_CF, ov); set_flag(FLAG_OF, ov);
                break;
            }
            case 5: {
                int32_t r = int32_t(int16_t(get_reg16(0))) * int32_t(int16_t(v));
                set_reg16(0, uint16_t(r)); set_reg16(2, uint16_t(uint32_t(r) >> 16));
                bool ov = (int32_t(int16_t(uint16_t(r))) != r);
                set_flag(FLAG_CF, ov); set_flag(FLAG_OF, ov);
                break;
            }
            case 6: {
                if (v == 0) { eip = instr_start_eip_; do_interrupt(0); faulted = true; break; }
                uint32_t dividend = (uint32_t(get_reg16(2)) << 16) | get_reg16(0);
                uint32_t q = dividend / v, rem = dividend % v;
                if (q > 0xFFFF) { eip = instr_start_eip_; do_interrupt(0); faulted = true; break; }
                set_reg16(0, uint16_t(q)); set_reg16(2, uint16_t(rem));
                break;
            }
            default: {
                int16_t divisor = int16_t(v);
                if (divisor == 0) { eip = instr_start_eip_; do_interrupt(0); faulted = true; break; }
                int32_t dividend = int32_t((uint32_t(get_reg16(2)) << 16) | get_reg16(0));
                int32_t q = dividend / divisor, rem = dividend % divisor;
                if (q > 32767 || q < -32768) { eip = instr_start_eip_; do_interrupt(0); faulted = true; break; }
                set_reg16(0, uint16_t(int16_t(q))); set_reg16(2, uint16_t(int16_t(rem)));
                break;
            }
        }
    }
    (void)faulted;
    // Published 486 costs (Quantasm 486 column). TEST is read-only (1 reg /
    // 2 mem) and NOT/NEG read-modify-write (1 / 3), while MUL and the
    // divides are an order of magnitude more expensive -- DIV r/m32 is 40
    // clocks, IDIV r/m32 43/44. A single flat group cost would undercost
    // exactly the DIV/MUL-heavy loop a period CPU-speed benchmark uses,
    // which is the concrete regression ibmpc-at's timing work chased down
    // (IBM_PCAT_REVIEW.md). Note the 486 charges the *same* cost for a
    // memory and a register MUL operand, and only +1 for IDIV's.
    switch (alu) {
        case 0: case 1: return rm.is_mem ? 2 : 1;                                 // TEST
        case 2: case 3: return rm.is_mem ? 3 : 1;                                 // NOT, NEG
        case 4: case 5: return mul_cost(multiplier, width);                        // MUL, IMUL
        case 6:         return width == 8 ? 16 : (width == 16 ? 24 : 40);          // DIV (mem and reg identical)
        default:        return width == 8 ? (rm.is_mem ? 20 : 19)                  // IDIV
                             : (width == 16 ? (rm.is_mem ? 28 : 27)
                                            : (rm.is_mem ? 44 : 43));
    }
}
int Cpu::grp5(uint8_t op) {  // 0xFE: INC/DEC r/m8   0xFF: INC/DEC/CALL/JMP/PUSH r/m16/32
    RM rm = decode_modrm();
    int alu = last_reg_;
    if (op == 0xFE) {
        bool cf = flag(FLAG_CF);
        if (alu == 0) { uint8_t r = add8(rm_read8(rm), 1, false); set_flag(FLAG_CF, cf); rm_write8(rm, r); }
        else if (alu == 1) { uint8_t r = sub8(rm_read8(rm), 1, false); set_flag(FLAG_CF, cf); rm_write8(rm, r); }
        return rm.is_mem ? 3 : 1;
    }
    switch (alu) {
        case 0: {  // INC -- never touches CF
            bool cf = flag(FLAG_CF);
            if (opsize32_) { uint32_t r = add32(rm_read32(rm), 1, false); set_flag(FLAG_CF, cf); rm_write32(rm, r); }
            else { uint16_t r = add16(rm_read16(rm), 1, false); set_flag(FLAG_CF, cf); rm_write16(rm, r); }
            return rm.is_mem ? 3 : 1;
        }
        case 1: {  // DEC
            bool cf = flag(FLAG_CF);
            if (opsize32_) { uint32_t r = sub32(rm_read32(rm), 1, false); set_flag(FLAG_CF, cf); rm_write32(rm, r); }
            else { uint16_t r = sub16(rm_read16(rm), 1, false); set_flag(FLAG_CF, cf); rm_write16(rm, r); }
            return rm.is_mem ? 3 : 1;
        }
        case 2: {  // CALL near indirect
            if (opsize32_) { uint32_t t = rm_read32(rm); push32(eip); set_ip(t); }
            else { uint16_t t = rm_read16(rm); push16(uint16_t(eip)); set_ip(t); }
            return 5;
        }
        case 3: {  // CALL far indirect (memory only)
            uint32_t off;
            uint16_t seg;
            if (opsize32_) { off = read32(rm.seg, rm.off); seg = read16(rm.seg, seg_off(rm.off, 4)); }
            else { off = read16(rm.seg, rm.off); seg = read16(rm.seg, seg_off(rm.off, 2)); }
            int c = far_transfer(seg, off, true);
            // Published 486: CALL far *indirect* costs 17 in real mode,
            // where the direct form costs 18; the protected-mode row is 20
            // for both, so only the real-mode figure needs correcting.
            if (!protected_mode()) c = 17;
            return c;
        }
        case 4:    // JMP near indirect
            set_ip(opsize32_ ? rm_read32(rm) : uint32_t(rm_read16(rm)));
            return 5;
        case 5: {  // JMP far indirect (memory only)
            uint32_t off;
            uint16_t seg;
            if (opsize32_) { off = read32(rm.seg, rm.off); seg = read16(rm.seg, seg_off(rm.off, 4)); }
            else { off = read16(rm.seg, rm.off); seg = read16(rm.seg, seg_off(rm.off, 2)); }
            int c = far_transfer(seg, off, false);
            // Published 486: JMP far indirect is 13 in real mode against
            // the direct form's 17; protected mode publishes 18 and 19.
            if (!protected_mode()) c = 13;
            else if (c == 19) c = 18;
            return c;
        }
        case 6:    // PUSH r/m
            if (opsize32_) push32(rm_read32(rm)); else push16(rm_read16(rm));
            return rm.is_mem ? 4 : 1;
        default:
            if (on_unimplemented) on_unimplemented(cs, instr_start_eip_, uint16_t(op));
            return 1;
    }
}

// --- descriptor-table and task-management instructions --------------------

bool Cpu::probe_desc(uint16_t selector, RawDesc &out) {
    if ((selector & 0xFFFCu) == 0) return false;
    bool use_ldt = (selector & 0x0004u) != 0;
    if (use_ldt && (ldt_sel_ & 0xFFFCu) == 0) return false;
    uint32_t base  = use_ldt ? ldtr_.base : gdtr_.base;
    uint32_t limit = use_ldt ? ldtr_.limit : gdtr_.limit;
    uint32_t index = selector & 0xFFF8u;
    if (index + 7u > limit) return false;
    out.lo = lin_read32(base + index, false);
    out.hi = lin_read32(base + index + 4, false);
    return true;
}

int Cpu::grp0f00() {  // SLDT/STR/LLDT/LTR/VERR/VERW r/m16
    RM rm = decode_modrm();
    int sub = last_reg_;
    // Every member of this group needs descriptor tables to mean anything,
    // and Intel documents all six as "not recognized in Real Address Mode"
    // -- they are invalid-opcode faults there, not no-ops. (Milestone 1
    // made them documented no-ops because this core had no protected mode
    // at all; now that it does, the real behavior is available.)
    if (!protected_mode()) raise(EXC_UD);
    switch (sub) {
        case 0: rm_write16(rm, ldt_sel_); return rm.is_mem ? 3 : 2;   // SLDT
        case 1: rm_write16(rm, tr_sel_); return rm.is_mem ? 3 : 2;    // STR
        case 2: {  // LLDT r/m16
            if (cpl() != 0) raise_err(EXC_GP, 0);
            uint16_t sel = rm_read16(rm);
            if ((sel & 0xFFFCu) == 0) {
                // A null LDT selector is legal: it marks the task as having
                // no LDT, and only a later LDT-selector reference faults.
                ldt_sel_ = sel;
                ldtr_.base = 0;
                ldtr_.limit = 0;
                return 11;
            }
            if (sel & 0x0004u) raise_sel(EXC_GP, sel);  // an LDT's own descriptor lives in the GDT
            RawDesc d = read_desc(sel, EXC_GP);
            uint8_t a = uint8_t((d.hi >> 8) & 0xFF);
            if (!acc_system(a) || sys_type(a) != SYS_LDT) raise_sel(EXC_GP, sel);
            if (!acc_present(a)) raise_sel(EXC_NP, sel);
            ldt_sel_ = sel;
            ldtr_.base = desc_base(d);
            uint32_t lim = desc_limit(d);
            ldtr_.limit = uint16_t(lim > 0xFFFFu ? 0xFFFFu : lim);
            return 11;
        }
        case 3: {  // LTR r/m16
            if (cpl() != 0) raise_err(EXC_GP, 0);
            uint16_t sel = rm_read16(rm);
            if ((sel & 0xFFFCu) == 0 || (sel & 0x0004u)) raise_sel(EXC_GP, sel);
            RawDesc d = read_desc(sel, EXC_GP);
            uint8_t a = uint8_t((d.hi >> 8) & 0xFF);
            int t = sys_type(a);
            if (!acc_system(a) || (t != SYS_TSS16_AVAIL && t != SYS_TSS32_AVAIL))
                raise_sel(EXC_GP, sel);
            if (!acc_present(a)) raise_sel(EXC_NP, sel);
            // LTR marks the task busy in its descriptor. From here on the
            // CPU considers itself executing that task, which is exactly
            // what lets a later task switch save state into it.
            uint32_t addr = gdtr_.base + (sel & 0xFFF8u) + 4;
            uint32_t hi = lin_read32(addr, true);
            lin_write32(addr, hi | 0x00000200u);
            tr_sel_ = sel;
            tr_.base = desc_base(d);
            uint32_t lim = desc_limit(d);
            tr_.limit = uint16_t(lim > 0xFFFFu ? 0xFFFFu : lim);
            tr_access_ = uint8_t((a & 0xF0u) | uint8_t(t | 0x2));
            return 20;
        }
        default: {  // VERR (/4) and VERW (/5)
            // These report through ZF and never fault on a bad selector --
            // that is their entire purpose: "can I safely load this?"
            uint16_t sel = rm_read16(rm);
            bool want_write = (sub == 5);
            RawDesc d;
            bool ok = probe_desc(sel, d);
            if (ok) {
                uint8_t a = uint8_t((d.hi >> 8) & 0xFF);
                int rpl = sel & 3;
                int eff = (cpl() > rpl) ? cpl() : rpl;
                if (acc_system(a)) ok = false;
                else if (want_write) ok = acc_writable(a);
                else ok = acc_readable(a);
                // A conforming code segment is readable from any level; for
                // everything else the effective privilege must reach it.
                if (ok && !acc_conforming(a) && eff > acc_dpl(a)) ok = false;
                if (ok && !acc_present(a)) ok = false;
            }
            set_flag(FLAG_ZF, ok);
            return 11;
        }
    }
}

int Cpu::grp0f01() {  // SGDT/SIDT/LGDT/LIDT/SMSW/LMSW/INVLPG
    RM rm = decode_modrm();
    switch (last_reg_) {
        case 0: case 1: {  // SGDT / SIDT m16&32 -- legal in real mode
            if (!rm.is_mem) raise(EXC_UD);
            const DescTableReg &t = (last_reg_ == 0) ? gdtr_ : idtr_;
            write16(rm.seg, rm.off, t.limit);
            write32(rm.seg, seg_off(rm.off, 2), t.base);
            return 10;
        }
        case 2: case 3: {  // LGDT / LIDT m16&32 -- legal in real mode
            if (!rm.is_mem) raise(EXC_UD);
            if (protected_mode() && cpl() != 0) raise_err(EXC_GP, 0);
            uint16_t limit = read16(rm.seg, rm.off);
            uint32_t base = read32(rm.seg, seg_off(rm.off, 2));
            // With a 16-bit operand size only 24 bits of base are loaded --
            // the 286-compatible form of the 6-byte pseudo-descriptor
            // (Intel 80486 PRM, LGDT/LIDT). A 486 in real mode defaults to
            // 16-bit operands, so the classic real-mode `LGDT [x]` a DOS
            // extender emits loads a 24-bit base, which is all it needs for
            // a GDT below 16MB.
            if (!opsize32_) base &= 0x00FFFFFFu;
            DescTableReg &t = (last_reg_ == 2) ? gdtr_ : idtr_;
            t.limit = limit;
            t.base = base;
            return 11;
        }
        case 4:  // SMSW r/m16 -- the low 16 bits of CR0, legal in real mode
            rm_write16(rm, uint16_t(cr_[0] & 0xFFFFu));
            return rm.is_mem ? 3 : 2;
        case 6: {  // LMSW r/m16
            if (protected_mode() && cpl() != 0) raise_err(EXC_GP, 0);
            uint16_t v = rm_read16(rm);
            // LMSW writes only CR0's low four bits (PE MP EM TS) and,
            // famously, cannot *clear* PE -- leaving protected mode needs a
            // MOV to CR0, which is why 286 code could enter protected mode
            // but never leave it (Intel 80486 PRM, LMSW).
            uint32_t nv = (cr_[0] & 0xFFFFFFF0u) | uint32_t(v & 0x0Fu);
            if (cr_[0] & CR0_PE) nv |= CR0_PE;
            write_cr0(nv);
            return 13;
        }
        case 7:  // INVLPG m
            if (!rm.is_mem) raise(EXC_UD);
            if (protected_mode() && cpl() != 0) raise_err(EXC_GP, 0);
            // The operand names a *linear* address whose TLB entry is
            // dropped. Composed straight from the segment base rather than
            // through seg_linear(), because invalidating an entry outside
            // the segment's limit is harmless and INVLPG is not documented
            // to raise a limit fault.
            tlb_invalidate(seg_base(rm.seg) + rm.off);
            return 12;
        default:
            raise(EXC_UD);   // /5 is undefined on a 486
    }
}

void Cpu::write_cr0(uint32_t v) {
    // ET is hardwired to 1 on an Intel486 -- the FPU is on-die, so there is
    // nothing for software to tell the CPU about it.
    v |= CR0_ET;
    // "Paging can be enabled only when protection is enabled" -- setting PG
    // with PE clear is a #GP(0), not a silently dropped bit (Intel 80486
    // PRM, CR0 description).
    if ((v & CR0_PG) && !(v & CR0_PE)) raise_err(EXC_GP, 0);
    uint32_t old = cr_[0];
    cr_[0] = v;
    if ((old ^ v) & (CR0_PG | CR0_WP)) tlb_flush();
    // Switching either direction deliberately reloads *nothing*: the
    // descriptor caches keep exactly what they held. That is what makes the
    // standard "set PE, then far-JMP to load a real CS" entry sequence work
    // -- the instructions between the two run out of the old cached CS --
    // and equally what makes the "unreal mode" exit trick work, where the
    // big limit loaded in protected mode survives back into real mode
    // (PC486_REVIEW.md §5.4).
}

int Cpu::mov_control_reg(uint8_t op2) {
    RM rm = decode_modrm();
    int idx = last_reg_ & 7;
    // These forms have no memory encoding: the other operand is always a
    // general register, so a mod != 11 ModR/M is an invalid opcode.
    if (rm.is_mem) raise(EXC_UD);
    int gpr = rm.reg;
    // Only ring 0 may touch the control or debug registers.
    if (protected_mode() && cpl() != 0) raise_err(EXC_GP, 0);
    switch (op2) {
        case 0x20:  // MOV r32, CRn
            if (idx == 1 || idx > 3) raise(EXC_UD);   // there is no CR1
            set_reg32(gpr, cr_[idx]);
            return 4;
        case 0x22: {  // MOV CRn, r32
            if (idx == 1 || idx > 3) raise(EXC_UD);
            uint32_t v = get_reg32(gpr);
            if (idx == 0) { write_cr0(v); return 16; }
            if (idx == 3) {
                // Writing CR3 is the architectural "flush the whole TLB"
                // operation -- how an OS switches address spaces.
                cr_[3] = v;
                tlb_flush();
                return 4;
            }
            cr_[idx] = v;
            return 4;
        }
        case 0x21: set_reg32(gpr, dr_[idx]); return 10;   // MOV r32, DRn
        case 0x23: dr_[idx] = get_reg32(gpr); return 11;  // MOV DRn, r32
        case 0x24: set_reg32(gpr, 0); return 4;           // MOV r32, TRn -- no cache/TLB test interface
        default: return 6;                                // MOV TRn, r32
    }
}

// --- the 0x0F escape space -------------------------------------------------

int Cpu::two_byte() {
    uint8_t op2 = fetch8();
    switch (op2) {
        case 0x00: return grp0f00();   // SLDT/STR/LLDT/LTR/VERR/VERW
        case 0x01: return grp0f01();   // SGDT/SIDT/LGDT/LIDT/SMSW/LMSW/INVLPG
        case 0x02: case 0x03: {
            // LAR r16/32, r/m16 and LSL (LSDT's "load segment limit"),
            // both protected-mode only and both reporting success in ZF
            // rather than faulting on a bad selector.
            RM rm = decode_modrm();
            int dst = last_reg_;
            if (!protected_mode()) raise(EXC_UD);
            uint16_t sel = rm_read16(rm);
            RawDesc d;
            bool ok = probe_desc(sel, d);
            if (ok) {
                uint8_t a = uint8_t((d.hi >> 8) & 0xFF);
                int rpl = sel & 3;
                int eff = (cpl() > rpl) ? cpl() : rpl;
                // A conforming code segment is visible from anywhere; for
                // everything else the effective privilege must reach the
                // descriptor's DPL.
                if (!acc_conforming(a) && eff > acc_dpl(a)) ok = false;
                // LSL refuses system descriptors that have no meaningful
                // limit to report -- a gate is not a segment.
                if (ok && op2 == 0x03 && acc_system(a)) {
                    int t = sys_type(a);
                    if (t != SYS_TSS16_AVAIL && t != SYS_TSS16_BUSY && t != SYS_LDT &&
                        t != SYS_TSS32_AVAIL && t != SYS_TSS32_BUSY) ok = false;
                }
            }
            if (ok) {
                if (op2 == 0x02) {
                    // LAR returns the descriptor's *access rights*: the
                    // whole high dword with the base and limit fields
                    // masked out (Intel 80486 PRM, LAR).
                    uint32_t rights = d.hi & 0x00F0FF00u;
                    if (opsize32_) set_reg32(dst, rights);
                    else set_reg16(dst, uint16_t(rights & 0xFFFFu));
                } else {
                    uint32_t lim = desc_limit(d);
                    if (opsize32_) set_reg32(dst, lim);
                    else set_reg16(dst, uint16_t(lim & 0xFFFFu));
                }
            }
            set_flag(FLAG_ZF, ok);
            return (op2 == 0x02) ? 11 : 10;
        }
        case 0x06:
            // CLTS clears CR0.TS, which is how an FPU-state-swapping #NM
            // handler says "the coprocessor now belongs to this task".
            if (protected_mode() && cpl() != 0) raise_err(EXC_GP, 0);
            cr_[0] &= ~uint32_t(CR0_TS);
            return 7;
        case 0x08:
            // INVD / WBINVD: this core has no cache model at all, so
            // "invalidate it" is correctly a no-op rather than an
            // approximation -- but both are still CPL-0-only instructions.
            if (protected_mode() && cpl() != 0) raise_err(EXC_GP, 0);
            return 4;
        case 0x09:
            if (protected_mode() && cpl() != 0) raise_err(EXC_GP, 0);
            return 5;
        case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x26:
            return mov_control_reg(op2);
        // PUSH/POP FS and GS (386 additions; genuine 486 instructions,
        // legal in real mode).
        case 0xA0: if (opsize32_) push32(fs); else push16(fs); return 3;
        case 0xA1: load_seg(SEG_FS, opsize32_ ? uint16_t(pop32()) : pop16()); return 3;
        case 0xA8: if (opsize32_) push32(gs); else push16(gs); return 3;
        case 0xA9: load_seg(SEG_GS, opsize32_ ? uint16_t(pop32()) : pop16()); return 3;

        case 0xA2:
            // CPUID. The part this machine carries is an SL-Enhanced
            // IntelDX2-66, one of the Intel486 revisions that has this
            // instruction: AP-485's processor list gives CPUID to IntelDX4
            // and to the SL-Enhanced IntelDX2 / Intel486 SX / Intel486 DX,
            // while "older versions of Intel486 SX, Intel486 DX and
            // IntelDX2 processors do not support" it and fault instead.
            //
            // Function 0 returns the maximum input value and "GenuineIntel"
            // in EBX:EDX:ECX; 1 is the maximum because function 2's cache
            // descriptors are Pentium-era, not 486. Function 1's signature
            // is type 0 / family 4 / model 3 -- AP-485's model table gives
            // model 3 to the IntelDX2 -- and the only feature bit an
            // Intel486 asserts is bit 0, the on-die FPU (every other EDX
            // bit names a Pentium-or-later feature: VME, TSC, MSR,
            // CMPXCHG8B, APIC...).
            // An input above the maximum returns function 1's data, the
            // convention AP-485 documents; the 486 has nothing to say about
            // it, and nothing in this machine's software asks.
            if (eax == 0) {
                eax = 1;
                ebx = 0x756E6547u;  // "Genu"
                edx = 0x49656E69u;  // "ineI"
                ecx = 0x6C65746Eu;  // "ntel"
            } else {
                eax = 0x00000433u;  // family 4, model 3 (IntelDX2), stepping 3
                ebx = 0;
                ecx = 0;
                edx = 0x00000001u;  // FPU on die
            }
            // The Quantasm table this file's cycle counts come from lists no
            // 486 figure for CPUID (it labels the instruction Pentium+); 14
            // is the published Pentium timing, used here for want of a 486 one.
            return 14;

        // SHLD/SHRD r/m, r, imm8 | CL
        case 0xA4: case 0xAC: {
            RM rm = decode_modrm();
            int src = last_reg_;
            int count = fetch8();
            shld(rm, src, count, op2 == 0xAC);
            return rm.is_mem ? 3 : 2;
        }
        case 0xA5: case 0xAD: {
            RM rm = decode_modrm();
            int src = last_reg_;
            shld(rm, src, get_reg8(1), op2 == 0xAD);
            return rm.is_mem ? 4 : 3;
        }

        // Bit test group. BT only reads; BTS/BTR/BTC read-modify-write.
        case 0xA3: case 0xAB: case 0xB3: case 0xBB: {  // BT/BTS/BTR/BTC r/m, r
            RM rm = decode_modrm();
            int32_t bit = opsize32_ ? int32_t(get_reg32(last_reg_)) : int32_t(int16_t(get_reg16(last_reg_)));
            int opsz = opsize32_ ? 32 : 16;
            RM target = rm;
            if (rm.is_mem) {
                // For a memory operand the bit offset is not masked: it
                // selects a bit in a string of operand-size units starting
                // at the effective address (Intel 80486 PRM, BT/BTS/BTR/BTC).
                int32_t unit = bit / opsz - ((bit % opsz < 0) ? 1 : 0);
                target.off = uint16_t(rm.off + unit * (opsz / 8));
                bit = bit - unit * opsz;
            } else {
                bit &= (opsz - 1);
            }
            uint32_t v = opsize32_ ? rm_read32(target) : rm_read16(target);
            bool set = ((v >> bit) & 1) != 0;
            set_flag(FLAG_CF, set);
            if (op2 != 0xA3) {
                uint32_t nv = (op2 == 0xAB) ? (v | (1u << bit))
                            : (op2 == 0xB3) ? (v & ~(1u << bit))
                                            : (v ^ (1u << bit));
                if (opsize32_) rm_write32(target, nv); else rm_write16(target, uint16_t(nv));
            }
            if (op2 == 0xA3) return rm.is_mem ? 8 : 3;   // BT
            return rm.is_mem ? 13 : 6;                   // BTS/BTR/BTC
        }
        case 0xBA: {  // BT/BTS/BTR/BTC r/m, imm8
            RM rm = decode_modrm();
            int sub = last_reg_;
            int opsz = opsize32_ ? 32 : 16;
            int bit = fetch8() & (opsz - 1);
            if (sub < 4) {  // /0-/3 are undefined on a 486
                if (on_unimplemented) on_unimplemented(cs, instr_start_eip_, uint16_t(0x0F00 | op2));
                return 1;
            }
            uint32_t v = opsize32_ ? rm_read32(rm) : rm_read16(rm);
            set_flag(FLAG_CF, ((v >> bit) & 1) != 0);
            if (sub != 4) {
                uint32_t nv = (sub == 5) ? (v | (1u << bit))
                            : (sub == 6) ? (v & ~(1u << bit))
                                         : (v ^ (1u << bit));
                if (opsize32_) rm_write32(rm, nv); else rm_write16(rm, uint16_t(nv));
            }
            if (sub == 4) return 3;                      // BT r/m,imm8 -- 3 for both reg and mem
            return rm.is_mem ? 8 : 6;                    // BTS/BTR/BTC r/m,imm8
        }

        case 0xAF: {  // IMUL r16/32, r/m16/32 (two-operand form)
            RM rm = decode_modrm();
            uint32_t src;
            if (opsize32_) {
                src = rm_read32(rm);
                int64_t r = int64_t(int32_t(get_reg32(last_reg_))) * int64_t(int32_t(src));
                bool of = (int64_t(int32_t(uint32_t(r))) != r);
                set_flag(FLAG_CF, of); set_flag(FLAG_OF, of);
                set_reg32(last_reg_, uint32_t(r));
            } else {
                src = rm_read16(rm);
                int32_t r = int32_t(int16_t(get_reg16(last_reg_))) * int32_t(int16_t(uint16_t(src)));
                bool of = (int32_t(int16_t(uint16_t(r))) != r);
                set_flag(FLAG_CF, of); set_flag(FLAG_OF, of);
                set_reg16(last_reg_, uint16_t(r));
            }
            return mul_cost(src, opsize32_ ? 32 : 16);
        }

        case 0xB0: case 0xB1: {  // CMPXCHG r/m, r (486-native)
            RM rm = decode_modrm();
            bool wide = (op2 == 0xB1);
            bool equal;
            if (!wide) {
                uint8_t dst = rm_read8(rm), acc = get_reg8(0);
                sub8(acc, dst, false);              // flags exactly as CMP acc,r/m
                equal = (acc == dst);
                if (equal) rm_write8(rm, get_reg8(last_reg_));
                else set_reg8(0, dst);
            } else if (opsize32_) {
                uint32_t dst = rm_read32(rm);
                sub32(eax, dst, false);
                equal = (eax == dst);
                if (equal) rm_write32(rm, get_reg32(last_reg_));
                else eax = dst;
            } else {
                uint16_t dst = rm_read16(rm), acc = get_reg16(0);
                sub16(acc, dst, false);
                equal = (acc == dst);
                if (equal) rm_write16(rm, get_reg16(last_reg_));
                else set_reg16(0, dst);
            }
            // Published: 6 for a register destination, "7-10" for a memory
            // one. The natural reading of that range is the compare-only
            // case against the compare-plus-store case, which is what this
            // charges; Intel prints the range without saying which end is
            // which, so this is a labelled reading, not a cited split.
            if (!rm.is_mem) return 6;
            return equal ? 10 : 7;
        }

        case 0xB2: case 0xB4: case 0xB5: {  // LSS/LFS/LGS r16/32, m16:16 or m16:32
            RM rm = decode_modrm();
            int dst = last_reg_;
            uint32_t off;
            uint16_t seg;
            if (opsize32_) { off = read32(rm.seg, rm.off); seg = read16(rm.seg, seg_off(rm.off, 4)); }
            else { off = read16(rm.seg, rm.off); seg = read16(rm.seg, seg_off(rm.off, 2)); }
            // The segment register loads first, with every protected-mode
            // check; a fault there must leave the offset register untouched.
            load_seg((op2 == 0xB2) ? int(SEG_SS) : (op2 == 0xB4 ? int(SEG_FS) : int(SEG_GS)), seg);
            if (opsize32_) set_reg32(dst, off); else set_reg16(dst, uint16_t(off));
            return 6;
        }

        case 0xB6: case 0xB7: {  // MOVZX r16/32, r/m8 or r/m16
            RM rm = decode_modrm();
            uint32_t v = (op2 == 0xB6) ? rm_read8(rm) : rm_read16(rm);
            if (opsize32_) set_reg32(last_reg_, v); else set_reg16(last_reg_, uint16_t(v));
            return 3;
        }
        case 0xBE: case 0xBF: {  // MOVSX r16/32, r/m8 or r/m16
            RM rm = decode_modrm();
            int32_t v = (op2 == 0xBE) ? int32_t(int8_t(rm_read8(rm))) : int32_t(int16_t(rm_read16(rm)));
            if (opsize32_) set_reg32(last_reg_, uint32_t(v)); else set_reg16(last_reg_, uint16_t(v));
            return 3;
        }

        case 0xBC: case 0xBD: {  // BSF / BSR r16/32, r/m16/32
            RM rm = decode_modrm();
            uint32_t v = opsize32_ ? rm_read32(rm) : rm_read16(rm);
            int width = opsize32_ ? 32 : 16;
            // ZF is the only defined flag; the destination is left
            // UNDEFINED when the source is zero (Intel 80486 PRM), so this
            // core leaves the destination register untouched in that case
            // rather than inventing a value.
            if (v == 0) {
                set_flag(FLAG_ZF, true);
                return bitscan_cost(width, rm.is_mem);
            }
            set_flag(FLAG_ZF, false);
            int idx = 0, examined = 0;
            if (op2 == 0xBC) {
                for (int i = 0; i < width; ++i) { ++examined; if (v & (1u << i)) { idx = i; break; } }
            } else {
                for (int i = width - 1; i >= 0; --i) { ++examined; if (v & (1u << i)) { idx = i; break; } }
            }
            if (opsize32_) set_reg32(last_reg_, uint32_t(idx)); else set_reg16(last_reg_, uint16_t(idx));
            return bitscan_cost(examined, rm.is_mem);
        }

        case 0xC0: case 0xC1: {  // XADD r/m, r (486-native)
            RM rm = decode_modrm();
            if (op2 == 0xC0) {
                uint8_t dst = rm_read8(rm), src = get_reg8(last_reg_);
                uint8_t sum = add8(dst, src, false);
                set_reg8(last_reg_, dst);
                rm_write8(rm, sum);
            } else if (opsize32_) {
                uint32_t dst = rm_read32(rm), src = get_reg32(last_reg_);
                uint32_t sum = add32(dst, src, false);
                set_reg32(last_reg_, dst);
                rm_write32(rm, sum);
            } else {
                uint16_t dst = rm_read16(rm), src = get_reg16(last_reg_);
                uint16_t sum = add16(dst, src, false);
                set_reg16(last_reg_, dst);
                rm_write16(rm, sum);
            }
            return rm.is_mem ? 4 : 3;
        }

        case 0xC8: case 0xC9: case 0xCA: case 0xCB:
        case 0xCC: case 0xCD: case 0xCE: case 0xCF: {
            // BSWAP r32 (486-native): reverses the byte order of a 32-bit
            // register. Intel leaves the 16-bit-operand form undefined, so
            // this always operates on the full 32 bits.
            int r = op2 - 0xC8;
            uint32_t v = get_reg32(r);
            set_reg32(r, ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
                         ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24));
            return 1;
        }

        default:
            if (op2 >= 0x80 && op2 <= 0x8F) {  // Jcc rel16/rel32
                bool taken = cond(op2 & 0xF);
                if (opsize32_) {
                    int32_t rel = int32_t(fetch32());
                    if (taken) set_ip(uint32_t(eip + uint32_t(rel)));
                } else {
                    int16_t rel = int16_t(fetch16());
                    if (taken) set_ip(uint32_t(eip + uint32_t(int32_t(rel))));
                }
                return taken ? 3 : 1;  // published 486 Jcc: 1/3 (no jump/jump), same as the rel8 form
            }
            if (op2 >= 0x90 && op2 <= 0x9F) {  // SETcc r/m8
                RM rm = decode_modrm();
                bool t = cond(op2 & 0xF);
                rm_write8(rm, t ? 1 : 0);
                // Published 486 SETcc, "cycles are for: true/false":
                // r8 4/3, mem8 3/4 -- printed with the register and memory
                // cases inverted relative to each other, carried as
                // printed rather than smoothed out.
                if (rm.is_mem) return t ? 3 : 4;
                return t ? 4 : 3;
            }
            if (on_unimplemented) on_unimplemented(cs, instr_start_eip_, uint16_t(0x0F00 | op2));
            return 1;
    }
}

// --- x87 FPU --------------------------------------------------------------
//
// The Intel486 DX2 has the FPU on-die, so this is not an optional 387: it is
// part of the CPU, and CR0.ET reads 1 permanently. Architectural state is
// held in the real 80-bit extended format (Float80), not a host float, so
// FLD m80 / FSTP m80 round-trips are bit-exact for denormals, infinities and
// NaN payloads. Arithmetic is performed by converting to the host's
// `long double` and back.
//
// One labelled departure, stated plainly rather than implied: the host's
// `long double` is the genuine 80-bit extended format on x86-64 (where this
// is bit-exact) but a wider IEEE binary128 on arm64 and in a wasm build.
// There, computing in binary128 and then storing to 80-bit extended
// double-rounds, which can differ from the hardware's single rounding in the
// last bit of the significand for a small fraction of operands. Precision
// control (control-word bits 8-9) is applied on top, so a program that
// selects single or double precision gets exactly the width it asked for.
// Rounding control (bits 10-11) is honored where software actually depends
// on it -- the integer conversions FIST/FISTP/FRNDINT round explicitly per
// RC -- while arithmetic always rounds to nearest, the reset default; that
// last part is a documented gap, not a claim.

namespace {
// Status-word bits (Intel 80486 PRM, "Status Word").
constexpr uint16_t kFswIE = 1u << 0;   // invalid operation
constexpr uint16_t kFswDE = 1u << 1;   // denormalized operand
constexpr uint16_t kFswZE = 1u << 2;   // zero divide
constexpr uint16_t kFswOE = 1u << 3;   // overflow
constexpr uint16_t kFswUE = 1u << 4;   // underflow
constexpr uint16_t kFswPE = 1u << 5;   // precision
constexpr uint16_t kFswSF = 1u << 6;   // stack fault (a 387/486 addition to IE)
constexpr uint16_t kFswES = 1u << 7;   // error summary
constexpr uint16_t kFswC0 = 1u << 8;
constexpr uint16_t kFswC1 = 1u << 9;
constexpr uint16_t kFswC2 = 1u << 10;
constexpr uint16_t kFswC3 = 1u << 14;
constexpr uint16_t kFswB  = 1u << 15;  // busy; on a 486 it simply mirrors ES

// The "real indefinite" QNaN every masked invalid operation delivers.
constexpr uint64_t kIndefiniteSig = 0xC000000000000000ull;
constexpr uint16_t kIndefiniteExp = 0xFFFFu;

// Published i486 x87 cycle counts (the FPU rows of the same timing table
// used throughout this file). Where the table prints a range the floor is
// charged and the data-dependence is deliberately not modelled: unlike the
// integer MUL/BSF/RCL cases, Intel documents no mechanism for these ranges
// that could be fitted to both endpoints, so inventing a curve would be
// less honest than charging the published minimum and saying so.
constexpr int kFldMem32 = 3, kFldMem64 = 3, kFldMem80 = 6, kFldReg = 4;
constexpr int kFstMem32 = 7, kFstMem64 = 8, kFstMem80 = 6, kFstReg = 3;
constexpr int kFAdd = 8, kFMul = 16, kFDiv = 73, kFSqrt = 83, kFCom = 4;
constexpr int kFild = 13, kFist = 29, kFbld = 75, kFbstp = 175;
// The integer-operand arithmetic forms (FIADD/FISUB/FIMUL/FIDIV/FICOM) are
// published as their own, larger rows, because the operand has to be
// converted before the operation runs. This core charges the operation's
// published figure plus FILD's -- a stated composition of two published
// numbers rather than a citation of the combined row, since the combined
// rows are ranges whose mechanism Intel does not break down.
constexpr int kFldcw = 4, kFstcw = 3, kFstsw = 3, kFinit = 17, kFclex = 7;
constexpr int kFstenv = 67, kFldenv = 44, kFsave = 154, kFrstor = 131;
constexpr int kFsimple = 3, kFchs = 6, kFxam = 8, kFxch = 4, kFldconst = 8;
constexpr int kFprem = 70, kFrndint = 21, kFscale = 30, kFxtract = 16;
constexpr int kF2xm1 = 140, kFyl2x = 196, kFptan = 200, kFpatan = 218;
constexpr int kFsin = 257, kFsincos = 292;
}  // namespace

Float80 Cpu::to_float80(long double v) {
    Float80 f;
    bool neg = std::signbit(v);
    if (std::isnan(v)) {
        f.sign_exp = uint16_t(neg ? 0xFFFFu : 0x7FFFu);
        f.significand = kIndefiniteSig;
        return f;
    }
    if (std::isinf(v)) {
        f.sign_exp = uint16_t(neg ? 0xFFFFu : 0x7FFFu);
        f.significand = 0x8000000000000000ull;
        return f;
    }
    if (v == 0.0L) {
        f.sign_exp = uint16_t(neg ? 0x8000u : 0x0000u);
        f.significand = 0;
        return f;
    }
    long double a = neg ? -v : v;
    int exp2 = 0;
    long double m = std::frexp(a, &exp2);          // a = m * 2^exp2, 0.5 <= m < 1
    // The 80-bit format's value is M * 2^(E - 16383 - 63) with M carrying an
    // *explicit* integer bit, so scaling the [0.5,1) mantissa by 2^64 lands
    // M in [2^63, 2^64) and E = exp2 + 16382.
    long double scaled = std::ldexp(m, 64);
    uint64_t sig = uint64_t(scaled);
    int e = exp2 + 16382;
    if (e >= 0x7FFF) {                              // overflows the format: infinity
        f.sign_exp = uint16_t(neg ? 0xFFFFu : 0x7FFFu);
        f.significand = 0x8000000000000000ull;
        return f;
    }
    if (e <= 0) {
        // Denormal: shift the significand right until the exponent reaches
        // the format's floor, which is exactly what losing the implicit
        // normalization means here.
        int shift = 1 - e;
        sig = (shift >= 64) ? 0ull : (sig >> shift);
        e = 0;
    }
    f.sign_exp = uint16_t((neg ? 0x8000u : 0x0000u) | uint16_t(e));
    f.significand = sig;
    return f;
}

long double Cpu::from_float80(const Float80 &f) {
    bool neg = (f.sign_exp & 0x8000u) != 0;
    int e = f.sign_exp & 0x7FFFu;
    uint64_t m = f.significand;
    long double v;
    if (e == 0x7FFF) {
        v = ((m & 0x7FFFFFFFFFFFFFFFull) == 0) ? std::numeric_limits<long double>::infinity()
                                               : std::numeric_limits<long double>::quiet_NaN();
    } else if (m == 0) {
        v = 0.0L;
    } else {
        // Normals and denormals share one formula, because the explicit
        // integer bit means the significand is not scaled differently.
        v = std::ldexp(static_cast<long double>(m), (e == 0 ? 1 : e) - 16383 - 63);
    }
    return neg ? -v : v;
}

long double Cpu::st_value(int i) const { return from_float80(fpu_reg_[(fpu_top_ + i) & 7]); }

void Cpu::fpu_init() {
    // FINIT/RESET state (Intel 80486 PRM, "FPU Initialization"): control
    // word 037Fh -- all six exceptions masked, extended precision, round to
    // nearest -- status word 0, and every register tagged empty.
    fpu_cw_ = 0x037F;
    fpu_sw_ = 0;
    fpu_tw_ = 0xFFFF;
    fpu_top_ = 0;
    for (int i = 0; i < 8; ++i) fpu_reg_[i] = Float80{};
    fpu_last_ip_ = fpu_last_op_ = 0;
    fpu_last_cs_ = fpu_last_ds_ = fpu_last_opcode_ = 0;
}

void Cpu::fpu_check_available() {
    // CR0.EM routes every ESC opcode to #NM so a software emulator can pick
    // it up; CR0.TS does the same for the first FPU instruction after a task
    // switch, so a handler can swap the FPU's register stack between tasks.
    // On this machine EM is the interesting one only in principle -- the FPU
    // is on-die -- but both are real bits software sets.
    if (cr_[0] & (CR0_EM | CR0_TS)) raise(EXC_NM);
}

void Cpu::fpu_set_tag(int phys, bool empty) {
    int shift = (phys & 7) * 2;
    fpu_tw_ = uint16_t((fpu_tw_ & ~(0x3u << shift)) | (uint32_t(empty ? 3u : 0u) << shift));
}
bool Cpu::fpu_is_empty(int i) const {
    int phys = (fpu_top_ + i) & 7;
    return ((fpu_tw_ >> (phys * 2)) & 3u) == 3u;
}

void Cpu::fpu_stack_fault(bool overflow) {
    // A stack fault is an invalid-operation exception with SF also set; C1
    // distinguishes overflow (1) from underflow (0), which is the only way a
    // handler can tell which happened (Intel 80486 PRM, "Stack Fault").
    fpu_sw_ |= kFswIE | kFswSF;
    if (overflow) fpu_sw_ |= kFswC1; else fpu_sw_ &= ~kFswC1;
    if (!(fpu_cw_ & kFswIE)) fpu_sw_ |= kFswES | kFswB;
}

void Cpu::fpu_push(long double v) {
    int next = (fpu_top_ - 1) & 7;
    if (((fpu_tw_ >> (next * 2)) & 3u) != 3u) {
        // Pushing onto a full stack: the destination gets the indefinite
        // QNaN and the exception is flagged, rather than silently losing a
        // register's contents.
        fpu_stack_fault(true);
        fpu_top_ = next;
        fpu_reg_[next].sign_exp = kIndefiniteExp;
        fpu_reg_[next].significand = kIndefiniteSig;
        fpu_set_tag(next, false);
        return;
    }
    fpu_top_ = next;
    fpu_reg_[next] = to_float80(fpu_round_to_precision(v));
    fpu_set_tag(next, false);
}

long double Cpu::fpu_pop() {
    if (fpu_is_empty(0)) {
        fpu_stack_fault(false);
        return from_float80(Float80{kIndefiniteSig, kIndefiniteExp});
    }
    long double v = from_float80(fpu_reg_[fpu_top_]);
    fpu_set_tag(fpu_top_, true);
    fpu_top_ = (fpu_top_ + 1) & 7;
    return v;
}

long double Cpu::fpu_get(int i) const {
    if (fpu_is_empty(i)) return from_float80(Float80{kIndefiniteSig, kIndefiniteExp});
    return from_float80(fpu_reg_[(fpu_top_ + i) & 7]);
}

void Cpu::fpu_set(int i, long double v) {
    int phys = (fpu_top_ + i) & 7;
    fpu_reg_[phys] = to_float80(fpu_round_to_precision(v));
    fpu_set_tag(phys, false);
}

long double Cpu::fpu_round_to_precision(long double v) const {
    // Precision control (control word bits 8-9): 00 = 24-bit single,
    // 10 = 53-bit double, 11 = 64-bit extended, which is the reset default
    // and what a Watcom-compiled program normally leaves it at. 01 is
    // reserved and treated as extended.
    switch ((fpu_cw_ >> 8) & 3u) {
        case 0: return static_cast<long double>(static_cast<float>(v));
        case 2: return static_cast<long double>(static_cast<double>(v));
        default: return v;
    }
}

void Cpu::fpu_compare(long double a, long double b, bool unordered_ok) {
    // FCOM sets C3/C2/C0 as a three-way result, and an unordered comparison
    // (either operand a NaN) sets all three. FCOM signals #IA on an
    // unordered compare; FUCOM does not, which is the only difference
    // between them (Intel 80486 PRM, FCOM/FUCOM).
    fpu_sw_ &= ~(kFswC0 | kFswC2 | kFswC3);
    if (std::isnan(a) || std::isnan(b)) {
        fpu_sw_ |= kFswC0 | kFswC2 | kFswC3;
        if (!unordered_ok) {
            fpu_sw_ |= kFswIE;
            if (!(fpu_cw_ & kFswIE)) fpu_sw_ |= kFswES | kFswB;
        }
        return;
    }
    if (a > b) return;                       // C3=0 C2=0 C0=0
    if (a < b) { fpu_sw_ |= kFswC0; return; }
    fpu_sw_ |= kFswC3;
}

int Cpu::esc_op(uint8_t op) {
    // Decode the ModR/M first so instruction length is right on every path,
    // including the ones that fault.
    uint32_t opcode_eip = instr_start_eip_;
    RM rm = decode_modrm();
    int sub = last_reg_;
    int i = rm.is_mem ? 0 : rm.reg;      // ST(i) index for the register forms
    // The register-form encodings are conventionally written as whole
    // ModR/M bytes (D9 E0 = FCHS, DB E3 = FNINIT), so reassemble one.
    uint8_t modrm_low = uint8_t(0xC0u | (uint32_t(sub) << 3) | uint32_t(rm.is_mem ? 0 : rm.reg));

    fpu_check_available();
    // A pending unmasked exception is reported on the *next* FPU
    // instruction, not the one that caused it -- the 487/486 "deferred
    // error" behavior. The seven "no-wait" encodings below are specified not
    // to check, which is exactly why a handler can use FNSTSW and FNCLEX to
    // inspect and clear the very error it was called for.
    bool no_wait =
        (op == 0xDB && !rm.is_mem && (modrm_low == 0xE2 || modrm_low == 0xE3)) ||  // FNCLEX, FNINIT
        (op == 0xDF && !rm.is_mem && modrm_low == 0xE0) ||                         // FNSTSW AX
        (op == 0xDD && rm.is_mem && (sub == 6 || sub == 7)) ||                     // FNSAVE, FNSTSW m16
        (op == 0xD9 && rm.is_mem && (sub == 6 || sub == 7));                       // FNSTENV, FNSTCW
    if (!no_wait && (fpu_sw_ & kFswES) && (cr_[0] & CR0_NE)) raise(EXC_MF);

    // The operand pointers FSTENV/FSAVE hand a handler so it can find what
    // faulted. Recorded for every ESC instruction, as real hardware does.
    fpu_last_ip_ = opcode_eip;
    fpu_last_cs_ = cs;
    fpu_last_opcode_ = uint16_t(((op & 0x07u) << 8) | modrm_low);
    if (rm.is_mem) { fpu_last_op_ = rm.off; fpu_last_ds_ = seg_sel(rm.seg); }

    auto div_by_zero = [&]() {
        fpu_sw_ |= kFswZE;
        if (!(fpu_cw_ & kFswZE)) fpu_sw_ |= kFswES | kFswB;
    };
    auto arith = [&](int kind, long double a, long double b) -> long double {
        // kind: 0 ADD, 1 MUL, 4 SUB, 5 SUBR, 6 DIV, 7 DIVR -- the same
        // encoding order the ESC reg field uses.
        switch (kind) {
            case 0: return a + b;
            case 1: return a * b;
            case 4: return a - b;
            case 5: return b - a;
            case 6: if (b == 0.0L && !std::isnan(a) && a != 0.0L) div_by_zero(); return a / b;
            default: if (a == 0.0L && !std::isnan(b) && b != 0.0L) div_by_zero(); return b / a;
        }
    };
    auto arith_cost = [&](int kind) { return (kind == 1) ? kFMul : ((kind >= 6) ? kFDiv : kFAdd); };
    auto load_m32 = [&]() { uint32_t b = read32(rm.seg, rm.off); float f; std::memcpy(&f, &b, 4); return static_cast<long double>(f); };
    auto load_m64 = [&]() { uint64_t b = read64(rm.seg, rm.off); double d; std::memcpy(&d, &b, 8); return static_cast<long double>(d); };
    auto store_m32 = [&](long double v) { float f = static_cast<float>(v); uint32_t b; std::memcpy(&b, &f, 4); write32(rm.seg, rm.off, b); };
    auto store_m64 = [&](long double v) { double d = static_cast<double>(v); uint64_t b; std::memcpy(&b, &d, 8); write64(rm.seg, rm.off, b); };
    auto load_m80 = [&]() {
        Float80 f;
        f.significand = read64(rm.seg, rm.off);
        f.sign_exp = read16(rm.seg, seg_off(rm.off, 8));
        return f;
    };
    auto store_m80 = [&](const Float80 &f) {
        write64(rm.seg, rm.off, f.significand);
        write16(rm.seg, seg_off(rm.off, 8), f.sign_exp);
    };
    // Integer conversion honors the control word's rounding-control field,
    // which is the one place software genuinely depends on RC: a compiler's
    // (int) cast sets RC to truncate, does the FISTP, and restores it.
    auto round_per_rc = [&](long double v) -> long double {
        switch ((fpu_cw_ >> 10) & 3u) {
            case 0: return std::nearbyint(v);   // round to nearest, ties to even
            case 1: return std::floor(v);       // round down (toward -inf)
            case 2: return std::ceil(v);        // round up (toward +inf)
            default: return std::trunc(v);      // round toward zero
        }
    };
    auto store_int = [&](int bytes) {
        long double v = fpu_get(0);
        long double r = round_per_rc(v);
        // An out-of-range or NaN source is an invalid operation, and the
        // masked response is the format's "integer indefinite" -- the most
        // negative value it can hold, not a saturated maximum.
        int64_t lim_lo, lim_hi, indef;
        if (bytes == 2) { lim_lo = -32768; lim_hi = 32767; indef = -32768; }
        else if (bytes == 4) { lim_lo = -2147483648ll; lim_hi = 2147483647ll; indef = -2147483648ll; }
        else { lim_lo = INT64_MIN; lim_hi = INT64_MAX; indef = INT64_MIN; }
        int64_t out;
        if (std::isnan(r) || std::isinf(r) ||
            r < static_cast<long double>(lim_lo) || r > static_cast<long double>(lim_hi)) {
            fpu_sw_ |= kFswIE;
            if (!(fpu_cw_ & kFswIE)) fpu_sw_ |= kFswES | kFswB;
            out = indef;
        } else {
            out = int64_t(r);
            if (r != v) {
                fpu_sw_ |= kFswPE;
                if (!(fpu_cw_ & kFswPE)) fpu_sw_ |= kFswES | kFswB;
            }
        }
        if (bytes == 2) write16(rm.seg, rm.off, uint16_t(int16_t(out)));
        else if (bytes == 4) write32(rm.seg, rm.off, uint32_t(int32_t(out)));
        else write64(rm.seg, rm.off, uint64_t(out));
    };

    switch (op) {
        case 0xD8:
            if (rm.is_mem) {
                // The published figure for an arithmetic ESC opcode is the
                // same whether the operand is ST(i) or memory -- the table
                // gives one number for "FADD ST(i),ST / m32real / m64real" --
                // so the load is already inside it and must not be added.
                long double v = load_m32();
                if (sub == 2 || sub == 3) { fpu_compare(fpu_get(0), v, false); if (sub == 3) fpu_pop(); return kFCom; }
                fpu_set(0, arith(sub, fpu_get(0), v));
                return arith_cost(sub);
            }
            if (sub == 2 || sub == 3) { fpu_compare(fpu_get(0), fpu_get(i), false); if (sub == 3) fpu_pop(); return kFCom; }
            fpu_set(0, arith(sub, fpu_get(0), fpu_get(i)));
            return arith_cost(sub);
        case 0xDC:
            if (rm.is_mem) {
                long double v = load_m64();
                if (sub == 2 || sub == 3) { fpu_compare(fpu_get(0), v, false); if (sub == 3) fpu_pop(); return kFCom; }
                fpu_set(0, arith(sub, fpu_get(0), v));
                return arith_cost(sub);
            }
            // Register form: the destination is ST(i), not ST(0) -- and the
            // subtract/divide pairs are encoded *reversed* relative to the
            // D8 forms (DC E0+i is FSUBR, DC E8+i is FSUB), a genuine x87
            // encoding quirk that Intel documents and every assembler has to
            // special-case.
            {
                int kind = sub;
                if (sub == 4) kind = 5; else if (sub == 5) kind = 4;
                else if (sub == 6) kind = 7; else if (sub == 7) kind = 6;
                fpu_set(i, arith(kind, fpu_get(i), fpu_get(0)));
                return arith_cost(kind);
            }
        case 0xDE:
            if (rm.is_mem) {
                long double v = static_cast<long double>(int16_t(read16(rm.seg, rm.off)));
                if (sub == 2 || sub == 3) { fpu_compare(fpu_get(0), v, false); if (sub == 3) fpu_pop(); return kFCom + kFild; }
                fpu_set(0, arith(sub, fpu_get(0), v));
                return arith_cost(sub) + kFild;
            }
            if (modrm_low == 0xD9) {  // FCOMPP
                fpu_compare(fpu_get(0), fpu_get(1), false);
                fpu_pop(); fpu_pop();
                return kFCom;
            }
            {
                int kind = sub;
                if (sub == 4) kind = 5; else if (sub == 5) kind = 4;
                else if (sub == 6) kind = 7; else if (sub == 7) kind = 6;
                fpu_set(i, arith(kind, fpu_get(i), fpu_get(0)));
                fpu_pop();
                return arith_cost(kind);
            }
        case 0xDA:
            if (rm.is_mem) {
                long double v = static_cast<long double>(int32_t(read32(rm.seg, rm.off)));
                if (sub == 2 || sub == 3) { fpu_compare(fpu_get(0), v, false); if (sub == 3) fpu_pop(); return kFCom + kFild; }
                fpu_set(0, arith(sub, fpu_get(0), v));
                return arith_cost(sub) + kFild;
            }
            if (modrm_low == 0xE9) {  // FUCOMPP -- the 387/486 unordered compare
                fpu_compare(fpu_get(0), fpu_get(1), true);
                fpu_pop(); fpu_pop();
                return kFCom;
            }
            if (on_unimplemented) on_unimplemented(cs, opcode_eip, uint16_t(0xD800u | op));
            return kFsimple;
        case 0xD9:
            if (rm.is_mem) {
                switch (sub) {
                    case 0: fpu_push(load_m32()); return kFldMem32;
                    case 2: store_m32(fpu_get(0)); return kFstMem32;                 // FST m32
                    case 3: store_m32(fpu_get(0)); fpu_pop(); return kFstMem32;       // FSTP m32
                    case 4: {  // FLDENV
                        bool env32 = opsize32_;
                        uint32_t o = rm.off;
                        fpu_cw_ = read16(rm.seg, o);
                        uint16_t sw = read16(rm.seg, seg_off(o, env32 ? 4 : 2));
                        fpu_tw_ = read16(rm.seg, seg_off(o, env32 ? 8 : 4));
                        fpu_top_ = (sw >> 11) & 7;
                        fpu_sw_ = uint16_t(sw & ~0x3800u);
                        return kFldenv;
                    }
                    case 5: fpu_cw_ = read16(rm.seg, rm.off); return kFldcw;          // FLDCW
                    case 6: {  // FNSTENV -- the 14/28-byte environment
                        bool env32 = opsize32_;
                        uint32_t o = rm.off;
                        if (env32) {
                            write32(rm.seg, o, fpu_cw_);
                            write32(rm.seg, seg_off(o, 4), fpu_status());
                            write32(rm.seg, seg_off(o, 8), fpu_tw_);
                            write32(rm.seg, seg_off(o, 12), fpu_last_ip_);
                            write32(rm.seg, seg_off(o, 16), uint32_t(fpu_last_cs_) | (uint32_t(fpu_last_opcode_ & 0x07FFu) << 16));
                            write32(rm.seg, seg_off(o, 20), fpu_last_op_);
                            write32(rm.seg, seg_off(o, 24), fpu_last_ds_);
                        } else {
                            write16(rm.seg, o, fpu_cw_);
                            write16(rm.seg, seg_off(o, 2), fpu_status());
                            write16(rm.seg, seg_off(o, 4), fpu_tw_);
                            write16(rm.seg, seg_off(o, 6), uint16_t(fpu_last_ip_));
                            write16(rm.seg, seg_off(o, 8), uint16_t(((fpu_last_ip_ >> 16) << 12) | (fpu_last_opcode_ & 0x07FFu)));
                            write16(rm.seg, seg_off(o, 10), uint16_t(fpu_last_op_));
                            write16(rm.seg, seg_off(o, 12), uint16_t((fpu_last_op_ >> 16) << 12));
                        }
                        // FSTENV masks every exception afterwards, so the
                        // handler it is part of cannot immediately re-fault.
                        fpu_cw_ |= 0x003Fu;
                        return kFstenv;
                    }
                    default: write16(rm.seg, rm.off, fpu_cw_); return kFstcw;          // FNSTCW
                }
            }
            switch (modrm_low) {
                case 0xC0: case 0xC1: case 0xC2: case 0xC3:
                case 0xC4: case 0xC5: case 0xC6: case 0xC7:
                    fpu_push(fpu_get(modrm_low - 0xC0));                                 // FLD ST(i)
                    return kFldReg;
                case 0xC8: case 0xC9: case 0xCA: case 0xCB:
                case 0xCC: case 0xCD: case 0xCE: case 0xCF: {                            // FXCH ST(i)
                    int j = modrm_low - 0xC8;
                    Float80 t = fpu_reg_[fpu_top_];
                    fpu_reg_[fpu_top_] = fpu_reg_[(fpu_top_ + j) & 7];
                    fpu_reg_[(fpu_top_ + j) & 7] = t;
                    return kFxch;
                }
                case 0xD0: return kFsimple;                                              // FNOP
                case 0xE0: fpu_reg_[fpu_top_].sign_exp = uint16_t(fpu_reg_[fpu_top_].sign_exp ^ 0x8000u); return kFchs;   // FCHS
                case 0xE1: fpu_reg_[fpu_top_].sign_exp = uint16_t(fpu_reg_[fpu_top_].sign_exp & 0x7FFFu); return kFsimple; // FABS
                case 0xE4: fpu_compare(fpu_get(0), 0.0L, false); return kFCom;           // FTST
                case 0xE5: {  // FXAM -- classifies ST(0) into C3/C2/C0 with C1 = sign
                    fpu_sw_ &= ~(kFswC0 | kFswC1 | kFswC2 | kFswC3);
                    const Float80 &f = fpu_reg_[fpu_top_];
                    if (f.sign_exp & 0x8000u) fpu_sw_ |= kFswC1;
                    int e = f.sign_exp & 0x7FFFu;
                    if (fpu_is_empty(0)) fpu_sw_ |= kFswC3 | kFswC0;                     // empty: 101
                    else if (e == 0x7FFF) {
                        if ((f.significand & 0x7FFFFFFFFFFFFFFFull) == 0) fpu_sw_ |= kFswC2 | kFswC0;  // infinity: 011
                        else fpu_sw_ |= kFswC0;                                          // NaN: 001
                    } else if (e == 0 && f.significand == 0) fpu_sw_ |= kFswC3;           // zero: 100
                    else if (e == 0) fpu_sw_ |= kFswC3 | kFswC2;                          // denormal: 110
                    else fpu_sw_ |= kFswC2;                                              // normal: 010
                    return kFxam;
                }
                case 0xE8: fpu_push(1.0L); return kFldconst;                             // FLD1
                case 0xE9: fpu_push(3.321928094887362347870319429489390175864831393024580612054L); return kFldconst;  // FLDL2T
                case 0xEA: fpu_push(1.442695040888963407359924681001892137426645954152985934135L); return kFldconst;  // FLDL2E
                case 0xEB: fpu_push(3.141592653589793238462643383279502884197169399375105820975L); return kFldconst;  // FLDPI
                case 0xEC: fpu_push(0.301029995663981195213738894724493026768189881462108541310L); return kFldconst;  // FLDLG2
                case 0xED: fpu_push(0.693147180559945309417232121458176568075500134360255254120L); return kFldconst;  // FLDLN2
                case 0xEE: fpu_push(0.0L); return kFldconst;                             // FLDZ
                case 0xF0: fpu_set(0, std::exp2(fpu_get(0)) - 1.0L); return kF2xm1;      // F2XM1
                case 0xF1: {  // FYL2X: ST(1) * log2(ST(0)), popping
                    long double x = fpu_pop();
                    fpu_set(0, fpu_get(0) * std::log2(x));
                    return kFyl2x;
                }
                case 0xF2: {  // FPTAN: replaces ST(0) with tan, then pushes 1.0
                    long double t = std::tan(fpu_get(0));
                    fpu_set(0, t);
                    fpu_sw_ &= ~kFswC2;   // C2 = 0 means the argument was in range
                    fpu_push(1.0L);
                    return kFptan;
                }
                case 0xF3: {  // FPATAN: atan(ST(1)/ST(0)), popping
                    long double x = fpu_pop();
                    fpu_set(0, std::atan2(fpu_get(0), x));
                    return kFpatan;
                }
                case 0xF4: {  // FXTRACT: splits ST(0) into exponent and significand
                    long double v = fpu_get(0);
                    if (v == 0.0L) { div_by_zero(); fpu_set(0, -std::numeric_limits<long double>::infinity()); fpu_push(0.0L); return kFxtract; }
                    int e = 0;
                    long double m = std::frexp(v, &e);
                    fpu_set(0, static_cast<long double>(e - 1));
                    fpu_push(std::ldexp(m, 1));   // significand in [1,2)
                    return kFxtract;
                }
                case 0xF5: case 0xF8: {  // FPREM1 (F5, IEEE) and FPREM (F8, 8087)
                    long double a = fpu_get(0), b = fpu_get(1);
                    long double r = (modrm_low == 0xF5) ? std::remainder(a, b) : std::fmod(a, b);
                    fpu_set(0, r);
                    // C2 = 0 says the reduction is complete. Real hardware
                    // can leave it set after a partial reduction of a huge
                    // argument and expects the loop to run again; computing
                    // the exact remainder in one step means it is always
                    // complete here -- a labelled simplification, and the
                    // observable contract (C2 clear, correct remainder) is
                    // the one software tests.
                    fpu_sw_ &= ~kFswC2;
                    return kFprem;
                }
                case 0xF6: fpu_top_ = (fpu_top_ - 1) & 7; return kFsimple;               // FDECSTP
                case 0xF7: fpu_top_ = (fpu_top_ + 1) & 7; return kFsimple;               // FINCSTP
                case 0xF9: {  // FYL2XP1: ST(1) * log2(ST(0)+1), popping
                    long double x = fpu_pop();
                    fpu_set(0, fpu_get(0) * std::log2(x + 1.0L));
                    return kFyl2x;
                }
                case 0xFA: {  // FSQRT
                    long double v = fpu_get(0);
                    if (v < 0.0L) {
                        fpu_sw_ |= kFswIE;
                        if (!(fpu_cw_ & kFswIE)) fpu_sw_ |= kFswES | kFswB;
                        fpu_reg_[fpu_top_] = Float80{kIndefiniteSig, kIndefiniteExp};
                    } else {
                        fpu_set(0, std::sqrt(v));
                    }
                    return kFSqrt;
                }
                case 0xFB: {  // FSINCOS
                    long double v = fpu_get(0);
                    fpu_set(0, std::sin(v));
                    fpu_sw_ &= ~kFswC2;
                    fpu_push(std::cos(v));
                    return kFsincos;
                }
                case 0xFC: fpu_set(0, round_per_rc(fpu_get(0))); return kFrndint;         // FRNDINT
                case 0xFD: {  // FSCALE: ST(0) * 2^trunc(ST(1))
                    long double s = std::trunc(fpu_get(1));
                    fpu_set(0, std::ldexp(fpu_get(0), int(s)));
                    return kFscale;
                }
                case 0xFE: fpu_set(0, std::sin(fpu_get(0))); fpu_sw_ &= ~kFswC2; return kFsin;  // FSIN
                case 0xFF: fpu_set(0, std::cos(fpu_get(0))); fpu_sw_ &= ~kFswC2; return kFsin;  // FCOS
                default:
                    if (on_unimplemented) on_unimplemented(cs, opcode_eip, uint16_t(0xD800u | op));
                    return kFsimple;
            }
        case 0xDB:
            if (rm.is_mem) {
                switch (sub) {
                    case 0: fpu_push(static_cast<long double>(int32_t(read32(rm.seg, rm.off)))); return kFild;  // FILD m32int
                    case 2: store_int(4); return kFist;                                            // FIST m32int
                    case 3: store_int(4); fpu_pop(); return kFist;                                 // FISTP m32int
                    case 5: {  // FLD m80real -- the one load that is bit-exact by construction
                        Float80 f = load_m80();
                        int next = (fpu_top_ - 1) & 7;
                        if (((fpu_tw_ >> (next * 2)) & 3u) != 3u) { fpu_stack_fault(true); fpu_top_ = next; fpu_reg_[next] = Float80{kIndefiniteSig, kIndefiniteExp}; }
                        else { fpu_top_ = next; fpu_reg_[next] = f; }
                        fpu_set_tag(fpu_top_, false);
                        return kFldMem80;
                    }
                    case 7: {  // FSTP m80real
                        store_m80(fpu_reg_[fpu_top_]);
                        fpu_set_tag(fpu_top_, true);
                        fpu_top_ = (fpu_top_ + 1) & 7;
                        return kFstMem80;
                    }
                    default:
                        if (on_unimplemented) on_unimplemented(cs, opcode_eip, uint16_t(0xD800u | op));
                        return kFsimple;
                }
            }
            switch (modrm_low) {
                // FENI/FDISI were 8087 interrupt-enable controls; the 287
                // onward implements them as no-ops, and FSETPM was the 287's
                // "enter protected mode" hint that the 387 and 486 likewise
                // ignore. Genuine no-ops on this part, not gaps.
                case 0xE0: case 0xE1: case 0xE4: return kFsimple;
                case 0xE2: fpu_sw_ &= ~(kFswIE | kFswDE | kFswZE | kFswOE | kFswUE | kFswPE | kFswSF | kFswES | kFswB); return kFclex;  // FNCLEX
                case 0xE3: fpu_init(); return kFinit;                                     // FNINIT
                default:
                    if (on_unimplemented) on_unimplemented(cs, opcode_eip, uint16_t(0xD800u | op));
                    return kFsimple;
            }
        case 0xDD:
            if (rm.is_mem) {
                switch (sub) {
                    case 0: fpu_push(load_m64()); return kFldMem64;                       // FLD m64
                    case 2: store_m64(fpu_get(0)); return kFstMem64;                      // FST m64
                    case 3: store_m64(fpu_get(0)); fpu_pop(); return kFstMem64;            // FSTP m64
                    case 4: {  // FRSTOR -- environment plus all eight registers
                        bool env32 = opsize32_;
                        uint32_t o = rm.off;
                        uint32_t hdr = env32 ? 28 : 14;
                        fpu_cw_ = read16(rm.seg, o);
                        uint16_t sw = read16(rm.seg, seg_off(o, env32 ? 4 : 2));
                        fpu_tw_ = read16(rm.seg, seg_off(o, env32 ? 8 : 4));
                        fpu_top_ = (sw >> 11) & 7;
                        fpu_sw_ = uint16_t(sw & ~0x3800u);
                        for (int r = 0; r < 8; ++r) {
                            uint32_t base = seg_off(o, hdr + uint32_t(r) * 10);
                            Float80 f;
                            f.significand = read64(rm.seg, base);
                            f.sign_exp = read16(rm.seg, seg_off(base, 8));
                            fpu_reg_[(fpu_top_ + r) & 7] = f;
                        }
                        return kFrstor;
                    }
                    case 6: {  // FNSAVE -- environment plus all eight registers, then reset
                        bool env32 = opsize32_;
                        uint32_t o = rm.off;
                        uint32_t hdr = env32 ? 28 : 14;
                        if (env32) {
                            write32(rm.seg, o, fpu_cw_);
                            write32(rm.seg, seg_off(o, 4), fpu_status());
                            write32(rm.seg, seg_off(o, 8), fpu_tw_);
                            write32(rm.seg, seg_off(o, 12), fpu_last_ip_);
                            write32(rm.seg, seg_off(o, 16), uint32_t(fpu_last_cs_) | (uint32_t(fpu_last_opcode_ & 0x07FFu) << 16));
                            write32(rm.seg, seg_off(o, 20), fpu_last_op_);
                            write32(rm.seg, seg_off(o, 24), fpu_last_ds_);
                        } else {
                            write16(rm.seg, o, fpu_cw_);
                            write16(rm.seg, seg_off(o, 2), fpu_status());
                            write16(rm.seg, seg_off(o, 4), fpu_tw_);
                            write16(rm.seg, seg_off(o, 6), uint16_t(fpu_last_ip_));
                            write16(rm.seg, seg_off(o, 8), uint16_t(fpu_last_opcode_ & 0x07FFu));
                            write16(rm.seg, seg_off(o, 10), uint16_t(fpu_last_op_));
                            write16(rm.seg, seg_off(o, 12), 0);
                        }
                        for (int r = 0; r < 8; ++r) {
                            uint32_t base = seg_off(o, hdr + uint32_t(r) * 10);
                            const Float80 &f = fpu_reg_[(fpu_top_ + r) & 7];
                            write64(rm.seg, base, f.significand);
                            write16(rm.seg, seg_off(base, 8), f.sign_exp);
                        }
                        fpu_init();   // FSAVE leaves the FPU in its reset state
                        return kFsave;
                    }
                    default: write16(rm.seg, rm.off, fpu_status()); return kFstsw;         // FNSTSW m16
                }
            }
            switch (sub) {
                case 0: fpu_set_tag((fpu_top_ + i) & 7, true); return kFsimple;            // FFREE ST(i)
                case 2: fpu_set(i, fpu_get(0)); return kFstReg;                            // FST ST(i)
                case 3: fpu_set(i, fpu_get(0)); fpu_pop(); return kFstReg;                 // FSTP ST(i)
                default:
                    if (on_unimplemented) on_unimplemented(cs, opcode_eip, uint16_t(0xD800u | op));
                    return kFsimple;
            }
        default:  // 0xDF
            if (rm.is_mem) {
                switch (sub) {
                    case 0: fpu_push(static_cast<long double>(int16_t(read16(rm.seg, rm.off)))); return kFild;  // FILD m16int
                    case 2: store_int(2); return kFist;                                            // FIST m16int
                    case 3: store_int(2); fpu_pop(); return kFist;                                 // FISTP m16int
                    case 4: {  // FBLD m80bcd
                        // Bytes 0-8 carry 18 packed decimal digits, two per
                        // byte and least significant byte first; byte 9's
                        // top bit is the sign and the rest is ignored.
                        long double v = 0.0L;
                        for (int b = 8; b >= 0; --b) {
                            uint8_t byte = read8(rm.seg, seg_off(rm.off, uint32_t(b)));
                            v = v * 100.0L
                                + static_cast<long double>((byte >> 4) & 0x0F) * 10.0L
                                + static_cast<long double>(byte & 0x0F);
                        }
                        uint8_t sign = read8(rm.seg, seg_off(rm.off, 9));
                        fpu_push((sign & 0x80) ? -v : v);
                        return kFbld;
                    }
                    case 5: fpu_push(static_cast<long double>(int64_t(read64(rm.seg, rm.off)))); return kFild;   // FILD m64int
                    case 6: {  // FBSTP m80bcd
                        long double v = fpu_get(0);
                        bool neg = std::signbit(v);
                        long double a = std::trunc(neg ? -v : v);
                        for (int b = 0; b < 9; ++b) {
                            long double d0 = std::fmod(a, 10.0L); a = std::trunc(a / 10.0L);
                            long double d1 = std::fmod(a, 10.0L); a = std::trunc(a / 10.0L);
                            write8(rm.seg, seg_off(rm.off, uint32_t(b)),
                                   uint8_t((int(d1) << 4) | int(d0)));
                        }
                        write8(rm.seg, seg_off(rm.off, 9), uint8_t(neg ? 0x80 : 0x00));
                        fpu_pop();
                        return kFbstp;
                    }
                    default: store_int(8); fpu_pop(); return kFist;                                 // FISTP m64int
                }
            }
            if (modrm_low == 0xE0) { set_reg16(0, fpu_status()); return kFstsw; }                    // FNSTSW AX
            if (on_unimplemented) on_unimplemented(cs, opcode_eip, uint16_t(0xD800u | op));
            return kFsimple;
    }
}

// --- main decode loop ------------------------------------------------------

int Cpu::step() {
    // HLT idles until an interrupt arrives. Charged the published 486 HLT
    // cost per idle step so the embedding machine's wall-clock pacing keeps
    // advancing at a sane rate while halted.
    if (halted) { cycles += 4; return 4; }
    // In real mode a segment register's base simply tracks selector*16, so
    // a host or test assigning one of the public selector fields directly
    // is a segment load and must re-derive the base (see
    // refresh_real_bases()).
    if (!protected_mode()) refresh_real_bases();
    uint32_t start_eip = eip;
    instr_start_esp_ = esp;
    instr_start_ss_ = ss;
    instr_start_ss_desc_ = sd_[SEG_SS];
    try {
        return step_inner();
    } catch (const Fault &f) {
        return deliver_fault(f, start_eip);
    }
}

int Cpu::deliver_fault(const Fault &first, uint32_t start_eip) {
    // A *fault* (as opposed to a trap) reports the address of the faulting
    // instruction and is restartable, so the instruction's own starting EIP
    // and stack pointer are put back before the handler sees them -- a
    // half-pushed operand left behind would make the restart write it twice
    // (Intel 80486 PRM, "Exception Classes").
    eip = start_eip;
    esp = instr_start_esp_;
    ss = instr_start_ss_;
    sd_[SEG_SS] = instr_start_ss_desc_;
    Fault f = first;
    if (on_fault) on_fault(f.vector, f.error, cs, eip);
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            do_interrupt(uint8_t(f.vector), false, f.has_error, f.error);
            // Published 486 INT cost: 26 for the real-mode vectoring work
            // (INT3's figure, the closest anchored one -- see interrupt()),
            // 44 for a protected-mode gate at the same privilege level. The
            // inter-privilege figure is higher again; charged as the same 44
            // here rather than asserting a split this core cannot cite.
            int c = protected_mode() ? 44 : 26;
            cycles += c;
            return c;
        } catch (const Fault &) {
            // A fault taken while *delivering* a fault is a double fault,
            // and a fault while delivering that is shutdown -- the CPU
            // stops until RESET or NMI (Intel 80486 PRM, "Double Fault").
            if (f.vector == EXC_DF) break;
            f = Fault{EXC_DF, 0, true};
            if (on_fault) on_fault(EXC_DF, 0, cs, eip);
        }
    }
    halted = true;
    cycles += 4;
    return 4;
}

namespace {
// The eleven x86 prefix bytes, by opcode. Same set the prefix loop below used
// to switch over; 0 means "not a prefix".
enum : uint8_t {
    kPfxLock = 1, kPfxSegEs, kPfxSegCs, kPfxSegSs, kPfxSegDs, kPfxSegFs, kPfxSegGs,
    kPfxOpsize, kPfxAddrsize, kPfxRepNz, kPfxRepZ,
};
constexpr uint8_t MakePrefixTable(int i) {
    return i == 0x26 ? kPfxSegEs : i == 0x2E ? kPfxSegCs : i == 0x36 ? kPfxSegSs
         : i == 0x3E ? kPfxSegDs : i == 0x64 ? kPfxSegFs : i == 0x65 ? kPfxSegGs
         : i == 0x66 ? kPfxOpsize : i == 0x67 ? kPfxAddrsize : i == 0xF0 ? kPfxLock
         : i == 0xF2 ? kPfxRepNz : i == 0xF3 ? kPfxRepZ : uint8_t(0);
}
template <int... I>
constexpr std::array<uint8_t, 256> BuildPrefixTable(std::integer_sequence<int, I...>) {
    return {{MakePrefixTable(I)...}};
}
constexpr std::array<uint8_t, 256> kPrefix = BuildPrefixTable(std::make_integer_sequence<int, 256>{});
}  // namespace

int Cpu::step_inner() {
    prefetch_revalidate();
    seg_override_ = -1;
    rep_ = REP_NONE;
    // The default operand and address size is the code segment's D bit --
    // 16 bits in real mode always, and whatever the descriptor says in
    // protected mode. The 0x66/0x67 prefixes *toggle* that default rather
    // than selecting 32 bits outright, which is why the same prefix byte
    // means "use 32-bit operands" in a 16-bit segment and "use 16-bit
    // operands" in a 32-bit one.
    bool dflt32 = code32();
    opsize32_ = dflt32;
    addrsize32_ = dflt32;
    extra_cycles_ = 0;
    int c = 0;

    uint8_t op;
    for (;;) {
        op = fetch8();
        // One table lookup decides prefix-or-not. Most instructions carry no
        // prefix at all, and dispatching the question through a switch cost
        // 4.5% of a BOOM run for an answer that is "no" nearly every time
        // (PC486_REVIEW.md §16). kPrefix is the same eleven bytes the switch
        // listed: 0 = not a prefix.
        uint8_t kind = kPrefix[op];
        if (kind == 0) break;
        switch (kind) {
            case kPfxSegEs: seg_override_ = SEG_ES; break;
            case kPfxSegCs: seg_override_ = SEG_CS; break;
            case kPfxSegSs: seg_override_ = SEG_SS; break;
            case kPfxSegDs: seg_override_ = SEG_DS; break;
            case kPfxSegFs: seg_override_ = SEG_FS; break;  // FS: -- 386 addition, native on a 486
            case kPfxSegGs: seg_override_ = SEG_GS; break;  // GS:
            case kPfxOpsize: opsize32_ = !dflt32; break;    // operand-size override (toggles CS.D)
            case kPfxAddrsize: addrsize32_ = !dflt32; break; // address-size override (toggles CS.D)
            case kPfxRepNz: rep_ = REP_NZ; break;
            case kPfxRepZ: rep_ = REP_Z; break;
            default: break;                                 // LOCK -- no other bus master in this machine
        }
        // The 486 column publishes a cost only for the LOCK prefix (1
        // clock); it gives no separate figure for the segment-override,
        // operand-size or address-size prefixes, which the 486 decodes one
        // per clock. Charging that same published 1 clock per prefix byte
        // is therefore the cited figure for LOCK and a labelled, uniform
        // extension of it for the others.
        c += 1;
    }
    instr_start_eip_ = (eip - 1) & ip_mask();  // eip has already advanced past `op`

    // Fast path: the dense ALU block 0x00-0x3D, laid out as 8 groups of 6
    // opcodes (formats rm8,r8 / rm,r / r8,rm8 / r,rm / AL,imm8 / eAX,imm --
    // the +6/+7 slots in each group are segment push/pop or BCD adjust and
    // are excluded here, since (op & 7) > 5 for all of them).
    if (op < 0x40 && (op & 7) <= 5) {
        int alu = op >> 3;
        switch (op & 7) {
            case 0: { RM rm = decode_modrm(); uint8_t res = alu_apply8(alu, rm_read8(rm), get_reg8(last_reg_)); if (alu != 7) rm_write8(rm, res); c += alu_cost(alu, rm.is_mem, rm.is_mem); break; }
            case 1: {
                RM rm = decode_modrm();
                if (opsize32_) { uint32_t res = alu_apply32(alu, rm_read32(rm), get_reg32(last_reg_)); if (alu != 7) rm_write32(rm, res); }
                else { uint16_t res = alu_apply16(alu, rm_read16(rm), get_reg16(last_reg_)); if (alu != 7) rm_write16(rm, res); }
                c += alu_cost(alu, rm.is_mem, rm.is_mem);
                break;
            }
            case 2: { RM rm = decode_modrm(); uint8_t res = alu_apply8(alu, get_reg8(last_reg_), rm_read8(rm)); if (alu != 7) set_reg8(last_reg_, res); c += alu_cost(alu, false, rm.is_mem); break; }
            case 3: {
                RM rm = decode_modrm();
                if (opsize32_) { uint32_t res = alu_apply32(alu, get_reg32(last_reg_), rm_read32(rm)); if (alu != 7) set_reg32(last_reg_, res); }
                else { uint16_t res = alu_apply16(alu, get_reg16(last_reg_), rm_read16(rm)); if (alu != 7) set_reg16(last_reg_, res); }
                c += alu_cost(alu, false, rm.is_mem);
                break;
            }
            case 4: { uint8_t imm = fetch8(); uint8_t res = alu_apply8(alu, get_reg8(0), imm); if (alu != 7) set_reg8(0, res); c += 1; break; }
            default: {
                if (opsize32_) { uint32_t imm = fetch32(); uint32_t res = alu_apply32(alu, eax, imm); if (alu != 7) eax = res; }
                else { uint16_t imm = fetch16(); uint16_t res = alu_apply16(alu, get_reg16(0), imm); if (alu != 7) set_reg16(0, res); }
                c += 1;
                break;
            }
        }
        c += extra_cycles_;
        cycles += c;
        return c;
    }

    switch (op) {
        // --- segment push/pop (published 486: PUSH sreg 3, POP sreg 3) ---
        case 0x06: if (opsize32_) push32(es); else push16(es); c += 3; break;
        case 0x07: load_seg(SEG_ES, opsize32_ ? uint16_t(pop32()) : pop16()); c += 3; break;
        case 0x0E: if (opsize32_) push32(cs); else push16(cs); c += 3; break;
        case 0x16: if (opsize32_) push32(ss); else push16(ss); c += 3; break;
        case 0x17: load_seg(SEG_SS, opsize32_ ? uint16_t(pop32()) : pop16()); c += 3; break;
        case 0x1E: if (opsize32_) push32(ds); else push16(ds); c += 3; break;
        case 0x1F: load_seg(SEG_DS, opsize32_ ? uint16_t(pop32()) : pop16()); c += 3; break;

        case 0x0F: c += two_byte(); break;

        case 0x27: daa(); c += 2; break;
        case 0x2F: das(); c += 2; break;
        case 0x37: aaa(); c += 3; break;
        case 0x3F: aas(); c += 3; break;

        // --- INC/DEC reg (published 486: 1; never affects CF) ---
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47: {
            int r = op - 0x40; bool cf = flag(FLAG_CF);
            if (opsize32_) set_reg32(r, add32(get_reg32(r), 1, false));
            else set_reg16(r, add16(get_reg16(r), 1, false));
            set_flag(FLAG_CF, cf); c += 1;
            break;
        }
        case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
            int r = op - 0x48; bool cf = flag(FLAG_CF);
            if (opsize32_) set_reg32(r, sub32(get_reg32(r), 1, false));
            else set_reg16(r, sub16(get_reg16(r), 1, false));
            set_flag(FLAG_CF, cf); c += 1;
            break;
        }

        // --- PUSH/POP reg (published 486: 1 each) ---
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            // PUSH SP/ESP pushes the value from before the decrement on
            // the 286 and every later part -- so no special case is
            // needed, unlike the 8086 (see this file's header comment).
            if (opsize32_) push32(get_reg32(op - 0x50)); else push16(get_reg16(op - 0x50));
            c += 1;
            break;
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
            int r = op - 0x58;
            if (opsize32_) set_reg32(r, pop32()); else set_reg16(r, pop16());
            c += 1;
            break;
        }

        case 0x60: pusha(); c += 11; break;  // PUSHA/PUSHAD
        case 0x61: popa(); c += 9; break;    // POPA/POPAD
        case 0x62: bound(); c += 7; break;
        case 0x63: {
            // ARPL r/m16, r16 -- "adjust requested privilege level": raises
            // the destination selector's RPL to the source's if it is more
            // privileged, setting ZF when it had to change anything. An
            // operating system uses it on a selector a less privileged
            // caller handed in, so the caller cannot smuggle in a selector
            // more privileged than itself. Protected-mode only: a real 486
            // raises #UD for this opcode in real mode.
            RM rm = decode_modrm();
            int src = last_reg_;
            if (!protected_mode()) raise(EXC_UD);
            uint16_t dst = rm_read16(rm);
            uint16_t src_rpl = uint16_t(get_reg16(src) & 3);
            if ((dst & 3) < src_rpl) {
                rm_write16(rm, uint16_t((dst & 0xFFFCu) | src_rpl));
                set_flag(FLAG_ZF, true);
            } else {
                set_flag(FLAG_ZF, false);
            }
            c += 9;
            break;
        }
        case 0x68: if (opsize32_) push32(fetch32()); else push16(fetch16()); c += 1; break;
        case 0x69: {  // IMUL r,r/m,imm16/32
            RM rm = decode_modrm(); int r = last_reg_;
            uint32_t imm = opsize32_ ? fetch32() : fetch16();
            imul_imm(r, rm, imm);
            c += mul_cost(imm, opsize32_ ? 32 : 16);
            break;
        }
        case 0x6A: { int8_t imm = int8_t(fetch8()); if (opsize32_) push32(uint32_t(int32_t(imm))); else push16(uint16_t(int16_t(imm))); c += 1; break; }
        case 0x6B: {  // IMUL r,r/m,imm8 (sign-extended)
            RM rm = decode_modrm(); int r = last_reg_;
            uint32_t imm = uint32_t(int32_t(int8_t(fetch8())));
            imul_imm(r, rm, imm);
            c += mul_cost(imm, opsize32_ ? 32 : 16);
            break;
        }
        case 0x6C: case 0x6D: case 0x6E: case 0x6F: c += io_string_op(op); break;

        // --- Jcc rel8 (published 486: 1 not taken / 3 taken) ---
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
            bool taken = cond(op & 0xF);
            jcc_rel8(taken);
            c += taken ? 3 : 1;
            break;
        }

        case 0x80: case 0x81: case 0x82: case 0x83: c += grp1_immed(op); break;

        // --- TEST / XCHG r/m,r ---
        case 0x84: { RM rm = decode_modrm(); and8(rm_read8(rm), get_reg8(last_reg_)); c += rm.is_mem ? 2 : 1; break; }
        case 0x85: {
            RM rm = decode_modrm();
            if (opsize32_) and32(rm_read32(rm), get_reg32(last_reg_));
            else and16(rm_read16(rm), get_reg16(last_reg_));
            c += rm.is_mem ? 2 : 1;
            break;
        }
        case 0x86: { RM rm = decode_modrm(); uint8_t a = get_reg8(last_reg_), b = rm_read8(rm); set_reg8(last_reg_, b); rm_write8(rm, a); c += rm.is_mem ? 5 : 3; break; }
        case 0x87: {
            RM rm = decode_modrm();
            if (opsize32_) { uint32_t a = get_reg32(last_reg_), b = rm_read32(rm); set_reg32(last_reg_, b); rm_write32(rm, a); }
            else { uint16_t a = get_reg16(last_reg_), b = rm_read16(rm); set_reg16(last_reg_, b); rm_write16(rm, a); }
            c += rm.is_mem ? 5 : 3;
            break;
        }

        // --- MOV r/m,r and r,r/m. Published 486: 1 clock in every
        // --- direction and for both operand locations, thanks to the
        // --- on-chip cache making a load or store a one-cycle operation.
        // --- (The 286's own directional 3/5 asymmetry is gone.)
        case 0x88: { RM rm = decode_modrm(); rm_write8(rm, get_reg8(last_reg_)); c += 1; break; }
        case 0x89: { RM rm = decode_modrm(); if (opsize32_) rm_write32(rm, get_reg32(last_reg_)); else rm_write16(rm, get_reg16(last_reg_)); c += 1; break; }
        case 0x8A: { RM rm = decode_modrm(); set_reg8(last_reg_, rm_read8(rm)); c += 1; break; }
        case 0x8B: { RM rm = decode_modrm(); if (opsize32_) set_reg32(last_reg_, rm_read32(rm)); else set_reg16(last_reg_, rm_read16(rm)); c += 1; break; }
        case 0x8C: {  // MOV r/m16, sreg
            RM rm = decode_modrm();
            if ((last_reg_ & 7) > SEG_GS) raise(EXC_UD);  // reg fields 6 and 7 are not segment registers
            rm_write16(rm, seg_sel(last_reg_ & 7));
            c += 3;
            break;
        }
        case 0x8D: {  // LEA
            RM rm = decode_modrm();
            // Reads the full 32-bit effective address, not the
            // 64KB-limited memory offset -- LEA touches no memory, so a
            // 32-bit addressing form here is pure arithmetic (see RM in
            // cpu80486.h).
            if (opsize32_) set_reg32(last_reg_, rm.off); else set_reg16(last_reg_, uint16_t(rm.off));
            c += 1;  // published 1-2; the 2 case is the base+index+disp penalty decode_modrm() already added
            break;
        }
        case 0x8E: {  // MOV sreg, r/m16
            RM rm = decode_modrm();
            int si = last_reg_ & 7;
            // CS is not loadable this way -- changing the code segment needs
            // a control transfer, which is the whole point. Reg fields 6 and
            // 7 name no segment register at all.
            if (si == SEG_CS || si > SEG_GS) raise(EXC_UD);
            load_seg(si, rm_read16(rm));
            c += 3;
            break;
        }
        case 0x8F: {  // POP r/m
            // "If the ESP register is used as a base register for addressing
            // a destination operand in memory, the POP instruction computes
            // the effective address of the operand after it increments the
            // ESP register" (Intel SDM, POP) -- so POP [ESP+n] resolves
            // against the *post*-pop ESP, not the value ESP held when the
            // instruction started. Popping before decoding the ModRM/SIB --
            // rather than decoding it up front, as every other rm_write user
            // does -- is what gets that ordering right; decode_modrm() only
            // ever reads EIP for the encoding bytes, never ESP, so moving it
            // after the pop changes nothing else.
            if (opsize32_) { uint32_t v = pop32(); RM rm = decode_modrm(); rm_write32(rm, v); c += rm.is_mem ? 6 : 1; }
            else            { uint16_t v = pop16(); RM rm = decode_modrm(); rm_write16(rm, v); c += rm.is_mem ? 6 : 1; }
            break;
        }

        case 0x90: c += 1; break;  // NOP (XCHG eAX,eAX)
        case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
            int r = op - 0x90;
            if (opsize32_) { uint32_t t = eax; eax = get_reg32(r); set_reg32(r, t); }
            else { uint16_t t = get_reg16(0); set_reg16(0, get_reg16(r)); set_reg16(r, t); }
            c += 3;
            break;
        }
        case 0x98:  // CBW / CWDE
            if (opsize32_) eax = uint32_t(int32_t(int16_t(eax)));
            else set_reg16(0, uint16_t(int16_t(int8_t(eax & 0xFF))));
            c += 3;
            break;
        case 0x99:  // CWD / CDQ
            if (opsize32_) edx = (eax & 0x80000000u) ? 0xFFFFFFFFu : 0u;
            else set_reg16(2, (eax & 0x8000) ? 0xFFFFu : 0u);
            c += 3;
            break;
        case 0x9A: {  // CALL far direct (ptr16:16 or ptr16:32)
            uint32_t off = opsize32_ ? fetch32() : uint32_t(fetch16());
            uint16_t seg = fetch16();
            c += far_transfer(seg, off, true);
            break;
        }
        case 0x9B:
            // WAIT/FWAIT. With CR0.MP and CR0.TS both set it reports #NM so
            // a task-switching handler can save the FPU state the *previous*
            // task left behind -- MP exists precisely so WAIT and the ESC
            // opcodes can be trapped separately.
            if ((cr_[0] & CR0_MP) && (cr_[0] & CR0_TS)) raise(EXC_NM);
            c += 1;  // published 1-3
            break;
        case 0x9C:  // PUSHF / PUSHFD
            // PUSHFD is half of the AP-485 486-detection sequence (PUSHFD,
            // POP EAX, flip bit 18, PUSH EAX, POPFD, PUSHFD, POP EAX), so
            // the AC bit has to survive a round trip through here.
            //
            // In V86 it is IOPL-sensitive: "PUSHF, POPF, and IRET are
            // sensitive to IOPL so that the V86 monitor can control changes to
            // the interrupt-enable flag", and "CPL is always three in V86 mode;
            // therefore, if IOPL < 3, these instructions will trigger a
            // general-protection exception" (Intel 80386 PRM, "Additional
            // Sensitive Instructions"). A real fault to the monitor, not
            // POPF's silent masking below, which is a different rule for
            // ordinary protected-mode CPL > 0.
            if (v86_mode() && iopl() != 3) raise_err(EXC_GP, 0);
            if (opsize32_) push32(eflags); else push16(uint16_t(eflags));
            c += 4;
            break;
        case 0x9D:  // POPF / POPFD
            // Real mode has no CPL to gate the IOPL/IF load, so a real 486
            // loads IOPL and NT unconditionally -- and AC too, for the
            // 32-bit form. (Note for integration: ibmpc-at's 286 core
            // deliberately masks IOPL/NT to 0 here because letting them
            // round-trip broke FreeDOS 1.3's installer on that machine,
            // root cause unconfirmed -- see IBM_PCAT_REVIEW.md. This core
            // implements the genuine behavior, which is what every other
            // 486 emulator does; if a FreeDOS oddity ever shows up here,
            // this line is the first thing to bisect.)
            // In protected mode IOPL is writable only at CPL 0 and IF only
            // when CPL <= IOPL; a POPF that tries otherwise silently keeps
            // the old value rather than faulting (Intel 80486 PRM, POPF).
            // And in V86 it faults outright instead, at IOPL < 3 -- see PUSHF
            // above for the citation.
            if (v86_mode() && iopl() != 3) raise_err(EXC_GP, 0);
            {
                // VM (like RF) is never affected by POPF/POPFD, in any mode --
                // "the VM and RF flags... are not affected by the POPF/POPFD
                // instructions" (Intel 80486 PRM, "POPF/POPFD"); only IRETD
                // from CPL 0, or a task switch, can change it. kPopfMask and
                // kPopfdMask already keep the *popped* value from supplying a
                // VM bit, but without also keeping it here, the OR below would
                // silently zero whatever VM already was -- dropping a running
                // V86 task out of virtual-8086 mode on its own POPF/POPFD.
                uint32_t keep = FLAG_VM;
                if (protected_mode()) {
                    if (cpl() > 0) keep |= FLAG_IOPL;
                    if (cpl() > iopl()) keep |= FLAG_IF;
                }
                if (opsize32_) {
                    uint32_t v = pop32();
                    eflags = ((v & kPopfdMask & ~keep) | (eflags & keep)) | FLAG_R1;
                } else {
                    uint32_t v = pop16();
                    eflags = (eflags & 0xFFFF0000u) |
                             ((v & kPopfMask & ~keep) | (eflags & keep & 0xFFFFu)) | FLAG_R1;
                }
            }
            c += 9;
            break;
        case 0x9E: eflags = (eflags & ~uint32_t(0xFF)) | (get_reg8(4) & kSahfMask) | FLAG_R1; c += 2; break;  // SAHF
        case 0x9F: set_reg8(4, uint8_t(eflags & 0xFF)); c += 3; break;                                       // LAHF

        // --- MOV acc,[disp] / [disp],acc (published 486: 1 either way) ---
        case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
            int seg = (seg_override_ >= 0) ? seg_override_ : int(SEG_DS);
            // The displacement's own width follows the *address* size, and
            // a 32-bit one is a full 32-bit offset -- not truncated to the
            // real-mode window, for the same reason decode_modrm() does not
            // truncate (PC486_REVIEW.md §5.4).
            uint32_t off = addrsize32_ ? fetch32() : uint32_t(fetch16());
            if (op == 0xA0) set_reg8(0, read8(seg, off));
            else if (op == 0xA1) { if (opsize32_) eax = read32(seg, off); else set_reg16(0, read16(seg, off)); }
            else if (op == 0xA2) write8(seg, off, get_reg8(0));
            else { if (opsize32_) write32(seg, off, eax); else write16(seg, off, get_reg16(0)); }
            c += 1;
            break;
        }
        case 0xA4: case 0xA5: case 0xA6: case 0xA7: c += string_op(op); break;
        case 0xA8: { uint8_t imm = fetch8(); and8(get_reg8(0), imm); c += 1; break; }
        case 0xA9: if (opsize32_) and32(eax, fetch32()); else and16(get_reg16(0), fetch16()); c += 1; break;
        case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF: c += string_op(op); break;

        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7: set_reg8(op - 0xB0, fetch8()); c += 1; break;
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            if (opsize32_) set_reg32(op - 0xB8, fetch32()); else set_reg16(op - 0xB8, fetch16());
            c += 1;
            break;

        case 0xC0: case 0xC1: c += grp2_shift(op); break;
        case 0xC2: { uint16_t n = fetch16(); set_ip(opsize32_ ? pop32() : uint32_t(pop16())); add_sp(int32_t(n)); c += 5; break; }  // RET imm16
        case 0xC3: set_ip(opsize32_ ? pop32() : uint32_t(pop16())); c += 5; break;                                                 // RET
        case 0xC4: case 0xC5: {  // LES / LDS r16/32, m16:16 or m16:32
            RM rm = decode_modrm(); int r = last_reg_;
            uint32_t off;
            uint16_t seg;
            if (opsize32_) { off = read32(rm.seg, rm.off); seg = read16(rm.seg, seg_off(rm.off, 4)); }
            else { off = read16(rm.seg, rm.off); seg = read16(rm.seg, seg_off(rm.off, 2)); }
            load_seg((op == 0xC4) ? int(SEG_ES) : int(SEG_DS), seg);
            if (opsize32_) set_reg32(r, off); else set_reg16(r, uint16_t(off));
            c += 6;
            break;
        }
        case 0xC6: { RM rm = decode_modrm(); uint8_t imm = fetch8(); rm_write8(rm, imm); c += 1; break; }
        case 0xC7: { RM rm = decode_modrm(); if (opsize32_) rm_write32(rm, fetch32()); else rm_write16(rm, fetch16()); c += 1; break; }
        case 0xC8: {
            // Published 486 ENTER: 14 at nesting level 0, 17 at level 1,
            // and 17+3i for a higher level i.
            int lex = enter();
            c += (lex == 0) ? 14 : (lex == 1) ? 17 : (17 + 3 * lex);
            break;
        }
        case 0xC9: leave(); c += 5; break;
        case 0xCA: { uint16_t n = fetch16(); c += far_return(n, false); break; }  // RETF imm16
        case 0xCB: c += far_return(0, false); break;                              // RETF
        case 0xCC: do_interrupt(3, true); c += 26; break;
        case 0xCD: {  // INT imm8
            uint8_t n = fetch8();
            // The fourth IOPL-sensitive instruction in V86: "INT n is
            // sensitive so that the V86 monitor can intercept calls to the
            // 8086 OS" (Intel 80386 PRM, "Emulating 8086 Operating System
            // Calls"), so at IOPL < 3 the monitor gets a #GP instead of the
            // vector. INT3 and INTO are exceptions rather than software
            // interrupts and are not in that list.
            if (v86_mode() && iopl() != 3) raise_err(EXC_GP, 0);
            do_interrupt(n, true);
            c += 30;
            break;
        }
        case 0xCE: if (flag(FLAG_OF)) { do_interrupt(4, true); c += 28; } else c += 3; break;  // INTO: published 3/28
        case 0xCF: c += far_return(0, true); break;  // IRET / IRETD

        case 0xD0: case 0xD1: case 0xD2: case 0xD3: c += grp2_shift(op); break;
        case 0xD4: aam(); c += 15; break;
        case 0xD5: aad(); c += 14; break;
        case 0xD7: {  // XLAT
            int seg = (seg_override_ >= 0) ? seg_override_ : int(SEG_DS);
            // XLAT indexes a table at (E)BX by AL; the 0x67 prefix selects
            // EBX over BX as the table base.
            uint32_t tbl = addrsize32_ ? ebx : uint32_t(get_reg16(3));
            set_reg8(0, read8(seg, addrsize32_ ? (tbl + get_reg8(0)) : uint32_t(uint16_t(tbl + get_reg8(0)))));
            c += 4;
            break;
        }
        case 0xD8: case 0xD9: case 0xDA: case 0xDB:
        case 0xDC: case 0xDD: case 0xDE: case 0xDF:
            c += esc_op(op);  // x87 ESC space -- the on-die FPU
            break;

        case 0xE0: case 0xE1: case 0xE2: case 0xE3: c += loop_group(op); break;

        // --- Port I/O. Published 486: IN 14, OUT 16 -- both far slower
        // --- than the 286's 5/3, and asymmetric the other way round.
        case 0xE4: { uint8_t p = fetch8(); check_io_permission(p, 1); set_reg8(0, bus_in(p)); c += 14; break; }
        case 0xE5: { uint8_t p = fetch8(); check_io_permission(p, 2); set_reg16(0, bus_in16(p)); c += 14; break; }
        case 0xE6: { uint8_t p = fetch8(); check_io_permission(p, 1); bus_out(p, get_reg8(0)); c += 16; break; }
        case 0xE7: { uint8_t p = fetch8(); check_io_permission(p, 2); bus_out16(p, get_reg16(0)); c += 16; break; }

        case 0xE8: {  // CALL near rel
            if (opsize32_) { int32_t rel = int32_t(fetch32()); push32(eip); set_ip(uint32_t(eip + uint32_t(rel))); }
            else { int16_t rel = int16_t(fetch16()); push16(uint16_t(eip)); set_ip(uint32_t(eip + uint32_t(int32_t(rel)))); }
            c += 3;
            break;
        }
        case 0xE9: {  // JMP near rel
            if (opsize32_) { int32_t rel = int32_t(fetch32()); set_ip(uint32_t(eip + uint32_t(rel))); }
            else { int16_t rel = int16_t(fetch16()); set_ip(uint32_t(eip + uint32_t(int32_t(rel)))); }
            c += 3;
            break;
        }
        case 0xEA: {  // JMP far direct (ptr16:16 or ptr16:32)
            uint32_t off = opsize32_ ? fetch32() : uint32_t(fetch16());
            uint16_t seg = fetch16();
            c += far_transfer(seg, off, false);
            break;
        }
        case 0xEB: { int8_t rel = int8_t(fetch8()); set_ip(uint32_t(eip + uint32_t(int32_t(rel)))); c += 3; break; }  // JMP short
        case 0xEC: check_io_permission(uint16_t(edx), 1); set_reg8(0, bus_in(uint16_t(edx))); c += 14; break;
        case 0xED: check_io_permission(uint16_t(edx), 2); set_reg16(0, bus_in16(uint16_t(edx))); c += 14; break;
        case 0xEE: check_io_permission(uint16_t(edx), 1); bus_out(uint16_t(edx), get_reg8(0)); c += 16; break;
        case 0xEF: check_io_permission(uint16_t(edx), 2); bus_out16(uint16_t(edx), get_reg16(0)); c += 16; break;

        case 0xF1: c += 1; break;  // ICEBP/INT1 -- in-circuit-emulator breakpoint; no debug support here, documented no-op
        case 0xF4:
            // HLT stops the CPU until an interrupt arrives, so only ring 0
            // may issue it -- a user program halting the machine would be
            // a denial of service.
            if (protected_mode() && cpl() != 0) raise_err(EXC_GP, 0);
            halted = true;
            c += 4;
            break;
        case 0xF5: set_flag(FLAG_CF, !flag(FLAG_CF)); c += 2; break;  // CMC
        case 0xF6: case 0xF7: c += grp3_unary(op); break;
        case 0xF8: set_flag(FLAG_CF, false); c += 2; break;
        case 0xF9: set_flag(FLAG_CF, true); c += 2; break;
        // CLI/STI are I/O-privileged: masking interrupts is only permitted
        // when CPL <= IOPL (Intel 80486 PRM, CLI/STI). Published 5 on the
        // 486, up from the 286's 3.
        case 0xFA:
            if (protected_mode() && cpl() > iopl()) raise_err(EXC_GP, 0);
            set_flag(FLAG_IF, false);
            c += 5;
            break;
        case 0xFB:
            if (protected_mode() && cpl() > iopl()) raise_err(EXC_GP, 0);
            set_flag(FLAG_IF, true);
            c += 5;
            break;
        case 0xFC: set_flag(FLAG_DF, false); c += 2; break;
        case 0xFD: set_flag(FLAG_DF, true); c += 2; break;
        case 0xFE: case 0xFF: c += grp5(op); break;

        default:
            if (on_unimplemented) on_unimplemented(cs, instr_start_eip_, op);
            c += 1;  // unrecognized opcode -- see PC486_REVIEW.md's coverage notes
            break;
    }

    c += extra_cycles_;
    cycles += c;
    return c;
}

}  // namespace cpu80486
