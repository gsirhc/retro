// Intel 80486 DX2-66 core: real mode, protected mode, paging, x87 FPU. The
// 32-bit register file, 0x66/0x67 prefixes, FS/GS and the 0F opcode space are
// native 486 behavior; real-mode DOS code legally uses them.
//
// Not implemented:
//   - V86 entry through a task gate (Windows 3.x Enhanced Mode). IRETD entry,
// which JEMMEX uses, works; task_switch() rejects a TSS with VM set.
//   - Test registers TR3-TR7 read 0 and discard writes.
//
// Bus::read/write take physical addresses. Real mode computes seg*16 + offset
// without masking, so the 1MB wrap is the A20 gate's job in the chipset.
//
// Cycle counts are the 486 column of the Quantasm 80x86 timing table (Intel
// i486 PRM timing appendix); semantics are from the Intel 80486 PRM and the
// 8086/8088 User's Manual. No pipeline or cache timing is modelled here; the
// page cache and prefetch window save work, not cycles (PC486_REVIEW.md §15).
// See cpu80486.cpp for the data-dependent ranges.

#ifndef PC486_CPU80486_H
#define PC486_CPU80486_H

#include "cache486.h"

#include <csetjmp>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <vector>

// Performance panel counters, compiled out unless PC486_PERF.
#ifdef PC486_PERF
#define PC486_PERF_BUMP(field) (++(field))
#else
#define PC486_PERF_BUMP(field) ((void)0)
#endif

namespace cpu80486 {

// EFLAGS bits. Bit 1 reads 1; bits 3, 5, 15 read 0.
enum Flag : uint32_t {
    FLAG_CF   = 1u << 0,   // carry
    FLAG_R1   = 1u << 1,   // reserved, always 1
    FLAG_PF   = 1u << 2,   // parity (low byte of result, even = 1)
    FLAG_AF   = 1u << 4,   // auxiliary carry (BCD)
    FLAG_ZF   = 1u << 6,   // zero
    FLAG_SF   = 1u << 7,   // sign
    FLAG_TF   = 1u << 8,   // trap (single-step)
    FLAG_IF   = 1u << 9,   // interrupt enable
    FLAG_DF   = 1u << 10,  // direction (string ops)
    FLAG_OF   = 1u << 11,  // overflow
    FLAG_IOPL = 3u << 12,  // I/O privilege level (2 bits)
    FLAG_NT   = 1u << 14,  // nested task -- set by a CALL-driven task switch
    FLAG_RF   = 1u << 16,  // resume: suppresses instruction breakpoints for one instruction
    // Loadable only by IRETD from CPL 0 or a task switch (Intel 80386 PRM,
    // "Entering and Leaving Virtual 8086 Mode"), so kIretdMask carries it and
    // kPopfMask does not.
    FLAG_VM   = 1u << 17,
    // New on the 486; AP-485 has software toggle it to tell a 486 from a 386.
    // See kPopfdMask.
    FLAG_AC   = 1u << 18,
    // Settable on parts with CPUID; AP-485's CPUID-presence test toggles it.
    FLAG_ID   = 1u << 21,
};

// CR0 (Intel 80486 PRM, "Control Registers"). ET is hardwired to 1.
enum Cr0Bit : uint32_t {
    CR0_PE = 1u << 0,   // protection enable
    CR0_MP = 1u << 1,   // monitor coprocessor (gates WAIT on TS)
    CR0_EM = 1u << 2,   // emulate coprocessor: every ESC opcode traps #NM
    CR0_TS = 1u << 3,   // task switched: first FPU op after a task switch traps #NM
    CR0_ET = 1u << 4,   // extension type -- fixed 1 on a 486
    CR0_NE = 1u << 5,   // numeric error: 1 = #MF, 0 = external FERR#/IRQ13
    CR0_WP = 1u << 16,  // write protect -- a genuine 486 addition (see paging)
    CR0_AM = 1u << 18,  // alignment mask
    CR0_NW = 1u << 29,  // not write-through
    CR0_CD = 1u << 30,  // cache disable
    CR0_PG = 1u << 31,  // paging enable
};

// Exception vectors (Intel 80486 PRM, "Interrupts and Exceptions").
enum Exception : int {
    EXC_DE = 0,   // divide error
    EXC_DB = 1,   // debug
    EXC_BP = 3,   // INT3 breakpoint
    EXC_OF = 4,   // INTO overflow
    EXC_BR = 5,   // BOUND range exceeded
    EXC_UD = 6,   // invalid opcode
    EXC_NM = 7,   // device not available (no math coprocessor)
    EXC_DF = 8,   // double fault
    EXC_TS = 10,  // invalid TSS
    EXC_NP = 11,  // segment not present
    EXC_SS = 12,  // stack fault
    EXC_GP = 13,  // general protection
    EXC_PF = 14,  // page fault
    EXC_MF = 16,  // x87 floating-point error
    EXC_AC = 17,  // alignment check
};

// Memory and port I/O callbacks from the chipset. `addr` is physical; the core
// does segment translation and paging. in16/out16 exist because a 16-bit port
// access is one bus cycle, which IDE's data register at 0x1F0 needs (wd1003.h).
// Plain function pointers rather than std::function to avoid a second indirect
// call per guest byte (PC486_REVIEW.md §8). Build with Bus::For(host).
struct Bus {
    void *ctx = nullptr;
    uint8_t  (*read) (void *ctx, uint32_t addr)             = nullptr;
    void     (*write)(void *ctx, uint32_t addr, uint8_t v)  = nullptr;
    uint8_t  (*in)   (void *ctx, uint16_t port)             = nullptr;
    void     (*out)  (void *ctx, uint16_t port, uint8_t v)  = nullptr;
    uint16_t (*in16) (void *ctx, uint16_t port)             = nullptr;
    void     (*out16)(void *ctx, uint16_t port, uint16_t v) = nullptr;

    // Optional bulk path: resolves a 4KB physical page to a host pointer (null
    // for device windows, unpopulated space, ROM writes) plus an epoch that
    // invalidates earlier resolutions. Hosts without it use read/write.
    uint8_t *(*page)    (void *ctx, uint32_t page_base, bool write) = nullptr;
    const uint32_t *map_epoch = nullptr;

