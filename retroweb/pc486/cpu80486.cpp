// Intel 80486 DX2-66. Intel 80486 PRM; AP-485 for CPUID/AC detection.
// Cycles: 486 column of Quantasm's 80x86 timing table (i486 PRM appendix).
// base+index+disp costs +1. MUL/IMUL, BSF/BSR, RCL/RCR, CMPXCHG mem are
// fitted to their published ranges.
// PUSH SP pushes the pre-decrement value (286+, unlike the 8086).
#include "cpu80486.h"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace cpu80486 {

namespace {

// Writable-bit masks for POPF/POPFD (Intel 80486 PRM). IOPL and NT are
// storage-only in real mode; RF and VM are untouched by POPF.
constexpr uint32_t kPopfMask  = 0x00007FD5u;  // 16-bit POPF: CF PF AF ZF SF TF IF DF OF IOPL NT
// POPFD adds AC (18) and ID (21); ID is writable because this part has CPUID
// (AP-485).
constexpr uint32_t kPopfdMask = 0x00247FD5u;
constexpr uint32_t kSahfMask  = 0x000000D5u;  // SAHF loads only the low byte's defined flags
// Family 4, model 3, stepping 5: see Cpu::reset().
constexpr uint32_t kComponentId = 0x00000435u;
// IRETD additionally restores RF and VM from the stack image.
constexpr uint32_t kIretdMask = 0x00277FD5u;

// Remaining access-byte decoders; acc_code() and friends are in the header.
inline bool acc_present(uint8_t a)     { return (a & 0x80) != 0; }
inline int  acc_dpl(uint8_t a)         { return (a >> 5) & 3; }
inline bool acc_system(uint8_t a)      { return (a & 0x10) == 0; }
inline bool acc_conforming(uint8_t a)  { return acc_code(a) && (a & 0x04) != 0; }
inline int  sys_type(uint8_t a)        { return a & 0x0F; }

// System descriptor and gate type codes (Intel 80486 PRM).
enum SysType {
    SYS_TSS16_AVAIL = 1, SYS_LDT = 2, SYS_TSS16_BUSY = 3, SYS_CALL_GATE16 = 4,
    SYS_TASK_GATE = 5, SYS_INT_GATE16 = 6, SYS_TRAP_GATE16 = 7,
    SYS_TSS32_AVAIL = 9, SYS_TSS32_BUSY = 11, SYS_CALL_GATE32 = 12,
    SYS_INT_GATE32 = 14, SYS_TRAP_GATE32 = 15,
};

constexpr uint32_t kPtePresent  = 1u << 0;
constexpr uint32_t kPteWritable = 1u << 1;
constexpr uint32_t kPteUser     = 1u << 2;
constexpr uint32_t kPteAccessed = 1u << 5;
constexpr uint32_t kPteDirty    = 1u << 6;
constexpr uint32_t kPtePcd      = 1u << 4;

}  // namespace

void Cpu::init_state() {
    for (int i = 0; i < 8; ++i) {
        sd_[i] = SegDesc{};
        dr_[i] = 0;
    }
    // DR6 and DR7 read their reserved bits as ones from RESET (Intel486
    // Microprocessor Data Book, register state after RESET).
    dr_[6] = 0xFFFF0FF0u;
    dr_[7] = 0x00000400u;
    dbg_exec_ = dbg_data_ = dbg_pending_ = 0;
    ac_skip_ = false;
    for (int i = 0; i < 4; ++i) cr_[i] = 0;
    // ET is hardwired to 1 (FPU on-die). CD and NW come out of RESET set until
    // the BIOS clears them (Intel486 Data Book, register state after RESET).
    cr_[0] = CR0_CD | CR0_NW | CR0_ET;
    access_hooks_ = 0;
    gdtr_ = DescTableReg{0, 0};
    // RESET leaves IDTR base 0, limit 03FFh: the 8086 IVT (Intel 80486 PRM,
    // "Processor Initialization").
    idtr_ = DescTableReg{0, 0x03FF};
    ldtr_ = DescTableReg{0, 0};
    tr_ = DescTableReg{0, 0};
    ldt_sel_ = tr_sel_ = 0;
    tr_access_ = 0;
    cpl_ = 0;
    tlb_flush();
    page_map_flush();
    fpu_init();
    // CS is a code segment, so a write through a CS: override faults once
    // protection is on.
    sd_[SEG_CS].access = 0x9B;   // present, DPL 0, code, readable, accessed
    instr_start_ss_desc_ = sd_[SEG_SS];
}

void Cpu::reset() {
    eax = ebx = ecx = edx = esp = ebp = esi = edi = 0;
    ds = es = ss = fs = gs = 0;
    // CS's hidden base is FFFF0000h until the first far jump, so the first
    // fetch is at FFFFFFF0h, aliased to the BIOS ROM (Intel486 Data Book,
    // "RESET").
    cs = 0xF000;
    eip = 0xFFF0;
    eflags = FLAG_R1;
    halted = false;
    fault_pending_ = false;
    pending_fault_ = {};
    fault_jmp_set_ = false;
    shadow_ = false;
    shutdown_ = false;
    rep_resume_ = rep_resumed_ = false;
    cycles = 0;
    stall_ = 0;
    if (timing) timing->reset();   // RESET empties the L1; the clock restarts
    for (auto &snap : agi_snap_) for (auto &r : snap) r = 0;
    split_ = false;
    pcd_any_ = false;
    pcd_frames_.clear();
    seg_override_ = -1;
    rep_ = REP_NONE;
    opsize32_ = false;
    addrsize32_ = false;
    extra_cycles_ = 0;
    init_state();
    // EDX holds the component ID after RESET: family 4, model 3, stepping 5
    // (SL-Enhanced aB0/aC0, S-spec SX807/SX911, has CPUID; the B1 step, 0433h,
    // does not).
    edx = kComponentId;
    refresh_real_bases();
    sd_[SEG_CS].base = 0xFFFF0000u;
}

// --- faults ---------------------------------------------------------------

void Cpu::raise(int vector) {
    pending_fault_ = Fault{vector, 0, false};
#ifdef __EMSCRIPTEN__
    // Wasm C++ EH soft-locks under a V86 #GP storm (JEMMEX / EMMQXXX0); longjmp
    // matches Bochs.
    if (fault_jmp_set_) longjmp(fault_jmp_, 1);
#endif
    throw pending_fault_;
}
void Cpu::raise_err(int vector, uint32_t error) {
    pending_fault_ = Fault{vector, error, true};
#ifdef __EMSCRIPTEN__
    if (fault_jmp_set_) longjmp(fault_jmp_, 1);
#endif
    throw pending_fault_;
}
void Cpu::raise_ud(uint32_t at, uint16_t opword) {
    // Intel SDM Vol. 3, "Undefined Opcodes": reserved opcodes raise #UD.
    if (on_unimplemented) on_unimplemented(cs, at, opword);
    raise(EXC_UD);
}
void Cpu::raise_sel(int vector, uint16_t selector) {
    // Selector with RPL cleared (Intel 80486 PRM, "Error Code"). EXT stays
    // clear since callers report a selector the instruction named.
    pending_fault_ = Fault{vector, uint32_t(selector & 0xFFFCu), true};
#ifdef __EMSCRIPTEN__
    if (fault_jmp_set_) longjmp(fault_jmp_, 1);
#endif
    throw pending_fault_;
}

// --- interrupts and exceptions --------------------------------------------

void Cpu::do_interrupt(uint8_t vector, bool software, bool has_error, uint32_t error) {
    halted = false;
    vectored_ = true;
    if (protected_mode()) protected_mode_interrupt(vector, software, has_error, error);
    else real_mode_interrupt(vector);
}

void Cpu::real_mode_interrupt(uint8_t vector) {
    // Real-mode frame is 16-bit FLAGS, CS, IP, as on an 8086. A 0x66-prefixed
    // INT would push a 32-bit frame on real hardware; not implemented.
    // The vector is checked against IDTR (limit 03FFh from RESET), not a fixed
    // 1KB, since LIDT can shrink it.
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
    // Intel 80486 PRM, "Interrupts and Exceptions": the IDT holds 8-byte task,
    // interrupt or trap gates.
    uint32_t idx = uint32_t(vector) * 8;
    // EXT marks a fault not caused by the instruction itself: set for hardware
    // interrupts and exceptions, clear for software INT.
    uint32_t ext = software ? 0u : 1u;
    if (idx + 7 > idtr_.limit) raise_err(EXC_GP, idx + 2 + ext);
    uint32_t glo = lin_read32(idtr_.base + idx, false);
    uint32_t ghi = lin_read32(idtr_.base + idx + 4, false);
    uint8_t gate_access = uint8_t((ghi >> 8) & 0xFF);
    if (!acc_present(gate_access)) raise_err(EXC_NP, idx + 2 + ext);
    if (!acc_system(gate_access)) raise_err(EXC_GP, idx + 2 + ext);
    int gtype = sys_type(gate_access);
    // Gate DPL must be >= CPL for software INT only (Intel 80486 PRM).
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

    // Leaving V86: the gate must be a 32-bit trap or interrupt gate to a
    // nonconforming DPL 0 code segment (Intel 80386 PRM, "Entering and Leaving
    // Virtual 8086 Mode").
    bool from_v86 = v86_mode();
    if (from_v86) {
        if (!gate32 || acc_conforming(ca) || target_dpl != 0) raise_sel(EXC_GP, target_sel);
        // VM clears in the live flags only; old_eflags keeps VM = 1 so the
        // pushed frame lets IRETD restore V86. Done before any push so they use
        // protected-mode segmentation.
        set_flag(FLAG_VM, false);
    }

    if (!acc_conforming(ca) && target_dpl < cpl()) {
        // Inter-privilege: new stack from the TSS slot for the target ring; the
        // old SS:ESP is pushed on it.
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
        // CS loads below; the pushes land on the new stack.
        cs = uint16_t((target_sel & 0xFFFCu) | uint16_t(target_dpl));
        sd_[SEG_CS] = decode_desc(cd);
        sd_[SEG_CS].sel = cs;
        cpl_ = uint8_t(target_dpl);
        // V86 frame: the 8086 segment registers go on the PL 0 stack below
        // SS:ESP, giving GS FS DS ES SS ESP EFLAGS CS EIP from high to low
        // (Intel 80386 PRM, "Entering and Leaving Virtual 8086 Mode"). IRETD
        // pops the same nine dwords.
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
        // Zeroed before the handler runs (Intel 80386 PRM, same section); an
        // 8086 segment value isn't a usable selector.
        for (int si : {SEG_ES, SEG_DS, SEG_FS, SEG_GS}) load_seg(si, 0);
    }

    // Interrupt gates clear IF, trap gates don't. Both clear TF, NT and RF.
    if (is_interrupt_gate) set_flag(FLAG_IF, false);
    set_flag(FLAG_TF, false);
    set_flag(FLAG_NT, false);
    set_flag(FLAG_RF, false);
    set_ip(target_off);
}

int Cpu::interrupt(uint8_t vector) {
    agi_clear();
    // Hardware interrupt: no gate-DPL check. A fault during delivery becomes
    // #GP/#DF here and never escapes to the chipset.
    uint32_t start_eip = eip;
    // An interrupt between REP iterations restarts the instruction from its
    // prefixes after IRET.
    rep_resume_ = false;
    instr_start_esp_ = esp;
    instr_start_ss_ = ss;
    instr_start_ss_desc_ = sd_[SEG_SS];
#ifdef __EMSCRIPTEN__
    fault_jmp_set_ = true;
    if (setjmp(fault_jmp_) == 0) {
        do_interrupt(vector, false);
        fault_jmp_set_ = false;
    } else {
        fault_jmp_set_ = false;
        return deliver_fault(pending_fault_, start_eip);
    }
#else
    try {
        do_interrupt(vector, false);
    } catch (const Fault &f) {
        return deliver_fault(f, start_eip);
    }
#endif
    // Costed like INT3 (26), the best-anchored figure; INT imm8 is 30. Charged
    // only here, since step()'s INT paths charge their own cost.
    cycles += 26;
    int c = 26 + take_stall();
    // A task gate into a TSS with its T bit set traps before the new task's
    // first instruction.
    if (dbg_pending_ & kDbgTask) {
        dbg_pending_ = uint8_t(dbg_pending_ & ~kDbgTask);
        dr_[6] |= 0x8000u;   // DR6.BT
        return c + interrupt(uint8_t(EXC_DB));
    }
    return c;
}

// --- descriptors ----------------------------------------------------------

uint32_t Cpu::desc_base(const RawDesc &d) {
    // Base is split across bytes 2-4 and 7 for 286 compatibility (Intel 80486
    // PRM, "Segment Descriptors").
    return ((d.lo >> 16) & 0x0000FFFFu) | ((d.hi & 0x000000FFu) << 16) | (d.hi & 0xFF000000u);
}