    template <class T>
    static Bus For(T *host) {
        Bus b;
        b.ctx   = host;
        b.read  = [](void *c, uint32_t a) -> uint8_t { return static_cast<T *>(c)->mem_read(a); };
        b.write = [](void *c, uint32_t a, uint8_t v) { static_cast<T *>(c)->mem_write(a, v); };
        b.in    = [](void *c, uint16_t p) -> uint8_t { return static_cast<T *>(c)->io_in(p); };
        b.out   = [](void *c, uint16_t p, uint8_t v) { static_cast<T *>(c)->io_out(p, v); };
        b.in16  = [](void *c, uint16_t p) -> uint16_t { return static_cast<T *>(c)->io_in16(p); };
        b.out16 = [](void *c, uint16_t p, uint16_t v) { static_cast<T *>(c)->io_out16(p, v); };
        if constexpr (has_page_host<T>::value) {
            b.page = [](void *c, uint32_t pb, bool w) { return static_cast<T *>(c)->page_host(pb, w); };
            b.map_epoch = host->map_epoch();
        }
        return b;
    }

private:
    template <class T, class = void>
    struct has_page_host : std::false_type {};
    template <class T>
    struct has_page_host<T, std::void_t<decltype(std::declval<T &>().page_host(0u, false)),
                                        decltype(std::declval<T &>().map_epoch())>>
        : std::true_type {};
};

// Descriptor access-byte decoding (P DPL S TYPE), Intel 80486 PRM, "Segment
// Descriptors". Here because seg_linear()'s inline path needs them.
inline bool acc_code(uint8_t a)        { return (a & 0x18) == 0x18; }
inline bool acc_data(uint8_t a)        { return (a & 0x18) == 0x10; }
inline bool acc_readable(uint8_t a)    { return acc_code(a) ? (a & 0x02) != 0 : true; }
inline bool acc_writable(uint8_t a)    { return acc_data(a) && (a & 0x02) != 0; }
inline bool acc_expand_down(uint8_t a) { return acc_data(a) && (a & 0x04) != 0; }

// Hidden descriptor cache; outlives a mode change, which gives unreal mode
// (PC486_REVIEW.md §5.4).
struct SegDesc {
    uint32_t base  = 0;
    uint32_t limit = 0xFFFFu;   // byte-granular, already scaled by G
    uint8_t  access = 0x93;     // descriptor byte 5: P DPL S TYPE
    bool     big   = false;     // D/B: 32-bit code segment / 32-bit stack
    bool     null  = false;     // a null selector was loaded (DS/ES/FS/GS only)
    // Selector the cache was loaded from, so assigning a public selector field
    // re-derives the base (refresh_real_bases()).
    uint16_t sel = 0;
};

// GDTR / IDTR, and the LDTR / TR register pair's cached descriptor.
struct DescTableReg {
    uint32_t base  = 0;
    uint16_t limit = 0xFFFFu;
};

// 80-bit extended datum in memory format, so FLD/FSTP m80 round-trips
// bit-exact.
struct Float80 {
    uint64_t significand = 0;
    uint16_t sign_exp    = 0;
};

// A fault in flight; step() catches it and delivers the exception.
struct Fault {
    int      vector;
    uint32_t error;
    bool     has_error;
};

class Cpu {
public:
    // 32-bit registers; AX/AL are the low bits of the same storage.
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    uint32_t esp = 0, ebp = 0, esi = 0, edi = 0;

    // Segment selectors. FS and GS are reachable in real mode via 0x64/0x65.
    // Hidden caches are in desc().
    uint16_t cs = 0xF000, ds = 0, es = 0, ss = 0, fs = 0, gs = 0;

    // Masked to 16 bits in a 16-bit code segment.
    uint32_t eip = 0xFFF0;

    uint32_t eflags = FLAG_R1;
    // Cycles spent halted. Always counted; the front end's CPU load figure uses
    // it.
    uint64_t halt_cycles = 0;
    // Cycles spent in a recognizable DOS wait loop (see INT imm8). DOS idles by
    // polling INT 16h, so halted cycles alone read ~100% busy.
    uint64_t idle_poll_cycles = 0;
    uint64_t last_poll_cycle_ = 0;
    // Poll loops recur every few hundred to few thousand cycles; real work
    // between keyboard checks takes ~1.9M.
    static constexpr uint64_t kIdlePollGap = 50000;
#ifdef PC486_PERF
    struct Perf {
        uint64_t instrs, tlb_miss, fetch_slow, mmio;
        uint64_t opcode[256];
        uint64_t opcode0f[256];
    };
    Perf perf{};
#endif

    bool     halted = false;

    // True for one instruction after STI, MOV SS or POP SS (Intel 80486 PRM);
    // the machine checks it before delivering INTR.
    bool interrupt_shadow() const { return shadow_; }
    // Fault during double-fault delivery: shutdown cycle, which an AT board
    // turns into a reset. Stays set until reset().
    bool shutdown() const { return shutdown_; }
    uint64_t cycles = 0;   // total clock cycles executed (66MHz core clocks)
    // Board cache and bus timing (cache486.h). Null charges every access at the
    // L1-hit cost.
    pc486::Cache486 *timing = nullptr;
    // A period BIOS clears CR0.CD and NW during POST to turn the L1 on.
    void enable_cache() { cr_[0] &= ~uint32_t(CR0_CD | CR0_NW); }

    // A REP string instruction yields after this many cycles with EIP on its
    // first prefix, so devices and interrupts run between iterations. 0 runs to
    // the end.
    uint32_t rep_yield_cycles = 0;
    // Called before a reserved encoding raises #UD (CS, EIP of opcode, opcode
    // word).
    std::function<void(uint16_t cs, uint32_t eip, uint16_t opcode_word)> on_unimplemented;

    // Called for each delivered fault (vector, error code, CS, faulting EIP).
    std::function<void(int vector, uint32_t error, uint16_t cs, uint32_t eip)> on_fault;

    explicit Cpu(Bus bus) : bus_(std::move(bus)) { init_state(); }

    // Power-on state: CS:IP = F000:FFF0 (Intel 80486 PRM, "Processor
    // Initialization").
    void reset();

    // Executes one instruction at CS:EIP, returns cycles.
    int step();

    // Delivers an interrupt and wakes HLT; returns cycles. Real mode pushes the
    // 6-byte 8086 frame; protected mode goes through the IDT gate. The caller
    // checks IF for maskable sources.
    int interrupt(uint8_t vector);

    bool flag(Flag f) const { return (eflags & f) != 0; }
    void set_flag(Flag f, bool on) { eflags = on ? (eflags | f) : (eflags & ~uint32_t(f)); }

    // --- mode and protection state ---
    bool protected_mode() const { return (cr_[0] & CR0_PE) != 0; }
    bool paging_enabled() const { return (cr_[0] & CR0_PG) != 0; }
    // V86 is a submode of protected mode, so CR0.PE is part of the test.
    bool v86_mode() const { return (eflags & FLAG_VM) != 0 && (cr_[0] & CR0_PE) != 0; }
    // CPL is an internal register loaded on every CS load, not the CS RPL bits;
    // real mode sets it to 0 (PC486_REVIEW.md §6.5). Always 3 in V86 (Intel
    // 80386 PRM, "Additional Sensitive Instructions").
    int  cpl() const { return protected_mode() ? int(cpl_) : 0; }
    uint32_t cr(int i) const { return cr_[i & 3]; }
    uint32_t dr(int i) const { return read_dr(i & 7); }
    const SegDesc &desc(int seg_index) const { return sd_[seg_index & 7]; }
    const DescTableReg &gdtr() const { return gdtr_; }
    const DescTableReg &idtr() const { return idtr_; }
    const DescTableReg &ldtr() const { return ldtr_; }
    uint16_t ldt_selector() const { return ldt_sel_; }
    uint16_t tr_selector() const { return tr_sel_; }
    const DescTableReg &tr() const { return tr_; }

    // --- x87 state ---
    const Float80 &st(int i) const { return fpu_reg_[(fpu_top_ + i) & 7]; }
    uint16_t fpu_control() const { return fpu_cw_; }
    uint16_t fpu_status() const { return uint16_t((fpu_sw_ & ~0x3800u) | (uint16_t(fpu_top_) << 11)); }
    uint16_t fpu_tag() const { return fpu_tag_word(); }
    int fpu_top() const { return fpu_top_; }
    // ST(i) as a host long double, for tests and diagnostics.
    long double st_value(int i) const;

    // ModR/M sreg and override-prefix encoding.
    enum SegIndex { SEG_ES = 0, SEG_CS = 1, SEG_SS = 2, SEG_DS = 3, SEG_FS = 4, SEG_GS = 5 };

    // 8-bit half-register access by the 3-bit ModR/M reg/rm encoding
    // (0=AL,1=CL,2=DL,3=BL,4=AH,5=CH,6=DH,7=BH).
    uint8_t  get_reg8(int idx) const;
    void     set_reg8(int idx, uint8_t v);
    // 16-bit access by the 3-bit encoding (0=AX,1=CX,2=DX,3=BX,4=SP,
    // 5=BP,6=SI,7=DI). Preserves the register's upper 16 bits.
    uint16_t get_reg16(int idx) const;
    void     set_reg16(int idx, uint16_t v);
    uint32_t get_reg32(int idx) const;
    void     set_reg32(int idx, uint32_t v);

private:
    Bus bus_;

    uint8_t  bus_read (uint32_t a)              { return bus_.read(bus_.ctx, a); }
    void     bus_write(uint32_t a, uint8_t v)   { bus_.write(bus_.ctx, a, v); }
    uint8_t  bus_in   (uint16_t p)              { io_timing(p, 1, false); return bus_.in(bus_.ctx, p); }
    void     bus_out  (uint16_t p, uint8_t v)   { io_timing(p, 1, true); bus_.out(bus_.ctx, p, v); }
    uint16_t bus_in16 (uint16_t p)              { io_timing(p, 2, false); return bus_.in16(bus_.ctx, p); }
    void     bus_out16(uint16_t p, uint16_t v)  { io_timing(p, 2, true); bus_.out16(bus_.ctx, p, v); }
    // Bus stalls this instruction has run up, charged when it completes.
    uint32_t stall_ = 0;
    uint64_t now() const { return cycles + stall_; }
    void io_timing(uint16_t p, int size, bool write) {
        if (timing) stall_ += uint32_t(timing->io(p, size, write, now()));
    }
    __attribute__((always_inline)) void mem_timing(uint32_t phys, int size, bool write) {
        if (!timing) return;
        stall_ += uint32_t(write ? timing->write(phys, size, now())
                                 : timing->read(phys, size, fills(phys), now()));
    }
    void split_timing(const uint32_t *q, int n);
    bool split_ = false;   // inside a page-split read: its halves aren't misaligned again
    // The L1 fills unless CR0.CD is set or the page was mapped with PCD. Frames
    // mapped with PCD are a bitmap allocated on first use.
    std::vector<uint64_t> pcd_frames_;
    bool pcd_any_ = false;
    bool fills(uint32_t phys) const { return !(cr_[0] & CR0_CD) && (!pcd_any_ || !pcd(phys)); }
    bool pcd(uint32_t phys) const { return (pcd_frames_[phys >> 18] >> ((phys >> 12) & 63u)) & 1u; }
    void set_pcd(uint32_t frame, bool on) {
        if (!on && !pcd_any_) return;
        if (!pcd_any_) { pcd_frames_.assign(1u << 14, 0); pcd_any_ = true; }
        uint64_t bit = uint64_t(1) << ((frame >> 12) & 63u);
        if (on) pcd_frames_[frame >> 18] |= bit;
        else pcd_frames_[frame >> 18] &= ~bit;
    }
    int take_stall() {
        int s = int(stall_);
        stall_ = 0;
        cycles += uint64_t(s);
        return s;
    }
    // All devices are 16-bit, so BS16# runs a 32-bit I/O as two cycles, the
    // second at port + 2 (Intel486 Data Book, "Dynamic Bus Sizing").
    uint32_t bus_in32 (uint16_t p)              { return uint32_t(bus_in16(p)) | (uint32_t(bus_in16(uint16_t(p + 2))) << 16); }
    void     bus_out32(uint16_t p, uint32_t v)  { bus_out16(p, uint16_t(v)); bus_out16(uint16_t(p + 2), uint16_t(v >> 16)); }

    void init_state();

    // Prefix state for the current instruction, reset each step().
    int  seg_override_ = -1;   // -1 = none, else one of the SEG_* indices
    enum RepMode { REP_NONE, REP_Z, REP_NZ };
    RepMode rep_ = REP_NONE;
    bool opsize32_ = false;   // 32-bit operands: CS.D, toggled by 0x66
    bool addrsize32_ = false; // 32-bit addressing: CS.D, toggled by 0x67

    uint16_t &seg_reg(int idx);
    uint16_t  seg_sel(int idx) const;