uint32_t Cpu::desc_limit(const RawDesc &d) {
    uint32_t lim = (d.lo & 0x0000FFFFu) | (d.hi & 0x000F0000u);
    // G=1: the limit counts 4KB pages and the low 12 bits are ones, so limit 0
    // is a 4KB segment.
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
    // TI (bit 2) selects the LDT; bits 3-15 are the index, bits 0-1 the RPL
    // (Intel 80486 PRM, "Selectors").
    bool use_ldt = (selector & 0x0004u) != 0;
    uint32_t base  = use_ldt ? ldtr_.base : gdtr_.base;
    uint32_t limit = use_ldt ? ldtr_.limit : gdtr_.limit;
    if (use_ldt && (ldt_sel_ & 0xFFFCu) == 0) raise_sel(fault_vector, selector);
    uint32_t index = selector & 0xFFF8u;
    // A limit is the offset of the last valid byte (N descriptors: N*8-1).
    if (index + 7u > limit) raise_sel(fault_vector, selector);
    RawDesc d;
    d.lo = lin_read32(base + index, false);
    d.hi = lin_read32(base + index + 4, false);
    return d;
}

void Cpu::set_accessed(uint16_t selector) {
    // A successful load sets the descriptor's A bit in the table (Intel 80486
    // PRM, "Accessed Bit").
    bool use_ldt = (selector & 0x0004u) != 0;
    uint32_t base = use_ldt ? ldtr_.base : gdtr_.base;
    uint32_t addr = base + (selector & 0xFFF8u) + 4;
    uint32_t hi = lin_read32(addr, true);
    if (!(hi & 0x00000100u)) lin_write32(addr, hi | 0x00000100u);
}

// --- segment register loading ---------------------------------------------

void Cpu::load_seg_real(int si, uint16_t selector) {
    // Base becomes selector*16; limit and access rights stay, which is how
    // unreal mode works (PC486_REVIEW.md §5.4).
    seg_reg(si) = selector;
    sd_[si].sel = selector;
    sd_[si].base = uint32_t(selector) << 4;
    sd_[si].null = false;
    // A real-mode CS load sets CPL to 0 whatever the low bits, so HimemX can
    // set CR0.PE from CS=0291h and load a DPL-0 selector (PC486_REVIEW.md
    // §6.5). In V86 it sets CPL 3.
    if (si == SEG_CS) cpl_ = v86_mode() ? 3 : 0;
}

void Cpu::refresh_real_bases() {
    // A host or test may assign a public selector field directly. A changed
    // selector is a real-mode segment load, so the base is re-derived; an
    // unchanged one keeps a protected-mode base for unreal mode.
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
        // A null selector is legal for DS/ES/FS/GS and faults only on access;
        // SS can't be null (Intel 80486 PRM, "Null Selector").
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
        // SS must be a writable data segment with RPL and DPL both equal to
        // CPL.
        if (acc_system(a) || !acc_writable(a) || rpl != cpl() || acc_dpl(a) != cpl())
            raise_sel(EXC_GP, selector);
        if (!acc_present(a)) raise_sel(EXC_SS, selector);
    } else {
        if (acc_system(a) || (acc_code(a) && !acc_readable(a))) raise_sel(EXC_GP, selector);
        // Non-conforming: max(CPL, RPL) must be <= DPL.
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
        // Same rule as the walk below, including CR0.WP's supervisor exemption;
        // otherwise a multi-byte supervisor write to a read-only page succeeds
        // on its first byte and faults on its second (PC486_REVIEW.md §6.5).
        bool denied = (user && !(e.rights & 2)) ||
                      (write && !(e.rights & 1) && (user || (cr_[0] & CR0_WP)));
        // A cached entry answers a write only once D is set; otherwise the walk
        // reruns to set it, since software watches that bit.
        if (!denied && (!write || e.dirty)) return e.frame | (linear & 0x00000FFFu);
        if (denied) {
            cr_[2] = linear;
            raise_err(EXC_PF, 1u | (write ? 2u : 0u) | (user ? 4u : 0u));
        }
    }
    // Two-level walk: CR3 -> page directory -> page table (Intel 80486 PRM,
    // "Page Translation").
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
    // U/S and R/W are the AND of both levels.
    bool user_ok  = (pde & kPteUser) && (pte & kPteUser);
    bool writable = (pde & kPteWritable) && (pte & kPteWritable);
    if (user && !user_ok) {
        cr_[2] = linear;
        raise_err(EXC_PF, 1u | (write ? 2u : 0u) | 4u);
    }
    if (write && !writable) {
        // A supervisor write to a read-only page succeeds unless CR0.WP is set;
        // WP is new on the 486.
        if (user || (cr_[0] & CR0_WP)) {
            cr_[2] = linear;
            raise_err(EXC_PF, 1u | 2u | (user ? 4u : 0u));
        }
    }
    uint32_t new_pte = pte | kPteAccessed | (write ? kPteDirty : 0u);
    if (timing) {
        // 13, 21 or 28 bus clocks as neither, one or both entries need an
        // A/D update written back (27302101 12.3.1, rule 10).
        int updates = int(!(pde & kPteAccessed)) + int(new_pte != pte);
        stall_ += uint32_t(pc486::Cache486::kBusRatio * (updates == 0 ? 13 : updates == 1 ? 21 : 28));
        set_pcd(pte & 0xFFFFF000u, ((pde | pte) & kPtePcd) != 0);
    }
    if (!(pde & kPteAccessed)) phys_write32(pde_addr, pde | kPteAccessed);
    if (new_pte != pte) { phys_write32(pte_addr, new_pte); pte = new_pte; }
    e.valid = true;
    e.tag = vpn;
    e.frame = pte & 0xFFFFF000u;
    e.rights = uint8_t((writable ? 1 : 0) | (user_ok ? 2 : 0));
    e.dirty = (pte & kPteDirty) != 0;
    return e.frame | (linear & 0x00000FFFu);
}

// --- debug registers ------------------------------------------------------

// DR4 and DR5 alias DR6 and DR7 on a 486 (there is no CR4.DE to stop it).
uint32_t Cpu::read_dr(int idx) const {
    if (idx == 4 || idx == 5) idx += 2;
    return dr_[idx & 7];
}

void Cpu::write_dr(int idx, uint32_t v) {
    if (idx == 4 || idx == 5) idx += 2;
    if (idx == 6) v = (v & 0x0000E00Fu) | 0xFFFF0FF0u;
    else if (idx == 7) v = (v & 0xFFFF23FFu) | 0x00000400u;
    dr_[idx & 7] = v;
    if (idx == 7) dr7_decode();
}

// RW 00 is an instruction breakpoint, 01 a data write, 11 a data read or write;
// 10 is undefined (Intel 80486 PRM, "Debug Control Register").
void Cpu::dr7_decode() {
    dbg_exec_ = dbg_data_ = 0;
    for (int n = 0; n < 4; ++n) {
        if (!(dr_[7] & (3u << (2 * n)))) continue;
        uint32_t rw = (dr_[7] >> (16 + 4 * n)) & 3u;
        if (rw == 0) dbg_exec_ |= uint8_t(1u << n);
        else if (rw == 1 || rw == 3) dbg_data_ |= uint8_t(1u << n);
    }
    update_access_hooks();
}

void Cpu::access_hooks(uint32_t lin, int size, bool write) {
    ac_check(lin, size);
    if (dbg_data_) dbg_data_match(lin, size, write);
}

uint8_t Cpu::dbg_exec_match(uint32_t lin) const {
    uint8_t hit = 0;
    for (int n = 0; n < 4; ++n)
        if ((dbg_exec_ & (1u << n)) && dr_[n] == lin) hit |= uint8_t(1u << n);
    return hit;
}

// LEN 00 is one byte, 01 two, 11 four, address aligned to that length; any
// overlap matches.
void Cpu::dbg_data_match(uint32_t lin, int size, bool write) {
    uint32_t last = lin + uint32_t(size) - 1u;
    for (int n = 0; n < 4; ++n) {
        if (!(dbg_data_ & (1u << n))) continue;
        uint32_t rw = (dr_[7] >> (16 + 4 * n)) & 3u;
        if (rw == 1 && !write) continue;
        uint32_t len = (dr_[7] >> (18 + 4 * n)) & 3u;
        uint32_t mask = len == 1 ? 1u : len == 3 ? 3u : 0u;
        uint32_t lo = dr_[n] & ~mask;
        if (lo <= last && lin <= lo + mask) dbg_pending_ |= uint8_t(1u << n);
    }
}

// --- memory access --------------------------------------------------------