    // --- protection / mode state ----------------------------------------
    SegDesc      sd_[8];        // hidden descriptor caches, indexed by SEG_*
    DescTableReg gdtr_, idtr_;
    DescTableReg ldtr_, tr_;    // the cached descriptors behind LDTR and TR
    uint16_t     ldt_sel_ = 0, tr_sel_ = 0;
    uint8_t      tr_access_ = 0;  // TR's descriptor type byte: 16- vs 32-bit TSS, busy bit
    // Internal CPL register, written only by CS loads; see cpl().
    uint8_t      cpl_ = 0;

    uint32_t cr_[4] = {0, 0, 0, 0};
    uint32_t dr_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t  dbg_exec_ = 0;   // DR7-enabled instruction breakpoints, bit n = DRn
    uint8_t  dbg_data_ = 0;   // DR7-enabled data breakpoints
    // A pending #DB trap: bits 0-3 are data breakpoints this instruction
    // hit, kDbgTask a task switch into a TSS with its T bit set.
    static constexpr uint8_t kDbgTask = 0x80;
    uint8_t  dbg_pending_ = 0;
    bool     ac_skip_ = false;   // set by ac_check_whole() for one instruction

    bool code32() const { return sd_[SEG_CS].big; }
    bool stack32() const { return sd_[SEG_SS].big; }
    uint32_t ip_mask() const { return code32() ? 0xFFFFFFFFu : 0xFFFFu; }
    void set_ip(uint32_t v) { eip = v & ip_mask(); }

    // --- faults -----------------------------------------------------------
    [[noreturn]] void raise(int vector);
    [[noreturn]] void raise_err(int vector, uint32_t error);
    // #GP/#NP/#SS/#TS error code: selector index plus table and external bits,
    // or 0 (Intel 80486 PRM, "Error Code").
    [[noreturn]] void raise_sel(int vector, uint16_t selector);
    // A reserved encoding: reports it through on_unimplemented, then #UD.
    [[noreturn]] void raise_ud(uint32_t at, uint16_t opword);

    // --- descriptors ------------------------------------------------------
    struct RawDesc { uint32_t lo, hi; };
    RawDesc read_desc(uint16_t selector, int fault_vector);
    // Returns false instead of faulting; LAR/LSL/VERR/VERW report failure in
    // ZF.
    bool probe_desc(uint16_t selector, RawDesc &out);
    static SegDesc decode_desc(const RawDesc &d);
    static uint32_t desc_base(const RawDesc &d);
    static uint32_t desc_limit(const RawDesc &d);
    void set_accessed(uint16_t selector);
    void load_seg(int seg_index, uint16_t selector);
    // is_call selects CALL semantics over JMP. Returns the cycle cost of the
    // path taken (segment load, call gate, or task switch).
    int far_transfer(uint16_t selector, uint32_t offset, bool is_call);
    int far_return(uint32_t stack_adjust, bool is_iret);
    void load_seg_real(int seg_index, uint16_t selector);
    void refresh_real_bases();

    // --- task switching ---------------------------------------------------
    // link: a CALL/interrupt switch sets the back-link and NT, JMP doesn't,
    // IRET is the return path.
    enum class TaskLink { Jmp, Call, Iret };
    void task_switch(uint16_t tss_selector, TaskLink link, bool has_error, uint32_t error);
    uint32_t read_tss_dword(uint32_t tss_base, uint32_t off);
    uint16_t read_tss_word(uint32_t tss_base, uint32_t off);

    // --- paging -----------------------------------------------------------
    // Direct-mapped stand-in for the 486's 32-entry 4-way TLB. Flushing on CR3
    // writes and INVLPG keeps it architecturally equivalent.
    static constexpr int kTlbEntries = 64;
    struct TlbEntry {
        uint32_t tag = 0;        // linear page number
        uint32_t frame = 0;      // physical page frame address
        uint8_t  rights = 0;     // bit0 writable, bit1 user-accessible
        bool     valid = false;
        bool     dirty = false;  // the PTE's D bit is already set
    };
    TlbEntry tlb_[kTlbEntries];
    void tlb_flush();
    void tlb_invalidate(uint32_t linear);

    // --- physical page -> host pointer ------------------------------------
    // A page's backing store (RAM, ROM, VGA window, open bus) is resolved once
    // per 4KB page and cached; host == nullptr sends the access through the
    // per-byte thunks. The bus bumps its epoch (A20 changes) to flush.
    static constexpr int kPageMapEntries = 64;
    struct PageMap {
        uint32_t tag = 0xFFFFFFFFu;   // physical page number, or ~0 for empty
        uint8_t *host = nullptr;
    };
    PageMap rmap_[kPageMapEntries], wmap_[kPageMapEntries];
    uint32_t map_epoch_seen_ = 0;
    void page_map_flush();
    uint8_t *host_ptr(uint32_t phys, bool write) {
        if (bus_.page == nullptr) return nullptr;
        if (map_epoch_seen_ != *bus_.map_epoch) page_map_flush();
        uint32_t pfn = phys >> 12;
        PageMap &e = (write ? wmap_ : rmap_)[pfn & (kPageMapEntries - 1)];
        if (e.tag != pfn) {
            e.host = bus_.page(bus_.ctx, phys & 0xFFFFF000u, write);
            e.tag = pfn;
        }
        if (e.host == nullptr) { PC486_PERF_BUMP(perf.mmio); return nullptr; }
        return e.host + (phys & 0xFFFu);
    }

    // The 486 fills a prefetch queue rather than translating every code byte
    // (Intel 80486 PRM, "Instruction Prefetch"). This caches a host pointer for
    // one code page.
    //
    // Revalidated each instruction against CS's descriptor, CR0, CR3, CPL and
    // the TLB and page-map generations. Relies on software issuing INVLPG or
    // reloading CR3 after changing a mapping. EFLAGS.VM needs no term: it only
    // changes with a CS reload.
    const uint8_t *pf_base_ = nullptr;  // host pointer for EIP == pf_lo_
    uint32_t pf_phys_ = 0;              // physical address for EIP == pf_lo_
    uint32_t pf_lo_ = 0, pf_hi_ = 0;    // the EIP range pf_base_ covers
    SegDesc  pf_cs_{};                  // CS's descriptor when the window was filled
    uint32_t pf_cr0_ = 0, pf_cr3_ = 0, pf_tlb_gen_ = 0, pf_map_epoch_ = 0;
    uint8_t  pf_cpl_ = 0;
    uint32_t tlb_gen_ = 0;              // bumped by tlb_flush() / tlb_invalidate()
    void prefetch_clear() { pf_lo_ = pf_hi_ = 0; pf_base_ = nullptr; }
    // One XOR/OR test instead of ten short-circuiting branches; this is the
    // hottest line in the emulator (PC486_REVIEW.md §16). bus_.map_epoch is
    // non-null whenever the window is non-empty.
    void prefetch_revalidate() {
        if (pf_lo_ == pf_hi_) return;
        const SegDesc &s = sd_[SEG_CS];
        uint32_t diff = (pf_cs_.base ^ s.base) | (pf_cs_.limit ^ s.limit) |
                        uint32_t(pf_cs_.access ^ s.access) |
                        uint32_t(pf_cs_.big != s.big) | uint32_t(pf_cs_.null != s.null) |
                        (pf_cr0_ ^ cr_[0]) | (pf_cr3_ ^ cr_[3]) |
                        uint32_t(pf_cpl_ ^ cpl_) | (pf_tlb_gen_ ^ tlb_gen_) |
                        (pf_map_epoch_ ^ *bus_.map_epoch);
        if (diff != 0) prefetch_clear();
    }
    void prefetch_fill();
    // Translates a linear address, raising #PF if it cannot. `user` is CPL==3.
    // The TLB hit is inline because it runs on every guest memory byte;
    // translate_slow() does the walk and every fault, rechecking cached rights
    // first so a stale entry faults off its rights.
    uint32_t translate(uint32_t linear, bool write, bool user) {
        if (!paging_enabled()) return linear;
        uint32_t vpn = linear >> 12;
        const TlbEntry &e = tlb_[vpn & (kTlbEntries - 1)];
        if (e.valid && e.tag == vpn && (!write || e.dirty)) {
            bool denied = (user && !(e.rights & 2)) ||
                          (write && !(e.rights & 1) && (user || (cr_[0] & CR0_WP)));
            if (!denied) return e.frame | (linear & 0x00000FFFu);
        }
        PC486_PERF_BUMP(perf.tlb_miss);
        return translate_slow(linear, write, user);
    }
    uint32_t translate_slow(uint32_t linear, bool write, bool user);

    // --- memory access ----------------------------------------------------
    // Logical -> linear -> physical. `si` is a SEG_* index, since the base
    // depends on which register.
    uint32_t seg_base(int si) const { return sd_[si & 7].base; }
    // 8086-style segmentation in real mode and V86: base = selector*16, 64KB
    // limit (Intel 80386 PRM, "Registers and Instructions"). Paging is
    // separate: CR0.PG alone gates translate().
    bool real_addressing() const { return !protected_mode() || (eflags & FLAG_VM) != 0; }
    // Checks off..off+size-1 against limit and access rights, returns the
    // linear address. Real mode and V86 check the cached limit only
    // (PC486_REVIEW.md §44). Inline fast path for an expand-up segment;
    // seg_linear_slow() does the rest. Forced inline because the compiler stops
    // inlining once read8/write8 grow.
    __attribute__((always_inline)) inline uint32_t seg_linear(int si, uint32_t off, int size, bool write) {
        const SegDesc &s = sd_[si & 7];
        if (real_addressing()) {  // the cached limit is the only check
            if (uint64_t(off) + uint32_t(size - 1) <= s.limit) return s.base + off;
            return seg_linear_slow(si, off, size, write);
        }
        if (!s.null && !acc_expand_down(s.access)) {
            uint32_t last = off + uint32_t(size) - 1u;
            if (last >= off && last <= s.limit &&
                (write ? acc_writable(s.access) : acc_readable(s.access)))
                return s.base + off;
        }
        return seg_linear_slow(si, off, size, write);
    }
    uint32_t seg_linear_slow(int si, uint32_t off, int size, bool write);
    // Misaligned CPL 3 access with CR0.AM and EFLAGS.AC raises #AC(0), after
    // the limit check and before paging (Intel 80486 PRM, "Alignment Check";
    // Bochs order).
    void ac_check(uint32_t lin, int size) {
        if ((cr_[0] & CR0_AM) && (lin & uint32_t(size - 1)) && (eflags & FLAG_AC) &&
            cpl() == 3 && !ac_skip_)
            raise_err(EXC_AC, 0);
    }
    // Whole-operand checks (FSAVE, SGDT) run once and skip per-access checks
    // for the step.
    void ac_check_whole(int si, uint32_t off, int align) {
        ac_check(sd_[si & 7].base + off, align);
        ac_skip_ = true;
    }
    // Nonzero while CR0.AM is set or a data breakpoint is armed.
    uint8_t access_hooks_ = 0;
    void update_access_hooks() { access_hooks_ = uint8_t(((cr_[0] & CR0_AM) ? 1 : 0) | (dbg_data_ ? 2 : 0)); }
    // Out of line and cold, so the access paths that test the flag stay small.
    __attribute__((noinline, cold)) void access_hooks(uint32_t lin, int size, bool write);
    void dbg_data_match(uint32_t lin, int size, bool write);
    uint8_t dbg_exec_match(uint32_t lin) const;
    void dr7_decode();
    void write_dr(int idx, uint32_t v);
    uint32_t read_dr(int idx) const;
    uint8_t  read8(int si, uint32_t off);
    void     write8(int si, uint32_t off, uint8_t v);
    uint16_t read16(int si, uint32_t off);
    void     write16(int si, uint32_t off, uint16_t v);
    uint32_t read32(int si, uint32_t off);
    void     write32(int si, uint32_t off, uint32_t v);
    uint64_t read64(int si, uint32_t off);
    void     write64(int si, uint32_t off, uint64_t v);
    // Resolves byte 0's physical address for an access inside one page, so one
    // limit check and one walk cover every byte (Intel 80486 PRM, "Segment
    // Translation"). Returns false for a wrap, 32-bit overflow or page
    // straddle, which take the per-byte path.
    bool     access_phys(int si, uint32_t off, int size, bool write, uint32_t &phys);

    // Physical accessors for descriptor tables, page tables and the TSS.
    uint8_t  phys_read8(uint32_t a)  { return bus_read(a); }
    void     phys_write8(uint32_t a, uint8_t v) { bus_write(a, v); }
    uint32_t phys_read32(uint32_t a);
    void     phys_write32(uint32_t a, uint32_t v);
    uint16_t lin_read16(uint32_t linear, bool write_access);
    uint32_t lin_read32(uint32_t linear, bool write_access);
    void     lin_write16(uint32_t linear, uint16_t v);
    void     lin_write32(uint32_t linear, uint32_t v);