uint32_t Cpu::seg_linear_slow(int si, uint32_t off, int size, bool write) {
    const SegDesc &s = sd_[si & 7];
    int fault = (si == SEG_SS) ? EXC_SS : EXC_GP;
    uint32_t last = off + uint32_t(size) - 1u;
    // Real mode and V86 check only the cached limit, so a word at FFFFh faults
    // while an unreal-mode 4GB limit reaches past 64KB (Intel 80486 PRM,
    // "Real-Address Mode Exceptions"; PC486_REVIEW.md §44).
    if (real_addressing()) {
        if (last < off || last > s.limit) raise_err(fault, 0);
        return s.base + off;
    }
    if (s.null) raise_err(fault, 0);
    if (write) {
        if (!acc_writable(s.access)) raise_err(fault, 0);
    } else if (!acc_readable(s.access)) {
        raise_err(fault, 0);
    }
    if (last < off) raise_err(fault, 0);
    if (acc_expand_down(s.access)) {
        // Expand-down: valid offsets are limit+1 to the maximum (Intel 80486
        // PRM, "Expand-Down Data Segments").
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
    uint32_t lin = seg_linear(si, off, 1, false);
    if (access_hooks_) access_hooks(lin, 1, false);
    uint32_t p = translate(lin, false, cpl() == 3);
    mem_timing(p, 1, false);
    if (const uint8_t *h = host_ptr(p, false)) return *h;
    return bus_read(p);
}
void Cpu::write8(int si, uint32_t off, uint8_t v) {
    uint32_t lin = seg_linear(si, off, 1, true);
    if (access_hooks_) access_hooks(lin, 1, true);
    uint32_t p = translate(lin, true, cpl() == 3);
    mem_timing(p, 1, true);
    if (uint8_t *h = host_ptr(p, true)) { *h = v; return; }
    bus_write(p, v);
}
bool Cpu::access_phys(int si, uint32_t off, int size, bool write, uint32_t &phys) {
    uint32_t last = off + uint32_t(size) - 1u;
    if (last < off) return false;                                  // wraps past 2^32
    uint32_t lin = seg_linear(si, off, size, write);
    if (access_hooks_) access_hooks(lin, size, write);
    // A misaligned operand costs 3 clocks (27302101 12.3.1, rule 2), once
    // for the whole operand, not again for the halves a page split reads.
    if (timing && (lin & uint32_t(size - 1)) && !split_) stall_ += 3;
    if ((lin & 0xFFFu) > 0x1000u - uint32_t(size)) return false;   // straddles two pages
    phys = translate(lin, write, cpl() == 3);
    mem_timing(phys, size, write);
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
    split_ = true;
    uint32_t v = uint32_t(read16(si, off)) | (uint32_t(read16(si, seg_off(off, 2))) << 16);
    split_ = false;
    return v;
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
    split_ = true;
    uint64_t v = uint64_t(read32(si, off)) | (uint64_t(read32(si, seg_off(off, 4))) << 32);
    split_ = false;
    return v;
}
void Cpu::write16(int si, uint32_t off, uint16_t v) {
    // Both halves are checked and translated before either byte is stored, so a
    // straddling write can't half-complete and #PF stays restartable.
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
    uint32_t q[2] = {p0, p1};
    split_timing(q, 2);
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
    split_timing(q, 4);
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
    split_timing(q, 8);
    for (int i = 0; i < 8; ++i) bus_write(q[i], uint8_t((v >> (8 * i)) & 0xFF));
}

// Times a write assembled byte by byte: one bus write per physically contiguous
// run.
void Cpu::split_timing(const uint32_t *q, int n) {
    if (!timing) return;
    int start = 0;
    for (int i = 1; i <= n; ++i) {
        if (i < n && q[i] == q[i - 1] + 1) continue;
        mem_timing(q[start], i - start, true);
        start = i;
    }
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
// Descriptor tables, page tables and the TSS are addressed linearly, always as
// supervisor accesses.
uint16_t Cpu::lin_read16(uint32_t linear, bool write_access) {
    uint32_t p0 = translate(linear, write_access, false);
    uint32_t p1 = translate(linear + 1, write_access, false);
    if (timing) {
        if (p1 == p0 + 1) mem_timing(p0, 2, false);
        else { mem_timing(p0, 1, false); mem_timing(p1, 1, false); }
    }
    return uint16_t(bus_read(p0) | (uint16_t(bus_read(p1)) << 8));
}
uint32_t Cpu::lin_read32(uint32_t linear, bool write_access) {
    return uint32_t(lin_read16(linear, write_access)) |
           (uint32_t(lin_read16(linear + 2, write_access)) << 16);
}
void Cpu::lin_write16(uint32_t linear, uint16_t v) {
    uint32_t p0 = translate(linear, true, false);
    uint32_t p1 = translate(linear + 1, true, false);
    uint32_t q[2] = {p0, p1};
    split_timing(q, 2);
    bus_write(p0, uint8_t(v & 0xFF));
    bus_write(p1, uint8_t(v >> 8));
}
void Cpu::lin_write32(uint32_t linear, uint32_t v) {
    lin_write16(linear, uint16_t(v & 0xFFFF));
    lin_write16(linear + 2, uint16_t(v >> 16));
}

// Resolves the run of EIPs sharing the current code page so later bytes come
// from a host pointer. Leaves the window empty (full path per fetch) unless it
// is a plain forward run in one page of a readable code segment.
void Cpu::prefetch_fill() {
    prefetch_clear();
    // The window's revalidation reads *bus_.map_epoch unconditionally, so a Bus
    // without both bulk-path members gets no window.
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
    uint32_t phys = translate(lin, false, cpl() == 3);
    uint8_t *h = host_ptr(phys, false);
    if (h == nullptr) return;  // code on the VGA window or in open bus
    pf_base_ = h;
    pf_phys_ = phys;
    if (timing) stall_ += uint32_t(timing->fetch(phys, fills(phys), now()));
    pf_lo_ = eip;
    pf_hi_ = eip + span;
    pf_cs_ = sd_[SEG_CS];
    pf_cr0_ = cr_[0];
    pf_cr3_ = cr_[3];
    pf_cpl_ = cpl_;
    pf_tlb_gen_ = tlb_gen_;
    pf_map_epoch_ = *bus_.map_epoch;
}

// Intel 80486 PRM, "Instruction Format": an instruction longer than 15
// bytes raises #GP(0).
void Cpu::check_length(uint32_t bytes) {
    if (((eip - step_start_eip_) & ip_mask()) + bytes > 15u) raise_err(EXC_GP, 0);
}
void Cpu::arm_length_limit(int prefixes) {
    if (prefixes >= 15) raise_err(EXC_GP, 0);  // the opcode was byte 16
    len_check_ = true;
    uint32_t end = step_start_eip_ + 15u;
    if (pf_hi_ > end) pf_hi_ = pf_lo_ > end ? pf_lo_ : end;
}

// LOCK is legal only on a memory-destination ADD, ADC, AND, BTC, BTR, BTS,
// CMPXCHG, DEC, INC, NEG, NOT, OR, SBB, SUB, XOR, XADD or XCHG; anything
// else raises #UD (Intel 80486 PRM, LOCK). Peeks ahead without consuming.
bool Cpu::lock_allowed(uint8_t op) {
    uint32_t save = eip;
    bool ok = false;
    uint8_t op2 = 0;
    if (op == 0x0F) op2 = fetch8();
    bool candidate = op == 0x0F
        ? (op2 == 0xAB || op2 == 0xB3 || op2 == 0xBB || op2 == 0xBA || op2 == 0xB0 || op2 == 0xB1 ||
           op2 == 0xC0 || op2 == 0xC1)
        : ((op < 0x40 && (op & 7) <= 1 && (op >> 3) != 7) || (op >= 0x80 && op <= 0x83) ||
           op == 0x86 || op == 0x87 || op == 0xF6 || op == 0xF7 || op == 0xFE || op == 0xFF);
    if (candidate) {
        uint8_t modrm = fetch8();
        int reg = (modrm >> 3) & 7;
        if ((modrm >> 6) != 3) {
            if (op == 0x0F) ok = op2 != 0xBA || reg >= 5;
            else if (op >= 0x80 && op <= 0x83) ok = reg != 7;
            else if (op == 0xF6 || op == 0xF7) ok = reg == 2 || reg == 3;
            else if (op == 0xFE || op == 0xFF) ok = reg <= 1;
            else ok = true;
        }
    }
    eip = save;
    return ok;
}

uint8_t Cpu::fetch8_slow() {
    // A byte past CS's limit is a #GP even where 16-bit IP arithmetic would
    // wrap it to 0000h: an instruction can't straddle the limit.
    if (step_start_eip_ + ((eip - step_start_eip_) & ip_mask()) > sd_[SEG_CS].limit) raise_err(EXC_GP, 0);
    if (len_check_) check_length(1);
    prefetch_fill();
    if (len_check_) arm_length_limit(0);
    if (eip >= pf_lo_ && eip < pf_hi_) return fetch8();
    uint8_t v = read8(SEG_CS, eip);
    eip = (eip + 1) & ip_mask();
    return v;
}
uint16_t Cpu::fetch16_slow() {
    if (len_check_) check_length(2);
    prefetch_fill();
    if (len_check_) arm_length_limit(0);
    if (eip >= pf_lo_ && eip < pf_hi_ && pf_hi_ - eip >= 2u) return fetch16();
    uint16_t v = read16(SEG_CS, eip);
    eip = (eip + 2) & ip_mask();
    return v;
}
uint32_t Cpu::fetch32_slow() {
    if (len_check_) check_length(4);
    prefetch_fill();
    if (len_check_) arm_length_limit(0);
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
    // At CPL > IOPL the TSS I/O permission bitmap decides, one bit per port; a
    // set bit denies (Intel 80486 PRM, "I/O Permission Bit Map"). Real mode has
    // no privilege.
    if (!protected_mode()) return;
    // V86 ignores IOPL for I/O; only the permission bitmap applies (Intel 80386
    // PRM, "Virtual I/O").
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
// Published costs (Quantasm 486 column): JMP far 17 real / 19 protected, CALL
// far 18 / 20, RET far 13 (17 across privilege), IRET 15 / 36. The call-gate
// and task-switch figures below are single labelled values covering every
// sub-case.
namespace {
constexpr int kCallGateCost   = 35;
constexpr int kJmpGateCost    = 32;
// One figure for every task-switch path (JMP, CALL, task gate, IRET); not
// separately cited.
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
        // Conforming: entered from any less privileged level, CPL kept.
        // Non-conforming: exact CPL match (Intel 80486 PRM, "Direct Calls or
        // Jumps to Code Segments").
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
        // CS's RPL field is CPL, so a same-privilege transfer keeps the current
        // CPL.
        cs = uint16_t((selector & 0xFFFCu) | uint16_t(cpl()));
        sd_[SEG_CS] = decode_desc(d);
        sd_[SEG_CS].sel = cs;
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
        // The gate's DPL must admit both CPL and the selector's RPL.
        if (acc_dpl(a) < cpl() || acc_dpl(a) < (selector & 3)) raise_sel(EXC_GP, selector);
        if (!acc_present(a)) raise_sel(EXC_NP, selector);
        uint16_t target_sel = uint16_t(d.lo >> 16);
        uint32_t target_off = (d.lo & 0xFFFFu) | (gate32 ? (d.hi & 0xFFFF0000u) : 0u);
        // Low 5 bits of byte 4 are the parameter count.
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
            // A JMP through a call gate can't change privilege; only a CALL
            // leaves a return path.
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
            // Parameters are read off the old stack before the switch.
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
        // Real mode and V86: an 8086 far return. IRET is IOPL-sensitive in V86
        // (CPL is always 3) so the monitor can virtualize IF (Intel 80386 PRM,
        // "Additional Sensitive Instructions").
        if (is_iret && v86_mode() && iopl() != 3) raise_err(EXC_GP, 0);
        // A V86 task can't change VM or IOPL through its 8086 flags image.
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
    // IRET with NT set returns from a task through the TSS back-link (Intel
    // 80486 PRM, "Returning from a Nested Task").
    if (is_iret && flag(FLAG_NT)) {
        uint16_t back_link = read_tss_word(tr_.base, 0);
        task_switch(back_link, TaskLink::Iret, false, 0);
        return kTaskSwitchCost;
    }
    uint32_t new_eip;
    uint16_t new_cs;
    uint32_t new_flags = eflags;
    const int entry_cpl = cpl();  // the level that executed the return
    if (opsize32_) {
        new_eip = pop32();
        new_cs = uint16_t(pop32());
        if (is_iret) new_flags = pop32();
    } else {
        new_eip = pop16();
        new_cs = pop16();
        if (is_iret) new_flags = (eflags & 0xFFFF0000u) | pop16();
    }
    // IRETD from CPL 0 with VM set returns to V86 (Intel 80386 PRM, "Entering
    // and Leaving Virtual 8086 Mode"). Separate path since new_cs is an 8086
    // segment value, not a selector. Pops ESP, SS, ES, DS, FS, GS, mirroring
    // protected_mode_interrupt()'s frame.
    if (is_iret && opsize32_ && (new_flags & FLAG_VM) && entry_cpl == 0) {
        uint32_t new_esp = pop32();
        uint16_t n_ss = uint16_t(pop32());
        uint16_t n_es = uint16_t(pop32()), n_ds = uint16_t(pop32());
        uint16_t n_fs = uint16_t(pop32()), n_gs = uint16_t(pop32());
        // VM goes live first so each load takes the 8086 path and CS lands at
        // CPL 3.
        eflags = (new_flags & kIretdMask) | FLAG_R1;
        const int order[6] = {SEG_CS, SEG_SS, SEG_ES, SEG_DS, SEG_FS, SEG_GS};
        const uint16_t sel[6] = {new_cs, n_ss, n_es, n_ds, n_fs, n_gs};
        for (int i = 0; i < 6; ++i) {
            load_seg_real(order[i], sel[i]);
            SegDesc &s = sd_[order[i]];
            // load_seg_real() keeps the cached limit and D/B (unreal mode); a
            // V86 task must not inherit them, so force 16-bit 64KB segments at
            // privilege 3.
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
        // Outward: the caller's SS:ESP sits above the gate's copied parameters.
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
        cpl_ = uint8_t(rpl);  // before the sweep below, which reads cpl()
        ss = new_ss;
        sd_[SEG_SS] = decode_desc(sdsc);
        sd_[SEG_SS].sel = new_ss;
        esp = new_esp;
        set_ip(new_eip);
        // Null any segment register the inner level could reach but this one
        // can't (Intel 80486 PRM, "Returning from a Procedure").
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
        // IOPL is writable only at CPL 0 and IF only when CPL <= IOPL;
        // otherwise IRET keeps the old values (Intel 80486 PRM, "IRET"). VM
        // follows IOPL. Both test the CPL that executed the IRET, as Bochs
        // iret_protected does with prev_cpl.
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
    // Intel 80486 PRM, "Task Switching": save the outgoing state, update busy
    // bits, load the incoming TSS, then reload segments with full checks.
    if (tss_selector & 0x0004u) raise_sel(EXC_GP, tss_selector);  // a TSS lives in the GDT
    if ((tss_selector & 0xFFFCu) == 0) raise_sel(EXC_GP, tss_selector);
    RawDesc nd = read_desc(tss_selector, EXC_GP);
    uint8_t na = uint8_t((nd.hi >> 8) & 0xFF);
    if (!acc_system(na)) raise_sel(EXC_GP, tss_selector);
    int nt = sys_type(na);
    bool new_is32;
    bool task_trap = false;
    if (link == TaskLink::Iret) {
        // The task being returned to is still marked busy.
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
    // Minimum TSS limit: 67h for 32-bit, 2Bh for 16-bit (Intel 80486 PRM, #TS).
    if (new_limit < (new_is32 ? 0x67u : 0x2Bu)) raise_sel(EXC_TS, tss_selector);
    // A TSS with VM set in its saved EFLAGS needs V86 entry through a task
    // gate, which is not implemented: the CS load below would read an 8086
    // segment value as a selector. Fails before anything is committed. Period
    // V86 managers use IRETD instead.
    if (new_is32 && (read_tss_dword(new_base, 36) & FLAG_VM)) raise_sel(EXC_TS, tss_selector);

    // 1. Save the outgoing state. LTR is what puts the CPU in a task, so TR
    // must be valid.
    if (tr_access_ == 0) raise_sel(EXC_TS, tr_sel_);
    bool old_is32 = (sys_type(tr_access_) == SYS_TSS32_AVAIL || sys_type(tr_access_) == SYS_TSS32_BUSY);
    uint32_t ob = tr_.base;
    // An IRET-driven return clears the saved NT, unwinding the nesting.
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

    // 2. Busy bits and back link. JMP clears the outgoing busy bit; CALL nests
    // (outgoing stays busy, back link and NT recorded); IRET unwinds.
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

    // 3. TR points at the incoming task; CR0.TS makes its first FPU op trap #NM
    // so the handler can swap FPU state.
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
        // CR3 is per-task, reloaded only with paging on.
        if (paging_enabled()) { cr_[3] = new_cr3; tlb_flush(); }
        esp = new_esp;
        // The T bit traps after the switch with DR6.BT set (Intel 80386 PRM,
        // "Debug Exceptions").
        task_trap = (read_tss_word(new_base, 100) & 1u) != 0;
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

    // 5. LDTR first: the incoming selectors may be LDT selectors.
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

    // 6. CS before the data segments; CPL comes from CS.
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

    // 7. An exception through a task gate pushes its error code on the new
    // task's stack.
    if (has_error) {
        if (new_is32) push32(error); else push16(uint16_t(error));
    }
    if (task_trap) dbg_pending_ |= kDbgTask;
}

// --- register file ----------------------------------------------------

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
        // 32-bit addressing (Intel 80486 PRM, ModR/M and SIB tables). The 0x67
        // prefix selects the form in any mode, real mode included.
        int base_reg = -1, index_reg = -1, scale = 0;
        int disp_size = 0;  // 0, 1 or 4 bytes
        if (rm == 4) {
            uint8_t sib = fetch8();
            scale = (sib >> 6) & 3;
            int idx = (sib >> 3) & 7;
            int b = sib & 7;
            // index == 4 means no index; ESP can't be an index.
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

        if (base_reg >= 0) {
            ea += get_reg32(base_reg);
            has_base = true;
            if (agi(base_reg)) extra_cycles_ += 1;
        }
        if (index_reg >= 0) { ea += get_reg32(index_reg) << scale; has_index = true; }
        ea += disp;
        has_disp = (disp_size != 0);
        // ESP or EBP base defaults to SS (Intel 80486 PRM, "Default Segment
        // Attribute").
        default_ss = has_base && (base_reg == 4 || base_reg == 5);
    } else {
        // 16-bit addressing (Intel 8086 manual table 2-19); intermediate sums
        // wrap mod 64KB.
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
        static constexpr uint8_t kBase16[8] = {3, 3, 5, 5, 6, 7, 5, 3};
        if (has_base && agi(kBase16[rm])) extra_cycles_ += 1;
        if (!disp_only) {
            if (mod == 1) { a16 = uint16_t(a16 + int16_t(int8_t(fetch8()))); has_disp = true; }
            else if (mod == 2) { a16 = uint16_t(a16 + fetch16()); has_disp = true; }
        }
        ea = a16;
    }

    // Quantasm legend: base+index+disp costs +1 on 286-486, all other forms
    // nothing.
    if (has_base && has_index && has_disp) extra_cycles_ += 1;

    int seg = default_ss ? int(SEG_SS) : int(SEG_DS);
    if (seg_override_ >= 0) seg = seg_override_;
    out.seg = uint8_t(seg);
    out.off = ea;
    out.disp = has_disp;
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
    // Published costs (Quantasm 486 column): reg,reg / reg,imm / acc,imm 1;
    // reg,mem 2; mem,reg / mem,imm 3. CMP only reads memory, so it costs 2 in
    // every memory form.
    if (!any_mem) return 1;
    if (alu == 7) return 2;         // CMP: no write-back
    return dst_is_mem ? 3 : 2;
}

// --- data-dependent cost models -------------------------------------------

int Cpu::mul_cost(uint32_t multiplier, int width_bits) {
    // Published MUL/IMUL: r/m8 13-18, r/m16 13-26, r/m32 13-42, same for
    // register and memory. Early-out multiply, so the cost tracks the
    // multiplier's top bit. Modelled as floor 13 plus one clock per significant
    // bit past the third, reaching 18/26/42 at the ceiling for widths 8/16/32.
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
    // Published BSF 6-42 (register) / 7-43 (memory), BSR 6-103 / 7-104. One
    // clock per bit examined on top of the floor, inside BSF's range for every
    // width.
    return (is_mem ? 7 : 6) + bits_examined;
}

int Cpu::rotate_carry_cost(int count, bool is_mem) {
    // Published RCL/RCR by CL or imm8: 8-30 (register), 9-31 (memory);
    // rotate-through-carry is iterative. Linear in count from the floor at
    // count 1, saturating at the ceiling. The by-1 encodings D0/D1 are a
    // separate case (3/4).
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
    // SHLD/SHRD r/m, r, count (386+): fills the vacated bits from the source
    // register. Count 0 leaves flags alone; counts are masked mod 32.
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
        // Intel calls a 16-bit double shift with count above 15 undefined, but
        // the 486 shifts the 32-bit dest:src pair and keeps the named half, and
        // Borland's runtime shift helpers in CWSDPMI (cl=24) depend on that
        // (PC486_REVIEW.md §9).
        // 64-bit pair so the shift can't overflow; narrowed to 32 so bits
        // shifted off the top are lost.
        uint16_t dst = rm_read16(rm), src = get_reg16(src_reg);
        uint64_t pair = right ? ((uint64_t(src) << 16) | dst) : ((uint64_t(dst) << 16) | src);
        uint16_t res;
        bool cf;
        if (!right) {
            // CF is dest's bit 16-count for documented counts (1-16).
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
// From the 286 on, AAA/AAS adjust all of AX so a carry or borrow out of AL
// reaches AH; the 8086 adjusted AL alone (Intel SDM AAA/AAS).
void Cpu::aaa() {
    uint16_t ax = get_reg16(0);
    if (((ax & 0x0F) > 9) || flag(FLAG_AF)) {
        ax = uint16_t(ax + 0x106);
        set_flag(FLAG_AF, true);
        set_flag(FLAG_CF, true);
    } else {
        set_flag(FLAG_AF, false);
        set_flag(FLAG_CF, false);
    }
    set_reg16(0, uint16_t(ax & 0xFF0F));
}
void Cpu::aas() {
    uint16_t ax = get_reg16(0);
    if (((ax & 0x0F) > 9) || flag(FLAG_AF)) {
        ax = uint16_t(ax - 6);
        ax = uint16_t(ax - 0x100);
        set_flag(FLAG_AF, true);
        set_flag(FLAG_CF, true);
    } else {
        set_flag(FLAG_AF, false);
        set_flag(FLAG_CF, false);
    }
    set_reg16(0, uint16_t(ax & 0xFF0F));
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
        pop32();  // saved SP slot is discarded
        ebx = pop32(); edx = pop32(); ecx = pop32(); eax = pop32();
    } else {
        set_reg16(7, pop16()); set_reg16(6, pop16()); set_reg16(5, pop16());
        pop16();
        set_reg16(3, pop16()); set_reg16(2, pop16()); set_reg16(1, pop16()); set_reg16(0, pop16());
    }
}
void Cpu::bound() {
    RM rm = decode_modrm();
    if (!rm.is_mem) raise_ud(instr_start_eip_, 0x62);
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
    // The target wraps in the segment's own width.
    if (taken) set_ip(uint32_t(eip + uint32_t(int32_t(rel))));
}
int Cpu::loop_group(uint8_t op) {
    int8_t rel = int8_t(fetch8());
    // 0x67 selects CX vs ECX as the loop counter.
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
    // Published costs (Quantasm 486 column, no jump/jump): LOOP 6/7, LOOPE and
    // LOOPNE 6/9, JCXZ 5/8. Charged per op and outcome since counting loops are
    // what speed benchmarks run.
    if (op == 0xE3) return take ? 8 : 5;
    if (op == 0xE2) return take ? 7 : 6;
    return take ? 9 : 6;
}

// --- string instructions ---------------------------------------------------

int Cpu::string_op(uint8_t op) {
    bool wide = (op & 1) != 0;
    bool is_rep = (rep_ != REP_NONE);
    // The source honors an override; the destination is always ES.
    int src_seg = (seg_override_ >= 0) ? seg_override_ : int(SEG_DS);
    bool is_cmp_scan = (op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF);
    // 0x66 widens word string ops to dword; 0x67 selects ESI/EDI/ECX. Pointers
    // can pass 0FFFFh in unreal mode (PC486_REVIEW.md §5.4): HimemX copies XMS
    // blocks with F3 67 66 A5 (REP MOVSD, addr32).
    bool dword = wide && opsize32_;
    bool addr32 = addrsize32_;
    auto counter = [&]() -> uint32_t { return addr32 ? ecx : uint32_t(uint16_t(ecx)); };
    auto dec_counter = [&]() { if (addr32) ecx = ecx - 1; else set_reg16(1, uint16_t(ecx - 1)); };
    int iterations = 0;  // executed; REPE/REPNE can stop short of CX
    // Under TF a REP runs one iteration per step, EIP staying on the
    // instruction until the last (Intel 80486 PRM, "Single-Step Trap").
    const bool step_each = is_rep && flag(FLAG_TF);
    const int per_iteration = (op == 0xA4 || op == 0xA5) ? 3 : (op == 0xA6 || op == 0xA7) ? 7
                            : (op == 0xAA || op == 0xAB) ? 4 : 5;
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
        // A watchpoint hit ends the REP after this iteration, with EIP still on
        // the instruction.
        if (step_each || (dbg_pending_ & 0x0F)) {
            if (counter() != 0) eip = step_start_eip_;
            break;
        }
        if (rep_yield_cycles != 0 && counter() != 0 &&
            uint32_t(iterations * per_iteration) >= rep_yield_cycles) {
            eip = step_start_eip_;
            rep_resume_ = true;
            break;
        }
    } while (is_rep && counter() != 0);
    // A continuation pays only for its own iterations; the first step paid
    // setup.
    if (is_rep && rep_resumed_) return iterations * per_iteration;
    // Published string costs (Quantasm 486 column). REP forms are formulas in
    // n, the iterations actually run, with the footnote's "5 if n=0, 13 if n=1"
    // for REP MOVS/STOS and "5 if n=0" for compare/scan forms.
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
            // LODS 5. No REP LODS formula is published since software never
            // REPs it.
            return 5;
        default:               // SCAS 6; REP(N)E SCAS 7+5n
            if (!is_rep) return 6;
            return n == 0 ? 5 : 7 + 5 * n;
    }
}
int Cpu::io_string_op(uint8_t op) {
    bool wide = (op & 1) != 0;
    bool is_rep = (rep_ != REP_NONE);  // plain REP only
    int src_seg = (seg_override_ >= 0) ? seg_override_ : int(SEG_DS);
    // As for the other string ops: 0x66 makes INSD/OUTSD, 0x67 selects
    // ESI/EDI/ECX (Intel 80486 PRM, INS and OUTS).
    bool dword = wide && opsize32_;
    bool addr32 = addrsize32_;
    int size = dword ? 4 : (wide ? 2 : 1);
    auto counter = [&]() -> uint32_t { return addr32 ? ecx : uint32_t(uint16_t(ecx)); };
    int iterations = 0;
    const bool step_each = is_rep && flag(FLAG_TF);
    do {
        if (is_rep && counter() == 0) break;
        // Each transfer takes the IOPL and permission-bitmap check.
        check_io_permission(uint16_t(edx), size);
        int dir = flag(FLAG_DF) ? -size : size;
        if (op == 0x6C || op == 0x6D) {  // INS: port DX -> ES:DI
            uint32_t di = addr32 ? edi : uint32_t(uint16_t(edi));
            if (!wide) write8(SEG_ES, di, bus_in(uint16_t(edx)));
            else if (dword) write32(SEG_ES, di, bus_in32(uint16_t(edx)));
            else write16(SEG_ES, di, bus_in16(uint16_t(edx)));
            if (addr32) edi = uint32_t(edi + dir); else set_reg16(7, uint16_t(di + dir));
        } else {                          // OUTS: DS:SI -> port DX
            uint32_t si = addr32 ? esi : uint32_t(uint16_t(esi));
            if (!wide) bus_out(uint16_t(edx), read8(src_seg, si));
            else if (dword) bus_out32(uint16_t(edx), read32(src_seg, si));
            else bus_out16(uint16_t(edx), read16(src_seg, si));
            if (addr32) esi = uint32_t(esi + dir); else set_reg16(6, uint16_t(si + dir));
        }
        ++iterations;
        if (!is_rep) break;
        if (addr32) ecx = ecx - 1; else set_reg16(1, uint16_t(ecx - 1));
        // A watchpoint hit ends the REP after this iteration, with EIP still on
        // the instruction.
        if (step_each || (dbg_pending_ & 0x0F)) {
            if (counter() != 0) eip = step_start_eip_;
            break;
        }
        if (rep_yield_cycles != 0 && counter() != 0 && uint32_t(iterations * 17) >= rep_yield_cycles) {
            eip = step_start_eip_;
            rep_resume_ = true;
            break;
        }
    } while (is_rep && counter() != 0);
    // Published INS and OUTS cost 17 in real mode. No REP formula is published,
    // so one iteration's cost is charged per iteration run, with the same "5 if
    // n=0" as the other string forms.
    if (!is_rep) return 17;
    return iterations == 0 ? 5 : 17 * iterations;
}

// --- opcode groups (reg field of ModR/M selects the operation) -----------

int Cpu::grp1_immed(uint8_t op) {  // 0x80/0x82: r/m8,imm8  0x81: r/m16/32,imm16/32  0x83: r/m16/32,imm8(sx)
    RM rm = decode_modrm();
    disp_imm(rm);
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
    if (by_imm) { count = fetch8(); disp_imm(rm); }
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
    // Published costs (Quantasm 486 column). The barrel shifter makes
    // SHL/SHR/SAR/ROL/ROR count-independent, unlike the 286; only RCL/RCR
    // iterate. A register shift is 2 by imm8 and 3 by 1 or CL.
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
            case 0: case 1: { uint8_t imm = fetch8(); and8(v, imm); disp_imm(rm); break; }  // TEST
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
            case 0: case 1: { uint32_t imm = fetch32(); and32(v, imm); disp_imm(rm); break; }
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
            case 0: case 1: { uint16_t imm = fetch16(); and16(v, imm); disp_imm(rm); break; }
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
    // Published costs (Quantasm 486 column). TEST is 1 reg / 2 mem, NOT/NEG 1 /
    // 3, DIV r/m32 40, IDIV r/m32 43/44. MUL costs the same for memory and
    // register operands; IDIV adds 1 for memory. A flat group cost would
    // undercost the DIV/MUL-heavy loops speed benchmarks use.
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
        if (alu > 1) raise_ud(instr_start_eip_, uint16_t(op));   // FE /2-/7 are reserved
        bool cf = flag(FLAG_CF);
        if (alu == 0) { uint8_t r = add8(rm_read8(rm), 1, false); set_flag(FLAG_CF, cf); rm_write8(rm, r); }
        else { uint8_t r = sub8(rm_read8(rm), 1, false); set_flag(FLAG_CF, cf); rm_write8(rm, r); }
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
            if (!rm.is_mem) raise_ud(instr_start_eip_, uint16_t(op));
            uint32_t off;
            uint16_t seg;
            if (opsize32_) { off = read32(rm.seg, rm.off); seg = read16(rm.seg, seg_off(rm.off, 4)); }
            else { off = read16(rm.seg, rm.off); seg = read16(rm.seg, seg_off(rm.off, 2)); }
            int c = far_transfer(seg, off, true);
            // Published CALL far indirect is 17 in real mode (direct is 18);
            // protected is 20 for both.
            if (!protected_mode()) c = 17;
            return c;
        }
        case 4:    // JMP near indirect
            set_ip(opsize32_ ? rm_read32(rm) : uint32_t(rm_read16(rm)));
            return 5;
        case 5: {  // JMP far indirect (memory only)
            if (!rm.is_mem) raise_ud(instr_start_eip_, uint16_t(op));
            uint32_t off;
            uint16_t seg;
            if (opsize32_) { off = read32(rm.seg, rm.off); seg = read16(rm.seg, seg_off(rm.off, 4)); }
            else { off = read16(rm.seg, rm.off); seg = read16(rm.seg, seg_off(rm.off, 2)); }
            int c = far_transfer(seg, off, false);
            // Published JMP far indirect is 13 in real mode (direct is 17);
            // protected is 18 and 19.
            if (!protected_mode()) c = 13;
            else if (c == 19) c = 18;
            return c;
        }
        case 6:    // PUSH r/m
            if (opsize32_) push32(rm_read32(rm)); else push16(rm_read16(rm));
            return rm.is_mem ? 4 : 1;
        default:   // FF /7
            raise_ud(instr_start_eip_, uint16_t(op));
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
    // Not recognized in real address mode (Intel 80486 PRM): #UD, not a no-op.
    if (!protected_mode()) raise(EXC_UD);
    switch (sub) {
        case 0: rm_write16(rm, ldt_sel_); return rm.is_mem ? 3 : 2;   // SLDT
        case 1: rm_write16(rm, tr_sel_); return rm.is_mem ? 3 : 2;    // STR
        case 2: {  // LLDT r/m16
            if (cpl() != 0) raise_err(EXC_GP, 0);
            uint16_t sel = rm_read16(rm);
            if ((sel & 0xFFFCu) == 0) {
                // A null LDT selector means no LDT; only a later LDT reference
                // faults.
                ldt_sel_ = sel;
                ldtr_.base = 0;
                ldtr_.limit = 0;
                return 11;
            }
            if (sel & 0x0004u) raise_sel(EXC_GP, sel);  // an LDT's descriptor lives in the GDT
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
            // LTR marks the task busy; from here the CPU is executing it, so a
            // task switch can save into it.
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
            // Reports through ZF; never faults on a bad selector.
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
                // Conforming is readable from any level; otherwise the
                // effective privilege must reach it.
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
            ac_check_whole(rm.seg, rm.off, 4);   // GDTR/IDTR image: 4 (Intel 80486 PRM, "Alignment Check")
            write16(rm.seg, rm.off, t.limit);
            write32(rm.seg, seg_off(rm.off, 2), t.base);
            ac_skip_ = false;
            return 10;
        }
        case 2: case 3: {  // LGDT / LIDT m16&32 -- legal in real mode
            if (!rm.is_mem) raise(EXC_UD);
            if (protected_mode() && cpl() != 0) raise_err(EXC_GP, 0);
            uint16_t limit = read16(rm.seg, rm.off);
            uint32_t base = read32(rm.seg, seg_off(rm.off, 2));
            // A 16-bit operand loads only 24 bits of base (286-compatible form,
            // Intel 80486 PRM, LGDT/LIDT), so a real-mode DOS extender's LGDT
            // gets a 24-bit base.
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
            // LMSW writes only CR0's low four bits and cannot clear PE; leaving
            // protected mode needs MOV CR0 (Intel 80486 PRM, LMSW).
            uint32_t nv = (cr_[0] & 0xFFFFFFF0u) | uint32_t(v & 0x0Fu);
            if (cr_[0] & CR0_PE) nv |= CR0_PE;
            write_cr0(nv);
            return 13;
        }
        case 7:  // INVLPG m
            if (!rm.is_mem) raise(EXC_UD);
            if (protected_mode() && cpl() != 0) raise_err(EXC_GP, 0);
            // Takes a linear address, built from the segment base without
            // seg_linear(): INVLPG raises no limit fault.
            tlb_invalidate(seg_base(rm.seg) + rm.off);
            return 12;
        default:
            raise(EXC_UD);   // /5 is undefined on a 486
    }
}

void Cpu::write_cr0(uint32_t v) {
    // ET is hardwired to 1.
    v |= CR0_ET;
    // PG without PE is #GP(0) (Intel 80486 PRM, CR0).
    if ((v & CR0_PG) && !(v & CR0_PE)) raise_err(EXC_GP, 0);
    // NW set with CD clear is the other invalid combination (Intel SDM, MOV CR).
    if ((v & CR0_NW) && !(v & CR0_CD)) raise_err(EXC_GP, 0);
    uint32_t old = cr_[0];
    cr_[0] = v;
    update_access_hooks();
    if ((old ^ v) & (CR0_PG | CR0_WP)) tlb_flush();
    // Neither direction reloads the descriptor caches. That is what makes
    // set-PE-then-far-JMP work and what lets a big limit survive into real mode
    // (unreal mode, PC486_REVIEW.md §5.4).
}

int Cpu::mov_control_reg(uint8_t op2) {
    RM rm = decode_modrm();
    int idx = last_reg_ & 7;
    // No memory encoding: mod != 11 is #UD.
    if (rm.is_mem) raise(EXC_UD);
    int gpr = rm.reg;
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
                cr_[3] = v;
                tlb_flush();
                return 4;
            }
            cr_[idx] = v;
            return 4;
        }
        case 0x21: case 0x23:
            // DR7.GD turns a debug-register access into #DB with DR6.BD set and
            // clears itself (Intel 80486 PRM, "Debug Control Register").
            if (dr_[7] & 0x2000u) {
                dr_[6] |= 0x2000u;
                dr_[7] &= ~0x2000u;
                raise(EXC_DB);
            }
            if (op2 == 0x21) { set_reg32(gpr, read_dr(idx)); return 10; }
            write_dr(idx, get_reg32(gpr));
            return 11;
        case 0x24: set_reg32(gpr, 0); return 4;           // MOV r32, TRn -- no cache/TLB test interface
        default: return 6;                                // MOV TRn, r32
    }
}