    // The offset of a later part of a multi-byte operand. A 486 doesn't wrap
    // it at 64KB the way an 8086 did; the limit check faults instead.
    uint32_t seg_off(uint32_t base_off, uint32_t delta) const { return base_off + delta; }

    // Fetch from the prefetch window; a first fetch, page crossing, fault or
    // unmappable page goes to *_slow. Only fetch8 is forced inline: forcing the
    // wider forms measured 1% slower in wasm (PC486_REVIEW.md §16).
#define PC486_ALWAYS_INLINE __attribute__((always_inline)) inline
    PC486_ALWAYS_INLINE uint8_t fetch8() {
        if (eip >= pf_lo_ && eip < pf_hi_) {
            uint8_t v = pf_base_[eip - pf_lo_];
            eip = (eip + 1) & ip_mask();
            return v;
        }
        PC486_PERF_BUMP(perf.fetch_slow);
        return fetch8_slow();
    }
    uint16_t fetch16() {
        if (eip >= pf_lo_ && eip < pf_hi_ && pf_hi_ - eip >= 2u) {
            const uint8_t *p = pf_base_ + (eip - pf_lo_);
            eip = (eip + 2) & ip_mask();
            return uint16_t(uint32_t(p[0]) | (uint32_t(p[1]) << 8));
        }
        return fetch16_slow();
    }
    uint32_t fetch32() {
        if (eip >= pf_lo_ && eip < pf_hi_ && pf_hi_ - eip >= 4u) {
            const uint8_t *p = pf_base_ + (eip - pf_lo_);
            eip = (eip + 4) & ip_mask();
            return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                   (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        }
        return fetch32_slow();
    }
#undef PC486_ALWAYS_INLINE
    uint8_t  fetch8_slow();
    uint16_t fetch16_slow();
    uint32_t fetch32_slow();

    // Stack width follows SS.B (always 16-bit in real mode). A 16-bit stack
    // wraps SP mod 64KB and leaves ESP's upper half alone, including for 32-bit
    // pushes, which move SP by 4.
    uint16_t sp() const { return uint16_t(esp); }
    void set_sp(uint16_t v) { esp = (esp & 0xFFFF0000u) | v; }
    void add_sp(int32_t delta);
    void     push16(uint16_t v);
    uint16_t pop16();
    void     push32(uint32_t v);
    uint32_t pop32();

    // --- ModR/M decode ----------------------------------------------------
    // A resolved operand: register (index in reg) or memory (seg is a SEG_*
    // index with the override and default-segment rules applied; EBP/ESP bases
    // use SS). Eight bytes so it returns in registers (PC486_REVIEW.md §16).
    struct RM {
        // Effective address, untruncated: 32-bit forms may exceed 0FFFFh and
        // pass through (unreal mode, PC486_REVIEW.md §5.4). LEA reads this
        // directly.
        uint32_t off;       // valid when is_mem
        bool     is_mem;
        uint8_t  reg;       // valid when !is_mem
        uint8_t  seg;       // valid when is_mem -- a SEG_* index
        bool     disp;      // the address carried a displacement
    };
    // Decodes ModR/M, SIB and displacement at CS:EIP, advancing EIP. The
    // register and plain-base forms are inline so the result stays in
    // registers; the rest is decode_modrm_slow(). The out-of-line call was
    // 14.8% of a BOOM run in wasm (PC486_REVIEW.md §16).
    __attribute__((always_inline)) inline RM decode_modrm() {
        uint8_t modrm = fetch8();
        last_reg_ = (modrm >> 3) & 7;
        uint8_t mod = modrm & 0xC0;
        if (mod == 0xC0) {
            RM out;
            out.off = 0;
            out.is_mem = false;
            out.reg = uint8_t(modrm & 7);
            out.seg = SEG_ES;  // unused when !is_mem
            out.disp = false;
            return out;
        }
        // rm == 4 is SIB; rm == 5 with mod == 0 is disp32 with no base.
        uint8_t rm = modrm & 7;
        if (addrsize32_ && rm != 4 && (rm != 5 || mod != 0)) {
            if (agi(rm)) extra_cycles_ += 1;
            uint32_t ea = get_reg32(rm);
            if (mod == 0x40) ea += uint32_t(int32_t(int8_t(fetch8())));
            else if (mod == 0x80) ea += fetch32();
            RM out;
            out.off = ea;
            out.is_mem = true;
            out.reg = 0;
            // EBP base defaults to SS (Intel 80486 PRM, "Default Segment
            // Attribute"); ESP took the SIB path.
            out.seg = uint8_t(seg_override_ >= 0 ? seg_override_
                                                 : (rm == 5 ? int(SEG_SS) : int(SEG_DS)));
            out.disp = mod != 0;
            return out;
        }
        // SIB forms (7.2% of a BOOM run in wasm, PC486_REVIEW.md §16).
        if (addrsize32_ && rm == 4) {
            uint8_t sib = fetch8();
            uint8_t base = uint8_t(sib & 7);
            uint8_t index = uint8_t((sib >> 3) & 7);
            // base == 5 with mod == 0 is disp32 with no base; index == 4 is no
            // index.
            bool no_base = base == 5 && mod == 0;
            bool has_index = index != 4;
            bool has_disp = no_base;
            uint32_t ea = no_base ? 0 : get_reg32(base);
            if (has_index) ea += get_reg32(index) << ((sib >> 6) & 3);
            if (no_base) ea += fetch32();
            else if (mod == 0x40) { ea += uint32_t(int32_t(int8_t(fetch8()))); has_disp = true; }
            else if (mod == 0x80) { ea += fetch32(); has_disp = true; }
            // Base+index+displacement costs one extra clock, as in
            // decode_modrm_slow().
            if (!no_base && has_index && has_disp) extra_cycles_ += 1;
            if (!no_base && agi(base)) extra_cycles_ += 1;
            RM out;
            out.off = ea;
            out.is_mem = true;
            out.reg = 0;
            out.seg = uint8_t(seg_override_ >= 0 ? seg_override_
                              : (!no_base && (base == 4 || base == 5) ? int(SEG_SS) : int(SEG_DS)));
            out.disp = has_disp;
            return out;
        }
        return decode_modrm_slow(modrm);
    }
    // Entered only with mod != 3, and with the ModR/M byte already consumed.
    RM decode_modrm_slow(uint8_t modrm);
    // Forced inline; the optimizer won't inline into step_inner() on its own
    // (PC486_REVIEW.md §16).
#define PC486_ALWAYS_INLINE __attribute__((always_inline)) inline
    PC486_ALWAYS_INLINE uint8_t  rm_read8(const RM &rm)              { return rm.is_mem ? read8(rm.seg, rm.off) : get_reg8(rm.reg); }
    PC486_ALWAYS_INLINE void     rm_write8(const RM &rm, uint8_t v)  { if (rm.is_mem) write8(rm.seg, rm.off, v); else set_reg8(rm.reg, v); }
    PC486_ALWAYS_INLINE uint16_t rm_read16(const RM &rm)             { return rm.is_mem ? read16(rm.seg, rm.off) : get_reg16(rm.reg); }
    PC486_ALWAYS_INLINE void     rm_write16(const RM &rm, uint16_t v){ if (rm.is_mem) write16(rm.seg, rm.off, v); else set_reg16(rm.reg, v); }
    PC486_ALWAYS_INLINE uint32_t rm_read32(const RM &rm)             { return rm.is_mem ? read32(rm.seg, rm.off) : get_reg32(rm.reg); }
    PC486_ALWAYS_INLINE void     rm_write32(const RM &rm, uint32_t v){ if (rm.is_mem) write32(rm.seg, rm.off, v); else set_reg32(rm.reg, v); }
#undef PC486_ALWAYS_INLINE

    void set_pzs8(uint8_t r);
    void set_pzs16(uint16_t r);
    void set_pzs32(uint32_t r);
    static bool parity_even(uint8_t v);

    // ALU primitives set flags per the Intel 80486 PRM flag tables.
    uint8_t  add8(uint8_t a, uint8_t b, bool carry_in);
    uint16_t add16(uint16_t a, uint16_t b, bool carry_in);
    uint32_t add32(uint32_t a, uint32_t b, bool carry_in);
    uint8_t  sub8(uint8_t a, uint8_t b, bool borrow_in);
    uint16_t sub16(uint16_t a, uint16_t b, bool borrow_in);
    uint32_t sub32(uint32_t a, uint32_t b, bool borrow_in);
    uint8_t  and8(uint8_t a, uint8_t b);
    uint16_t and16(uint16_t a, uint16_t b);
    uint32_t and32(uint32_t a, uint32_t b);
    uint8_t  or8(uint8_t a, uint8_t b);
    uint16_t or16(uint16_t a, uint16_t b);
    uint32_t or32(uint32_t a, uint32_t b);
    uint8_t  xor8(uint8_t a, uint8_t b);
    uint16_t xor16(uint16_t a, uint16_t b);
    uint32_t xor32(uint32_t a, uint32_t b);

    uint8_t  alu_apply8(int alu, uint8_t a, uint8_t b);   // alu = ADD/OR/ADC/SBB/AND/SUB/XOR/CMP selector, 0-7
    uint16_t alu_apply16(int alu, uint16_t a, uint16_t b);
    uint32_t alu_apply32(int alu, uint32_t a, uint32_t b);
    // Published cost of one ALU-group op: CMP/TEST only read memory (2), ADD
    // etc. read-modify-write (3).
    static int alu_cost(int alu, bool dst_is_mem, bool any_mem);

    // count is pre-masked mod 32. Plain shifts and rotates are
    // count-independent on the barrel shifter; RCL/RCR are not.
    uint8_t  shiftrot8(int op, uint8_t v, int count);
    uint16_t shiftrot16(int op, uint16_t v, int count);
    uint32_t shiftrot32(int op, uint32_t v, int count);
    // SHLD/SHRD (386+): double-precision shift, 0x0F 0xA4/0xA5/0xAC/0xAD.
    void shld(const RM &rm, int src_reg, int count, bool right);

    // Cost models for instructions whose published 486 cost is a range; see
    // cpu80486.cpp.
    static int mul_cost(uint32_t multiplier, int width_bits);
    static int bitscan_cost(int bits_examined, bool is_mem);
    static int rotate_carry_cost(int count, bool is_mem);

    void daa(); void das(); void aaa(); void aas(); void aam(); void aad();
    void pusha(); void popa();
    void bound();
    void imul_imm(int dst_reg, const RM &rm, uint32_t imm);  // IMUL r,r/m,imm
    int  enter(); void leave();

    bool cond(int cc) const;      // Jcc/SETcc/LOOPcc condition-code evaluation (cc = opcode low nibble)
    void jcc_rel8(bool taken);

    // These compute their own cycle cost: it varies too much within a group for
    // one constant (DIV r/m32 is 40 clocks, TEST r/m32,imm32 is 1-2), and REP
    // costs depend on the count.
    int  loop_group(uint8_t op);   // 0xE0-0xE3: LOOPNE/LOOPE/LOOP/JCXZ
    int  string_op(uint8_t op);    // 0xA4-0xA7, 0xAA-0xAF: MOVS/CMPS/STOS/LODS/SCAS, honors REP/REPE/REPNE
    int  io_string_op(uint8_t op); // 0x6C-0x6F: INS/OUTS
    int  grp1_immed(uint8_t op);   // 0x80/0x81/0x82/0x83: ALU r/m,imm
    int  grp2_shift(uint8_t op);   // 0xC0/0xC1/0xD0-0xD3: shift/rotate group
    int  grp3_unary(uint8_t op);   // 0xF6/0xF7: TEST/NOT/NEG/MUL/IMUL/DIV/IDIV
    int  grp5(uint8_t op);         // 0xFE/0xFF: INC/DEC/CALL/JMP/PUSH r/m
    int  two_byte();               // the 0x0F escape space
    int  grp0f00();                // 0x0F 0x00: SLDT/STR/LLDT/LTR/VERR/VERW
    int  grp0f01();                // 0x0F 0x01: SGDT/SIDT/LGDT/LIDT/SMSW/LMSW/INVLPG
    int  mov_control_reg(uint8_t op2);
    void write_cr0(uint32_t v);

    // IN/OUT/INS/OUTS/CLI/STI need CPL <= IOPL or a TSS I/O bitmap grant.
    void check_io_permission(uint16_t port, int size);
    int  iopl() const { return int((eflags & FLAG_IOPL) >> 12); }

    // Runs the interrupt sequence without charging cycles, so each caller
    // charges its published cost once. software (INT n/INT3/INTO) is the only
    // case checked against the gate DPL.
    void do_interrupt(uint8_t vector, bool software = true, bool has_error = false, uint32_t error = 0);
    void real_mode_interrupt(uint8_t vector);
    void protected_mode_interrupt(uint8_t vector, bool software, bool has_error, uint32_t error);

    // Executes one instruction; faults propagate to step(), which delivers them
    // with the starting state restored.
    int  step_inner();
    // Delivers a fault from step_inner(): restores restartable state, vectors,
    // escalates to #DF then shutdown.
    int  deliver_fault(const Fault &first, uint32_t start_eip);
    // step()'s instruction-breakpoint and RF work: the cycles of a #DB fault
    // it delivered, or -1 to run the instruction.
    __attribute__((noinline, cold)) int step_debug(uint32_t start_eip);

    int  extra_cycles_ = 0;  // base+index+disp penalty, added to the opcode's cost by step()
    // Pipeline penalties with a board timing model (Embedded Intel486
    // Developer's Manual 27302101, 12.3.1): +1 for a base register the previous
    // instruction wrote (rule 4) and for a displacement with an immediate (rule
    // 8). Snapshots are the registers at the start of this and the previous
    // instruction, alternating.
    uint32_t agi_snap_[2][8] = {};
    int      agi_cur_ = 0;
    bool agi(int r) const { return agi_snap_[agi_cur_][r] != agi_snap_[agi_cur_ ^ 1][r]; }
    void agi_clear() { for (int i = 0; i < 8; ++i) agi_snap_[agi_cur_][i] = get_reg32(i); }
    void disp_imm(const RM &rm) { if (timing && rm.disp) extra_cycles_ += 1; }
    int  last_reg_ = 0;      // ModR/M reg field of the last decode_modrm(), an operation selector
    uint32_t step_start_eip_ = 0;   // EIP of the instruction's first prefix byte
    bool rep_resume_ = false;   // the last step yielded part-way through a REP
    bool rep_resumed_ = false;  // this step continues one, so its setup cost is paid
    // Armed for 4+ prefixes, the only way past the 15-byte limit; the slow
    // fetch path raises #GP(0).
    bool len_check_ = false;
    void arm_length_limit(int prefixes);
    void check_length(uint32_t bytes);
    bool lock_allowed(uint8_t op);
    uint16_t fpu_tag_word() const;
    void fpu_load_tag_word(uint16_t tw);
    uint32_t instr_start_eip_ = 0;  // CS:EIP after prefixes, restored on a fault
    uint32_t instr_start_esp_ = 0;  // ESP likewise
    uint16_t instr_start_ss_ = 0;
    SegDesc  instr_start_ss_desc_;  // and its descriptor cache

    // Wasm build: raise_* longjmps into fault_jmp_ instead of throwing, so a
    // tight V86 #GP loop can't soft-lock Chromium's wasm C++ EH. Native throws.
    bool     fault_pending_ = false;
    bool     shadow_ = false;
    bool     shutdown_ = false;
    bool     vectored_ = false;   // this step entered a handler through do_interrupt
    Fault    pending_fault_{};
    bool     fault_jmp_set_ = false;
    std::jmp_buf fault_jmp_{};

    // --- x87 FPU ----------------------------------------------------------
    Float80  fpu_reg_[8];
    int      fpu_top_ = 0;    // TOP field of the status word (0-7)
    uint16_t fpu_cw_ = 0x037F;  // control word: all exceptions masked, extended precision, round to nearest
    uint16_t fpu_sw_ = 0;       // status word, minus TOP
    uint16_t fpu_tw_ = 0xFFFF;  // tag word: all eight registers empty
    // The last ESC instruction's own operand pointers, which FSTENV/FSAVE
    // store so a #MF handler can find what faulted.
    uint32_t fpu_last_ip_ = 0, fpu_last_op_ = 0;
    uint16_t fpu_last_cs_ = 0, fpu_last_ds_ = 0, fpu_last_opcode_ = 0;

    int  esc_op(uint8_t op);          // the 0xD8-0xDF ESC space
    void fpu_init();
    void fpu_check_available();       // CR0.EM / CR0.TS -> #NM
    void fpu_push(long double v);
    long double fpu_pop();
    long double fpu_get(int i) const;
    void fpu_set(int i, long double v);
    void fpu_set_tag(int phys, bool empty);
    bool fpu_is_empty(int i) const;
    void fpu_xch(int i);
    void fpu_stack_fault(bool overflow);
    void fpu_compare(long double a, long double b, bool unordered_ok);
    long double fpu_round_to_precision(long double v) const;
    static Float80 to_float80(long double v);
    static long double from_float80(const Float80 &f);
};

// GPR file in ModR/M encoding order. Indexing a pointer-to-member is a load and
// add, replacing an eight-way switch that never predicted (PC486_REVIEW.md
// §16).
namespace detail {
inline constexpr uint32_t Cpu::*kGpr32[8] = {&Cpu::eax, &Cpu::ecx, &Cpu::edx, &Cpu::ebx,
                                             &Cpu::esp, &Cpu::ebp, &Cpu::esi, &Cpu::edi};
// The 8-bit encoding names the low or high byte of the first four: index & 3
// picks the register, bit 2 picks the byte (0=AL..3=BL, 4=AH..7=BH).
inline constexpr uint32_t Cpu::*kGpr8[4] = {&Cpu::eax, &Cpu::ecx, &Cpu::edx, &Cpu::ebx};
}  // namespace detail

#define PC486_ALWAYS_INLINE __attribute__((always_inline)) inline

PC486_ALWAYS_INLINE uint32_t Cpu::get_reg32(int idx) const { return this->*detail::kGpr32[idx & 7]; }
PC486_ALWAYS_INLINE void Cpu::set_reg32(int idx, uint32_t v) { this->*detail::kGpr32[idx & 7] = v; }
PC486_ALWAYS_INLINE uint16_t Cpu::get_reg16(int idx) const { return uint16_t(get_reg32(idx)); }
PC486_ALWAYS_INLINE void Cpu::set_reg16(int idx, uint16_t v) {
    uint32_t &r = this->*detail::kGpr32[idx & 7];
    r = (r & 0xFFFF0000u) | v;
}
PC486_ALWAYS_INLINE uint8_t Cpu::get_reg8(int idx) const {
    return uint8_t((this->*detail::kGpr8[idx & 3]) >> ((idx & 4) << 1));
}
PC486_ALWAYS_INLINE void Cpu::set_reg8(int idx, uint8_t v) {
    uint32_t &r = this->*detail::kGpr8[idx & 3];
    const int sh = (idx & 4) << 1;
    r = (r & ~(uint32_t(0xFFu) << sh)) | (uint32_t(v) << sh);
}

#undef PC486_ALWAYS_INLINE

}  // namespace cpu80486

#endif // PC486_CPU80486_H