// --- the 0x0F escape space -------------------------------------------------

int Cpu::two_byte() {
    uint8_t op2 = fetch8();
    PC486_PERF_BUMP(perf.opcode0f[op2]);
    switch (op2) {
        case 0x00: return grp0f00();   // SLDT/STR/LLDT/LTR/VERR/VERW
        case 0x01: return grp0f01();   // SGDT/SIDT/LGDT/LIDT/SMSW/LMSW/INVLPG
        case 0x02: case 0x03: {
            // LAR/LSL: protected mode only, report success in ZF instead of
            // faulting.
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
                // Conforming is visible from anywhere; otherwise the effective
                // privilege must reach the DPL.
                if (!acc_conforming(a) && eff > acc_dpl(a)) ok = false;
                // LSL refuses gates, which have no limit.
                if (ok && op2 == 0x03 && acc_system(a)) {
                    int t = sys_type(a);
                    if (t != SYS_TSS16_AVAIL && t != SYS_TSS16_BUSY && t != SYS_LDT &&
                        t != SYS_TSS32_AVAIL && t != SYS_TSS32_BUSY) ok = false;
                }
            }
            if (ok) {
                if (op2 == 0x02) {
                    // LAR returns the access rights: the high dword with base
                    // and limit masked out (Intel 80486 PRM, LAR).
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
            // CLTS clears CR0.TS, handing the FPU to this task.
            if (protected_mode() && cpl() != 0) raise_err(EXC_GP, 0);
            cr_[0] &= ~uint32_t(CR0_TS);
            return 7;
        case 0x08: case 0x09:
            // INVD/WBINVD empty the L1 and run the flush cycle, which empties
            // the board's L2. Timing only; the model holds no data.
            if (protected_mode() && cpl() != 0) raise_err(EXC_GP, 0);
            if (timing) { timing->invalidate_l1(); timing->invalidate_l2(); }
            return op2 == 0x08 ? 4 : 5;
        case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x26:
            return mov_control_reg(op2);
        case 0xA0: if (opsize32_) push32(fs); else push16(fs); return 3;
        case 0xA1: load_seg(SEG_FS, opsize32_ ? uint16_t(pop32()) : pop16()); return 3;
        case 0xA8: if (opsize32_) push32(gs); else push16(gs); return 3;
        case 0xA9: load_seg(SEG_GS, opsize32_ ? uint16_t(pop32()) : pop16()); return 3;

        case 0xA2:
            // CPUID exists on the SL-Enhanced IntelDX2, Intel486 SX and DX;
            // older steppings fault (AP-485 processor list).
            // Function 0: max input 1 and "GenuineIntel" in EBX:EDX:ECX
            // (function 2's cache descriptors are Pentium-era). Function 1:
            // type 0, family 4, model 3 (AP-485); the only feature bit is 0,
            // the on-die FPU. Inputs above the maximum return function 1's data
            // (AP-485).
            if (eax == 0) {
                eax = 1;
                ebx = 0x756E6547u;  // "Genu"
                edx = 0x49656E69u;  // "ineI"
                ecx = 0x6C65746Eu;  // "ntel"
            } else {
                eax = kComponentId;  // same as EDX after RESET
                ebx = 0;
                ecx = 0;
                edx = 0x00000001u;  // FPU on die
            }
            // The Quantasm table lists no 486 CPUID figure; 14 is the Pentium
            // timing.
            return 14;

        // SHLD/SHRD r/m, r, imm8 | CL
        case 0xA4: case 0xAC: {
            RM rm = decode_modrm();
            int src = last_reg_;
            int count = fetch8();
            disp_imm(rm);
            shld(rm, src, count, op2 == 0xAC);
            return rm.is_mem ? 3 : 2;
        }
        case 0xA5: case 0xAD: {
            RM rm = decode_modrm();
            int src = last_reg_;
            shld(rm, src, get_reg8(1), op2 == 0xAD);
            return rm.is_mem ? 4 : 3;
        }

        case 0xA3: case 0xAB: case 0xB3: case 0xBB: {  // BT/BTS/BTR/BTC r/m, r
            RM rm = decode_modrm();
            int32_t bit = opsize32_ ? int32_t(get_reg32(last_reg_)) : int32_t(int16_t(get_reg16(last_reg_)));
            int opsz = opsize32_ ? 32 : 16;
            RM target = rm;
            if (rm.is_mem) {
                // A memory operand's bit offset is unmasked; it indexes a
                // string of operand-size units from the effective address
                // (Intel 80486 PRM, BT/BTS/BTR/BTC).
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
            disp_imm(rm);
            if (sub < 4) raise_ud(instr_start_eip_, uint16_t(0x0F00 | op2));   // /0-/3 are reserved
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
            // Published 6 for a register destination, "7-10" for memory. Read
            // as compare-only vs compare-plus-store; Intel doesn't say which
            // end is which.
            if (!rm.is_mem) return 6;
            return equal ? 10 : 7;
        }

        case 0xB2: case 0xB4: case 0xB5: {  // LSS/LFS/LGS r16/32, m16:16 or m16:32
            RM rm = decode_modrm();
            if (!rm.is_mem) raise_ud(instr_start_eip_, uint16_t(0x0F00 | op2));
            int dst = last_reg_;
            uint32_t off;
            uint16_t seg;
            if (opsize32_) { off = read32(rm.seg, rm.off); seg = read16(rm.seg, seg_off(rm.off, 4)); }
            else { off = read16(rm.seg, rm.off); seg = read16(rm.seg, seg_off(rm.off, 2)); }
            // Load the segment first; a fault must leave the offset register
            // untouched.
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
            // ZF is the only defined flag; the destination is undefined for a
            // zero source (Intel 80486 PRM), so it is left untouched.
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
            // BSWAP r32 (486). The 16-bit form is undefined, so always 32 bits.
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
                // Published SETcc: r8 4/3, mem8 3/4 (true/false), inverted
                // between the two as printed.
                if (rm.is_mem) return t ? 3 : 4;
                return t ? 4 : 3;
            }
            raise_ud(instr_start_eip_, uint16_t(0x0F00 | op2));
    }
}

// --- x87 FPU --------------------------------------------------------------
// State is held in the 80-bit Float80 format so FLD m80 / FSTP m80 round-trips
// are bit-exact; arithmetic converts to the host's `long double` and back.
// On arm64 and in wasm `long double` is binary128, so results double-round and
// can differ from hardware in the last significand bit for a few operands.
// Precision control (bits 8-9) is applied on top. Rounding control (bits 10-11)
// is honored for FIST/FISTP/FRNDINT only; arithmetic always rounds to nearest.

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

// Published i486 x87 cycle counts. Where a range is printed the floor is
// charged; Intel documents no mechanism that could be fitted to both endpoints.
constexpr int kFldMem32 = 3, kFldMem64 = 3, kFldMem80 = 6, kFldReg = 4;
constexpr int kFstMem32 = 7, kFstMem64 = 8, kFstMem80 = 6, kFstReg = 3;
constexpr int kFAdd = 8, kFMul = 16, kFDiv = 73, kFSqrt = 83, kFCom = 4;
constexpr int kFild = 13, kFist = 29, kFbld = 75, kFbstp = 175;
// Integer-operand arithmetic (FIADD etc.) is published as larger rows; this
// charges the operation's figure plus FILD's, a composition rather than the
// combined range.
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
    // M carries an explicit integer bit: scaling the [0.5,1) mantissa by 2^64
    // puts M in [2^63, 2^64) with E = exp2 + 16382.
    long double scaled = std::ldexp(m, 64);
    uint64_t sig = uint64_t(scaled);
    int e = exp2 + 16382;
    if (e >= 0x7FFF) {                              // overflows the format: infinity
        f.sign_exp = uint16_t(neg ? 0xFFFFu : 0x7FFFu);
        f.significand = 0x8000000000000000ull;
        return f;
    }
    if (e <= 0) {
        // Denormal: shift right until the exponent reaches the format's floor.
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
        // Normals and denormals share a formula because the integer bit is
        // explicit.
        v = std::ldexp(static_cast<long double>(m), (e == 0 ? 1 : e) - 16383 - 63);
    }
    return neg ? -v : v;
}

long double Cpu::st_value(int i) const { return from_float80(fpu_reg_[(fpu_top_ + i) & 7]); }

void Cpu::fpu_init() {
    // FINIT/RESET state (Intel 80486 PRM, "FPU Initialization"): control word
    // 037Fh, status 0, all registers empty.
    fpu_cw_ = 0x037F;
    fpu_sw_ = 0;
    fpu_tw_ = 0xFFFF;
    fpu_top_ = 0;
    for (int i = 0; i < 8; ++i) fpu_reg_[i] = Float80{};
    fpu_last_ip_ = fpu_last_op_ = 0;
    fpu_last_cs_ = fpu_last_ds_ = fpu_last_opcode_ = 0;
}

void Cpu::fpu_check_available() {
    // CR0.EM traps every ESC opcode to #NM for a software emulator; CR0.TS
    // traps the first one after a task switch so a handler can swap FPU state.
    if (cr_[0] & (CR0_EM | CR0_TS)) raise(EXC_NM);
}

void Cpu::fpu_set_tag(int phys, bool empty) {
    int shift = (phys & 7) * 2;
    fpu_tw_ = uint16_t((fpu_tw_ & ~(0x3u << shift)) | (uint32_t(empty ? 3u : 0u) << shift));
}
// The tag register only tracks empty or not; the stored tag word classifies
// each full register: 00 valid, 01 zero, 10 special, 11 empty (Intel 80486 PRM,
// "Tag Word"; Bochs fpu_tags.cc packs it the same way).
uint16_t Cpu::fpu_tag_word() const {
    uint16_t tw = 0;
    for (int phys = 0; phys < 8; ++phys) {
        unsigned tag;
        if (((fpu_tw_ >> (phys * 2)) & 3u) == 3u) {
            tag = 3;
        } else {
            const Float80 &f = fpu_reg_[phys];
            unsigned e = f.sign_exp & 0x7FFFu;
            if (e == 0x7FFFu) tag = 2;
            else if (e == 0) tag = f.significand == 0 ? 1 : 2;
            else tag = (f.significand >> 63) ? 0 : 2;
        }
        tw = uint16_t(tw | (tag << (phys * 2)));
    }
    return tw;
}
// FLDENV and FRSTOR keep only which registers are empty; the rest comes
// back from the contents when the word is next stored.
void Cpu::fpu_load_tag_word(uint16_t tw) {
    fpu_tw_ = 0;
    for (int phys = 0; phys < 8; ++phys)
        if (((tw >> (phys * 2)) & 3u) == 3u) fpu_tw_ = uint16_t(fpu_tw_ | (3u << (phys * 2)));
}
bool Cpu::fpu_is_empty(int i) const {
    int phys = (fpu_top_ + i) & 7;
    return ((fpu_tw_ >> (phys * 2)) & 3u) == 3u;
}

void Cpu::fpu_xch(int i) {
    // An empty operand is a stack underflow: masked, it reads as the real
    // indefinite and the exchange goes ahead; unmasked, nothing moves (Intel
    // 80486 PRM, FXCH).
    int other = (fpu_top_ + i) & 7;
    bool empty_top = fpu_is_empty(0), empty_other = fpu_is_empty(i);
    if (empty_top || empty_other) {
        fpu_stack_fault(false);
        if (!(fpu_cw_ & kFswIE)) return;
        if (empty_top) fpu_reg_[fpu_top_] = Float80{kIndefiniteSig, kIndefiniteExp};
        if (empty_other) fpu_reg_[other] = Float80{kIndefiniteSig, kIndefiniteExp};
    }
    std::swap(fpu_reg_[fpu_top_], fpu_reg_[other]);
    fpu_set_tag(fpu_top_, false);
    fpu_set_tag(other, false);
}

void Cpu::fpu_stack_fault(bool overflow) {
    // Stack fault is IE with SF set; C1 gives overflow (1) vs underflow (0)
    // (Intel 80486 PRM, "Stack Fault").
    fpu_sw_ |= kFswIE | kFswSF;
    if (overflow) fpu_sw_ |= kFswC1; else fpu_sw_ &= ~kFswC1;
    if (!(fpu_cw_ & kFswIE)) fpu_sw_ |= kFswES | kFswB;
}

void Cpu::fpu_push(long double v) {
    int next = (fpu_top_ - 1) & 7;
    if (((fpu_tw_ >> (next * 2)) & 3u) != 3u) {
        // Push onto a full stack: the destination gets the indefinite QNaN.
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
    // Precision control (bits 8-9): 00 single, 10 double, 11 extended (reset
    // default); 01 is reserved, treated as extended.
    switch ((fpu_cw_ >> 8) & 3u) {
        case 0: return static_cast<long double>(static_cast<float>(v));
        case 2: return static_cast<long double>(static_cast<double>(v));
        default: return v;
    }
}

void Cpu::fpu_compare(long double a, long double b, bool unordered_ok) {
    // FCOM sets C3/C2/C0 as a three-way result, all three for unordered. FCOM
    // signals #IA on unordered; FUCOM does not (Intel 80486 PRM, FCOM/FUCOM).
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
    // Decode ModR/M first so instruction length is right on every path,
    // faulting ones included.
    uint32_t opcode_eip = instr_start_eip_;
    RM rm = decode_modrm();
    int sub = last_reg_;
    int i = rm.is_mem ? 0 : rm.reg;      // ST(i) index for the register forms
    // Register forms are conventionally whole ModR/M bytes (D9 E0 = FCHS), so
    // reassemble one.
    uint8_t modrm_low = uint8_t(0xC0u | (uint32_t(sub) << 3) | uint32_t(rm.is_mem ? 0 : rm.reg));

    fpu_check_available();
    // Encodings the Intel486 reserves raise #UD (Intel SDM Vol. 3, "Undefined
    // Opcodes"). FCMOVcc, FCOMI and FISTTP arrived later; the undocumented
    // aliases below did not.
    bool reserved;
    if (rm.is_mem) {
        reserved = (sub == 1 && (op == 0xD9 || op == 0xDB || op == 0xDD || op == 0xDF)) ||
                   (op == 0xDB && (sub == 4 || sub == 6)) ||
                   (op == 0xDD && sub == 5);
    } else {
        switch (op) {
            case 0xD9: reserved = (modrm_low >= 0xD1 && modrm_low <= 0xD7) || modrm_low == 0xE2 || modrm_low == 0xE3 ||
                                  modrm_low == 0xE6 || modrm_low == 0xE7 || modrm_low == 0xEF; break;
            case 0xDA: reserved = modrm_low != 0xE9; break;
            case 0xDB: reserved = modrm_low < 0xE0 || modrm_low > 0xE4; break;
            case 0xDD: reserved = sub >= 6; break;
            case 0xDE: reserved = sub == 3 && modrm_low != 0xD9; break;
            case 0xDF: reserved = sub >= 4 && modrm_low != 0xE0; break;
            default:   reserved = false; break;
        }
    }
    if (reserved) raise_ud(opcode_eip, uint16_t(0xD800u | (uint32_t(op & 7) << 8) | modrm_low));

    // A pending unmasked exception is reported on the next FPU instruction
    // (487/486 deferred error). The seven no-wait encodings don't check, so a
    // handler can use FNSTSW and FNCLEX on the error it was called for.
    bool no_wait =
        (op == 0xDB && !rm.is_mem && (modrm_low == 0xE2 || modrm_low == 0xE3)) ||  // FNCLEX, FNINIT
        (op == 0xDF && !rm.is_mem && modrm_low == 0xE0) ||                         // FNSTSW AX
        (op == 0xDD && rm.is_mem && (sub == 6 || sub == 7)) ||                     // FNSAVE, FNSTSW m16
        (op == 0xD9 && rm.is_mem && (sub == 6 || sub == 7));                       // FNSTENV, FNSTCW
    if (!no_wait && (fpu_sw_ & kFswES) && (cr_[0] & CR0_NE)) raise(EXC_MF);

    // Operand pointers FSTENV/FSAVE hand a handler, recorded for every ESC
    // instruction.
    fpu_last_ip_ = opcode_eip;
    fpu_last_cs_ = cs;
    fpu_last_opcode_ = uint16_t(((op & 0x07u) << 8) | modrm_low);
    if (rm.is_mem) { fpu_last_op_ = rm.off; fpu_last_ds_ = seg_sel(rm.seg); }

    auto div_by_zero = [&]() {
        fpu_sw_ |= kFswZE;
        if (!(fpu_cw_ & kFswZE)) fpu_sw_ |= kFswES | kFswB;
    };
    auto arith = [&](int kind, long double a, long double b) -> long double {
        // kind: 0 ADD, 1 MUL, 4 SUB, 5 SUBR, 6 DIV, 7 DIVR, the ESC reg-field
        // order.
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
    // Integer conversion honors the rounding-control field; a compiler's (int)
    // cast sets RC to truncate around FISTP.
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
        // Out-of-range or NaN is invalid; masked, it returns the integer
        // indefinite (the most negative value).
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
                // One published figure covers ST(i) and memory operands, so the
                // load is not added.
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
            // Register form: destination is ST(i), and the subtract/divide
            // pairs are encoded reversed relative to D8 (DC E0+i is FSUBR, DC
            // E8+i is FSUB).
            if (sub == 2 || sub == 3) {  // DC D0+i / D8+i: undocumented FCOM/FCOMP aliases
                fpu_compare(fpu_get(0), fpu_get(i), false);
                if (sub == 3) fpu_pop();
                return kFCom;
            }
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
            if (sub == 2) {  // DE D0+i: undocumented FCOMP alias
                fpu_compare(fpu_get(0), fpu_get(i), false);
                fpu_pop();
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
            // FUCOMPP, the 387/486 unordered compare, is the only register form.
            fpu_compare(fpu_get(0), fpu_get(1), true);
            fpu_pop(); fpu_pop();
            return kFCom;
        case 0xD9:
            if (rm.is_mem) {
                switch (sub) {
                    case 0: fpu_push(load_m32()); return kFldMem32;
                    case 2: store_m32(fpu_get(0)); return kFstMem32;                 // FST m32
                    case 3: store_m32(fpu_get(0)); fpu_pop(); return kFstMem32;       // FSTP m32
                    case 4: {  // FLDENV
                        bool env32 = opsize32_;
                        uint32_t o = rm.off;
                        ac_check_whole(rm.seg, o, env32 ? 4 : 2);
                        fpu_cw_ = read16(rm.seg, o);
                        uint16_t sw = read16(rm.seg, seg_off(o, env32 ? 4 : 2));
                        fpu_load_tag_word(read16(rm.seg, seg_off(o, env32 ? 8 : 4)));
                        fpu_top_ = (sw >> 11) & 7;
                        fpu_sw_ = uint16_t(sw & ~0x3800u);
                        ac_skip_ = false;
                        return kFldenv;
                    }
                    case 5: fpu_cw_ = read16(rm.seg, rm.off); return kFldcw;          // FLDCW
                    case 6: {  // FNSTENV -- the 14/28-byte environment
                        bool env32 = opsize32_;
                        uint32_t o = rm.off;
                        ac_check_whole(rm.seg, o, env32 ? 4 : 2);
                        if (env32) {
                            write32(rm.seg, o, fpu_cw_);
                            write32(rm.seg, seg_off(o, 4), fpu_status());
                            write32(rm.seg, seg_off(o, 8), fpu_tag_word());
                            write32(rm.seg, seg_off(o, 12), fpu_last_ip_);
                            write32(rm.seg, seg_off(o, 16), uint32_t(fpu_last_cs_) | (uint32_t(fpu_last_opcode_ & 0x07FFu) << 16));
                            write32(rm.seg, seg_off(o, 20), fpu_last_op_);
                            write32(rm.seg, seg_off(o, 24), fpu_last_ds_);
                        } else {
                            write16(rm.seg, o, fpu_cw_);
                            write16(rm.seg, seg_off(o, 2), fpu_status());
                            write16(rm.seg, seg_off(o, 4), fpu_tag_word());
                            write16(rm.seg, seg_off(o, 6), uint16_t(fpu_last_ip_));
                            write16(rm.seg, seg_off(o, 8), uint16_t(((fpu_last_ip_ >> 16) << 12) | (fpu_last_opcode_ & 0x07FFu)));
                            write16(rm.seg, seg_off(o, 10), uint16_t(fpu_last_op_));
                            write16(rm.seg, seg_off(o, 12), uint16_t((fpu_last_op_ >> 16) << 12));
                        }
                        // FSTENV masks every exception afterwards so its
                        // handler can't re-fault.
                        fpu_cw_ |= 0x003Fu;
                        ac_skip_ = false;
                        return kFstenv;
                    }
                    default: write16(rm.seg, rm.off, fpu_cw_); return kFstcw;          // FNSTCW (/1 is reserved, above)
                }
            }
            switch (modrm_low) {
                case 0xC0: case 0xC1: case 0xC2: case 0xC3:
                case 0xC4: case 0xC5: case 0xC6: case 0xC7:
                    fpu_push(fpu_get(modrm_low - 0xC0));                                 // FLD ST(i)
                    return kFldReg;
                case 0xC8: case 0xC9: case 0xCA: case 0xCB:
                case 0xCC: case 0xCD: case 0xCE: case 0xCF:                              // FXCH ST(i)
                    fpu_xch(modrm_low - 0xC8);
                    return kFxch;
                case 0xD0: return kFsimple;                                              // FNOP
                case 0xD8: case 0xD9: case 0xDA: case 0xDB:
                case 0xDC: case 0xDD: case 0xDE: case 0xDF: {
                    // Undocumented FSTP ST(i) alias that skips the underflow
                    // check on an empty ST(0) (undocumented x86 list,
                    // "FSTPNCE"; sandpile.org FPU map).
                    int src = fpu_top_, dst = (fpu_top_ + i) & 7;
                    bool empty = fpu_is_empty(0);
                    fpu_reg_[dst] = fpu_reg_[src];
                    fpu_set_tag(dst, empty);
                    fpu_set_tag(src, true);
                    fpu_top_ = (fpu_top_ + 1) & 7;
                    return kFstReg;
                }
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
                    // C2 = 0 means the reduction is complete. Hardware can
                    // leave it set after a partial reduction of a huge
                    // argument; the exact remainder here is always complete,
                    // which is the contract software tests.
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
                default: fpu_set(0, std::cos(fpu_get(0))); fpu_sw_ &= ~kFswC2; return kFsin;  // FCOS (FF)
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
                    default: {  // FSTP m80real (/7; /1, /4, /6 are reserved, above)
                        store_m80(fpu_reg_[fpu_top_]);
                        fpu_set_tag(fpu_top_, true);
                        fpu_top_ = (fpu_top_ + 1) & 7;
                        return kFstMem80;
                    }
                }
            }
            switch (modrm_low) {
                // FENI/FDISI are no-ops from the 287 on; FSETPM is ignored by
                // the 387 and 486.
                case 0xE0: case 0xE1: case 0xE4: return kFsimple;
                case 0xE2: fpu_sw_ &= ~(kFswIE | kFswDE | kFswZE | kFswOE | kFswUE | kFswPE | kFswSF | kFswES | kFswB); return kFclex;  // FNCLEX
                default: fpu_init(); return kFinit;                                       // FNINIT (E3)
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
                        ac_check_whole(rm.seg, o, env32 ? 4 : 2);
                        uint32_t hdr = env32 ? 28 : 14;
                        fpu_cw_ = read16(rm.seg, o);
                        uint16_t sw = read16(rm.seg, seg_off(o, env32 ? 4 : 2));
                        fpu_load_tag_word(read16(rm.seg, seg_off(o, env32 ? 8 : 4)));
                        fpu_top_ = (sw >> 11) & 7;
                        fpu_sw_ = uint16_t(sw & ~0x3800u);
                        for (int r = 0; r < 8; ++r) {
                            uint32_t base = seg_off(o, hdr + uint32_t(r) * 10);
                            Float80 f;
                            f.significand = read64(rm.seg, base);
                            f.sign_exp = read16(rm.seg, seg_off(base, 8));
                            fpu_reg_[(fpu_top_ + r) & 7] = f;
                        }
                        ac_skip_ = false;
                        return kFrstor;
                    }
                    case 6: {  // FNSAVE -- environment plus all eight registers, then reset
                        bool env32 = opsize32_;
                        uint32_t o = rm.off;
                        ac_check_whole(rm.seg, o, env32 ? 4 : 2);
                        uint32_t hdr = env32 ? 28 : 14;
                        if (env32) {
                            write32(rm.seg, o, fpu_cw_);
                            write32(rm.seg, seg_off(o, 4), fpu_status());
                            write32(rm.seg, seg_off(o, 8), fpu_tag_word());
                            write32(rm.seg, seg_off(o, 12), fpu_last_ip_);
                            write32(rm.seg, seg_off(o, 16), uint32_t(fpu_last_cs_) | (uint32_t(fpu_last_opcode_ & 0x07FFu) << 16));
                            write32(rm.seg, seg_off(o, 20), fpu_last_op_);
                            write32(rm.seg, seg_off(o, 24), fpu_last_ds_);
                        } else {
                            write16(rm.seg, o, fpu_cw_);
                            write16(rm.seg, seg_off(o, 2), fpu_status());
                            write16(rm.seg, seg_off(o, 4), fpu_tag_word());
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
                        ac_skip_ = false;
                        return kFsave;
                    }
                    default: write16(rm.seg, rm.off, fpu_status()); return kFstsw;         // FNSTSW m16 (/1, /5 are reserved, above)
                }
            }
            switch (sub) {
                case 0: fpu_set_tag((fpu_top_ + i) & 7, true); return kFsimple;            // FFREE ST(i)
                case 1: fpu_xch(i); return kFxch;                                          // DD C8+i: undocumented FXCH alias
                case 2: fpu_set(i, fpu_get(0)); return kFstReg;                            // FST ST(i)
                case 3: fpu_set(i, fpu_get(0)); fpu_pop(); return kFstReg;                 // FSTP ST(i)
                case 4: fpu_compare(fpu_get(0), fpu_get(i), true); return kFCom;           // FUCOM ST(i)
                default: fpu_compare(fpu_get(0), fpu_get(i), true); fpu_pop(); return kFCom;  // FUCOMP ST(i)
            }
        default:  // 0xDF
            if (rm.is_mem) {
                switch (sub) {
                    case 0: fpu_push(static_cast<long double>(int16_t(read16(rm.seg, rm.off)))); return kFild;  // FILD m16int
                    case 2: store_int(2); return kFist;                                            // FIST m16int
                    case 3: store_int(2); fpu_pop(); return kFist;                                 // FISTP m16int
                    case 4: {  // FBLD m80bcd
                        // Bytes 0-8 hold 18 packed decimal digits, low byte
                        // first; byte 9's top bit is the sign.
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
                    default: store_int(8); fpu_pop(); return kFist;                                 // FISTP m64int (/1 is reserved, above)
                }
            }
            switch (sub) {
                case 0:  // DF C0+i: FFREEP, FFREE ST(i) then pop, with no stack fault
                    fpu_set_tag((fpu_top_ + i) & 7, true);
                    fpu_set_tag(fpu_top_, true);
                    fpu_top_ = (fpu_top_ + 1) & 7;
                    return kFsimple;
                case 1: fpu_xch(i); return kFxch;                                          // DF C8+i: undocumented FXCH alias
                case 2: case 3: fpu_set(i, fpu_get(0)); fpu_pop(); return kFstReg;          // DF D0+i / D8+i: undocumented FSTP aliases
                default: set_reg16(0, fpu_status()); return kFstsw;                        // FNSTSW AX (E0)
            }
    }
}

// --- main decode loop ------------------------------------------------------

int Cpu::step() {
    // HLT idles until an interrupt; the published cost per idle step keeps
    // wall-clock pacing advancing.
    if (halted) { cycles += 4; halt_cycles += 4; return 4; }
    // In real mode the base tracks selector*16, so a directly assigned selector
    // field is a segment load.
    if (!protected_mode()) refresh_real_bases();
    uint32_t start_eip = eip;
    step_start_eip_ = eip;
    rep_resumed_ = rep_resume_;
    rep_resume_ = false;
    instr_start_esp_ = esp;
    instr_start_ss_ = ss;
    instr_start_ss_desc_ = sd_[SEG_SS];
    // One branch covers the rare debug work on the hottest path.
    if ((eflags & FLAG_RF) || dbg_exec_) {
        int f = step_debug(start_eip);
        if (f >= 0) return f;
    }
    // Code fetch: the L1 line the instruction starts in, while it sits in the
    // prefetch window.
    if (timing && eip - pf_lo_ < pf_hi_ - pf_lo_)
        stall_ += uint32_t(timing->fetch(pf_phys_ + (eip - pf_lo_), fills(pf_phys_), cycles));
    if (timing) {
        agi_cur_ ^= 1;
        uint32_t *r = agi_snap_[agi_cur_];
        r[0] = eax; r[1] = ecx; r[2] = edx; r[3] = ebx;
        r[4] = esp; r[5] = ebp; r[6] = esi; r[7] = edi;
    }
    // Single-step traps after an instruction that began with TF set, so a POPF
    // that sets TF runs untrapped and one that clears it still traps.
    bool trap = flag(FLAG_TF);
    shadow_ = false;
    vectored_ = false;
    int c;
#ifdef __EMSCRIPTEN__
    fault_jmp_set_ = true;
    if (setjmp(fault_jmp_) == 0) {
        c = step_inner();
        fault_jmp_set_ = false;
        // Ring>0 HLT arms fault_pending_ without longjmp.
        if (fault_pending_) {
            fault_pending_ = false;
            return deliver_fault(pending_fault_, start_eip);
        }
    } else {
        fault_jmp_set_ = false;
        fault_pending_ = false;
        return deliver_fault(pending_fault_, start_eip);
    }
#else
    try {
        c = step_inner();
        if (fault_pending_) {
            fault_pending_ = false;
            return deliver_fault(pending_fault_, start_eip);
        }
    } catch (const Fault &f) {
        fault_pending_ = false;
        return deliver_fault(f, start_eip);
    }
#endif
    c += take_stall();
    // INT n / INT3 / INTO clear TF going into the handler, which runs untrapped
    // (Intel 80486 PRM, "Single-Step Trap"). A shadow holds the trap off until
    // after the next instruction. Data breakpoints and the TSS T bit trap in
    // the same #DB with every applicable status bit.
    bool step_trap = trap && !vectored_;
    if ((step_trap || dbg_pending_) && !shadow_) {
        if (step_trap) dr_[6] |= 0x4000u;                      // DR6.BS
        if (dbg_pending_ & kDbgTask) dr_[6] |= 0x8000u;        // DR6.BT
        dr_[6] |= dbg_pending_ & 0x0Fu;
        dbg_pending_ = 0;
        c += interrupt(uint8_t(EXC_DB));
    }
    return c;
}

int Cpu::step_debug(uint32_t start_eip) {
    // An instruction breakpoint faults on the first byte, held off one
    // instruction by RF. A REP continuing after an internal yield isn't a new
    // fetch.
    if (dbg_exec_ && !(eflags & FLAG_RF) && !rep_resumed_) {
        if (uint8_t hit = dbg_exec_match(sd_[SEG_CS].base + eip)) {
            dr_[6] |= hit;
            return deliver_fault(Fault{EXC_DB, 0, false}, start_eip);
        }
    }
    // RF clears when an instruction completes; IRET and a task switch can load
    // it again (Intel 80386 PRM, "Debug Exceptions").
    eflags &= ~FLAG_RF;
    return -1;
}

int Cpu::deliver_fault(const Fault &first, uint32_t start_eip) {
    agi_clear();
    split_ = false;
    // A fault restores the instruction's starting EIP and stack pointer before
    // the handler runs, so a restart doesn't repeat half-pushed operands (Intel
    // 80486 PRM, "Exception Classes").
    eip = start_eip;
    esp = instr_start_esp_;
    ss = instr_start_ss_;
    sd_[SEG_SS] = instr_start_ss_desc_;
    Fault f = first;
    // Every fault pushes EFLAGS with RF set so the restarting IRET doesn't
    // re-take an instruction breakpoint (Intel 80386 PRM, "Debug Exceptions").
    // Real mode's 16-bit FLAGS has no RF.
    dbg_pending_ = 0;
    ac_skip_ = false;
    if (protected_mode()) eflags |= FLAG_RF;
    if (on_fault) on_fault(f.vector, f.error, cs, eip);
    for (int attempt = 0; attempt < 2; ++attempt) {
#ifdef __EMSCRIPTEN__
        fault_jmp_set_ = true;
        if (setjmp(fault_jmp_) == 0) {
            do_interrupt(uint8_t(f.vector), false, f.has_error, f.error);
            fault_jmp_set_ = false;
            int c = protected_mode() ? 44 : 26;
            cycles += c;
            return c + take_stall();
        }
        fault_jmp_set_ = false;
        if (f.vector == EXC_DF) break;
        f = Fault{EXC_DF, 0, true};
        if (on_fault) on_fault(EXC_DF, 0, cs, eip);
#else
        try {
            do_interrupt(uint8_t(f.vector), false, f.has_error, f.error);
            // Published INT: 26 for real-mode vectoring (INT3's figure), 44 for
            // a protected-mode gate. The inter-privilege figure is higher;
            // charged as 44 here.
            int c = protected_mode() ? 44 : 26;
            cycles += c;
            return c + take_stall();
        } catch (const Fault &) {
            // A fault while delivering a fault is a double fault; one while
            // delivering that is shutdown until RESET or NMI (Intel 80486 PRM,
            // "Double Fault").
            if (f.vector == EXC_DF) break;
            f = Fault{EXC_DF, 0, true};
            if (on_fault) on_fault(EXC_DF, 0, cs, eip);
        }
#endif
    }
    halted = true;
    shutdown_ = true;
    cycles += 4;
    return 4;
}

namespace {
// The eleven x86 prefix bytes by opcode; 0 means not a prefix.
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
    len_check_ = false;
    // The default operand and address size is CS.D (16-bit in real mode).
    // 0x66/0x67 toggle it rather than selecting 32 bits.
    bool dflt32 = code32();
    opsize32_ = dflt32;
    addrsize32_ = dflt32;
    extra_cycles_ = 0;
    int c = 0;
    int prefixes = 0;
    bool lock = false;

    uint8_t op;
    for (;;) {
        op = fetch8();
        // One table lookup decides prefix-or-not; a switch cost 4.5% of a BOOM
        // run (PC486_REVIEW.md §16).
        uint8_t kind = kPrefix[op];
        if (kind == 0) break;
        PC486_PERF_BUMP(perf.opcode[op]);   // prefix bytes, counted in their own right
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
            default: lock = true; break;                    // LOCK
        }
        ++prefixes;
        // The 486 column publishes only LOCK (1 clock). The segment,
        // operand-size and address-size prefixes are charged the same 1 clock,
        // a uniform extension. A REP continuation already paid.
        if (!rep_resumed_) c += 1;
    }
    // 0Fh is an escape; two_byte() counts the opcode it runs.
    if (op != 0x0F) PC486_PERF_BUMP(perf.opcode[op]);
    instr_start_eip_ = (eip - 1) & ip_mask();  // eip has already advanced past `op`
    if (prefixes) {
        if (prefixes >= 4) arm_length_limit(prefixes);
        if (lock && !lock_allowed(op)) raise(EXC_UD);
    }

    // Fast path: the dense ALU block 0x00-0x3D, 8 groups of 6 opcodes; the
    // +6/+7 slots (segment push/pop, BCD adjust) are excluded by (op & 7) > 5.
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
        case 0x17: load_seg(SEG_SS, opsize32_ ? uint16_t(pop32()) : pop16()); shadow_ = true; c += 3; break;
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
            // PUSH SP/ESP pushes the pre-decrement value (286+), so no special
            // case.
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
            // ARPL raises the destination selector's RPL to the source's,
            // setting ZF if it changed anything. Protected mode only; #UD in
            // real mode.
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
            disp_imm(rm);
            imul_imm(r, rm, imm);
            c += mul_cost(imm, opsize32_ ? 32 : 16);
            break;
        }
        case 0x6A: { int8_t imm = int8_t(fetch8()); if (opsize32_) push32(uint32_t(int32_t(imm))); else push16(uint16_t(int16_t(imm))); c += 1; break; }
        case 0x6B: {  // IMUL r,r/m,imm8 (sign-extended)
            RM rm = decode_modrm(); int r = last_reg_;
            uint32_t imm = uint32_t(int32_t(int8_t(fetch8())));
            disp_imm(rm);
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

        // --- MOV r/m,r and r,r/m (published 486: 1 clock either way) ---
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
            if (!rm.is_mem) raise_ud(instr_start_eip_, op);   // a register has no address
            // Uses the full 32-bit effective address: LEA touches no memory, so
            // a 32-bit form is arithmetic.
            if (opsize32_) set_reg32(last_reg_, rm.off); else set_reg16(last_reg_, uint16_t(rm.off));
            c += 1;  // published 1-2; the 2 case is the base+index+disp penalty decode_modrm() already added
            break;
        }
        case 0x8E: {  // MOV sreg, r/m16
            RM rm = decode_modrm();
            int si = last_reg_ & 7;
            // CS isn't loadable this way; reg fields 6 and 7 name no segment
            // register.
            if (si == SEG_CS || si > SEG_GS) raise(EXC_UD);
            load_seg(si, rm_read16(rm));
            if (si == SEG_SS) shadow_ = true;
            c += 3;
            break;
        }
        case 0x8F: {  // POP r/m
            // POP [ESP+n] resolves against the post-pop ESP (Intel SDM, POP),
            // so the pop happens before decoding the ModRM/SIB.
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
            // WAIT/FWAIT reports #NM when CR0.MP and CR0.TS are both set, so a
            // task-switch handler can save the previous task's FPU state.
            if ((cr_[0] & CR0_MP) && (cr_[0] & CR0_TS)) raise(EXC_NM);
            c += 1;  // published 1-3
            break;
        case 0x9C:  // PUSHF / PUSHFD
            // PUSHFD is half of the AP-485 486-detection sequence, so AC must
            // survive a round trip.
            // In V86, PUSHF, POPF and IRET are IOPL-sensitive: at IOPL < 3 they
            // #GP to the monitor (Intel 80386 PRM, "Additional Sensitive
            // Instructions").
            if (v86_mode() && iopl() != 3) raise_err(EXC_GP, 0);
            if (opsize32_) push32(eflags); else push16(uint16_t(eflags));
            c += 4;
            break;
        case 0x9D:  // POPF / POPFD
            // Real mode loads IOPL and NT unconditionally, and AC for the
            // 32-bit form. ibmpc-at's 286 core masks IOPL/NT here for a FreeDOS
            // 1.3 installer problem (root cause unconfirmed,
            // IBM_PCAT_REVIEW.md).
            // In protected mode IOPL is writable only at CPL 0 and IF only when
            // CPL <= IOPL; otherwise the old value is silently kept (Intel
            // 80486 PRM, POPF). V86 faults at IOPL < 3, as for PUSHF.
            if (v86_mode() && iopl() != 3) raise_err(EXC_GP, 0);
            {
                // POPF/POPFD never affect VM or RF (Intel 80486 PRM,
                // "POPF/POPFD"). The masks keep the popped value from supplying
                // VM; this keeps the OR below from zeroing a running V86 task's
                // VM.
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
            // The displacement width follows the address size and isn't
            // truncated (PC486_REVIEW.md §5.4).
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
            if (!rm.is_mem) raise_ud(instr_start_eip_, op);
            uint32_t off;
            uint16_t seg;
            if (opsize32_) { off = read32(rm.seg, rm.off); seg = read16(rm.seg, seg_off(rm.off, 4)); }
            else { off = read16(rm.seg, rm.off); seg = read16(rm.seg, seg_off(rm.off, 2)); }
            load_seg((op == 0xC4) ? int(SEG_ES) : int(SEG_DS), seg);
            if (opsize32_) set_reg32(r, off); else set_reg16(r, uint16_t(off));
            c += 6;
            break;
        }
        case 0xC6: { RM rm = decode_modrm(); uint8_t imm = fetch8(); disp_imm(rm); rm_write8(rm, imm); c += 1; break; }
        case 0xC7: { RM rm = decode_modrm(); disp_imm(rm); if (opsize32_) rm_write32(rm, fetch32()); else rm_write16(rm, fetch16()); c += 1; break; }
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
            // Idle detection: INT 16h AH=01h/11h (peek keystroke) and INT 2Fh
            // AX=1680h (release time slice) mean "nothing to do". The gap
            // between consecutive polls is credited as idle, but only when
            // short enough to be a tight loop.
            {
                const uint8_t ah = get_reg8(4);
                const bool key_poll = (n == 0x16 && (ah == 0x01 || ah == 0x11));
                const bool yield = (n == 0x2F && ah == 0x16 && get_reg8(0) == 0x80);
                if (key_poll || yield) {
                    const uint64_t gap = cycles - last_poll_cycle_;
                    if (gap < kIdlePollGap) idle_poll_cycles += gap;
                    last_poll_cycle_ = cycles;
                }
            }
            // INT n is IOPL-sensitive in V86 so the monitor can intercept 8086
            // OS calls (Intel 80386 PRM, "Emulating 8086 Operating System
            // Calls"). INT3 and INTO are exceptions and are exempt.
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
            // XLAT indexes (E)BX by AL; 0x67 selects EBX as the base.
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

        // --- Port I/O (published 486: IN 14, OUT 16) ---
        case 0xE4: { uint8_t p = fetch8(); check_io_permission(p, 1); set_reg8(0, bus_in(p)); c += 14; break; }
        case 0xE5: { uint8_t p = fetch8(); if (opsize32_) { check_io_permission(p, 4); eax = bus_in32(p); } else { check_io_permission(p, 2); set_reg16(0, bus_in16(p)); } c += 14; break; }
        case 0xE6: { uint8_t p = fetch8(); check_io_permission(p, 1); bus_out(p, get_reg8(0)); c += 16; break; }
        case 0xE7: { uint8_t p = fetch8(); if (opsize32_) { check_io_permission(p, 4); bus_out32(p, eax); } else { check_io_permission(p, 2); bus_out16(p, get_reg16(0)); } c += 16; break; }

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
        case 0xED:
            if (opsize32_) { check_io_permission(uint16_t(edx), 4); eax = bus_in32(uint16_t(edx)); }
            else { check_io_permission(uint16_t(edx), 2); set_reg16(0, bus_in16(uint16_t(edx))); }
            c += 14;
            break;
        case 0xEE: check_io_permission(uint16_t(edx), 1); bus_out(uint16_t(edx), get_reg8(0)); c += 16; break;
        case 0xEF:
            if (opsize32_) { check_io_permission(uint16_t(edx), 4); bus_out32(uint16_t(edx), eax); }
            else { check_io_permission(uint16_t(edx), 2); bus_out16(uint16_t(edx), get_reg16(0)); }
            c += 16;
            break;

        // ICEBP: #DB trap with no gate-DPL check, unlike INT 1, and no DR6
        // status bit (sandpile.org, "ICEBP").
        case 0xF1: do_interrupt(uint8_t(EXC_DB), false); c += 26; break;
        case 0xF4:
            // HLT is ring 0 only: #GP(0) at CPL > 0, V86 included. Delivered
            // without throw: JEMMEX catches EMMQXXX0's HLT pad (C800:001A) #GP
            // in a tight loop that soft-locks Chromium's wasm exception
            // handling.
            if (protected_mode() && cpl() != 0) {
                pending_fault_ = Fault{EXC_GP, 0, true};
                fault_pending_ = true;
                return 0;
            }
            halted = true;
            c += 4;
            break;
        case 0xF5: set_flag(FLAG_CF, !flag(FLAG_CF)); c += 2; break;  // CMC
        case 0xF6: case 0xF7: c += grp3_unary(op); break;
        case 0xF8: set_flag(FLAG_CF, false); c += 2; break;
        case 0xF9: set_flag(FLAG_CF, true); c += 2; break;
        // CLI/STI need CPL <= IOPL (Intel 80486 PRM, CLI/STI). Published 5 on
        // the 486.
        case 0xFA:
            if (protected_mode() && cpl() > iopl()) raise_err(EXC_GP, 0);
            set_flag(FLAG_IF, false);
            c += 5;
            break;
        case 0xFB:
            if (protected_mode() && cpl() > iopl()) raise_err(EXC_GP, 0);
            if (!flag(FLAG_IF)) shadow_ = true;
            set_flag(FLAG_IF, true);
            c += 5;
            break;
        case 0xFC: set_flag(FLAG_DF, false); c += 2; break;
        case 0xFD: set_flag(FLAG_DF, true); c += 2; break;
        case 0xFE: case 0xFF: c += grp5(op); break;

        case 0xD6:
            // SALC: AL = CF ? FFh : 00h, flags untouched. Undocumented, present
            // on every Intel part from the 8086; no published 486 timing, so
            // charged as SBB AL,AL (1 clock).
            set_reg8(0, flag(FLAG_CF) ? 0xFF : 0x00);
            c += 1;
            break;

        default:
            raise_ud(instr_start_eip_, op);
    }

    c += extra_cycles_;
    cycles += c;
    return c;
}

}  // namespace cpu80486
