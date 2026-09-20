// Intel 80486DX2-66 CPU core: real address mode, protected mode, paging
// and the on-die x87 FPU.
//
// This is a genuine Intel486 DX2 -- 33MHz external bus, clock-doubled to
// 66MHz internally, on-die FPU, 32 address lines. Unlike ibmpc-at's
// cpu80286.h, the 32-bit register file, the 0x66 operand-size prefix, the
// 0x67 address-size prefix with SIB-byte addressing, FS/GS, and the
// 0x0F-prefixed opcode space are all *native* 486 behavior here, not a
// labelled compatibility concession -- an Intel486 really does have EAX,
// FS, GS, BSWAP, XADD and CMPXCHG, and real-mode DOS code (DOS extender
// stubs, 32-bit-aware memory managers, anything built by a 386-targeting
// compiler in 16-bit mode) legally uses all of them without ever entering
// protected mode.
//
// Milestone 1 was real address mode exclusively. Milestone 2 (this file)
// adds the machinery a DOS extender needs, so the same core that boots
// FreeDOS in real mode can also run a 32-bit protected-mode program:
//
//   - Segmentation: a real GDT/LDT, 8-byte descriptors decoded into a
//     per-segment-register hidden cache (base / limit / type / DPL / D-B),
//     CPL and RPL privilege checks, and the #GP(selector) / #NP / #SS
//     faults that enforce them. LGDT/LIDT/LLDT/LTR/SGDT/SIDT/SLDT/STR/
//     LAR/LSL/VERR/VERW/ARPL/CLTS all do their real work.
//   - Mode switching: CR0.PE is genuinely honored, both directions. LMSW
//     and MOV CR0 enter protected mode; clearing PE returns to real mode.
//   - Paging: CR0.PG, CR3, the two-level page directory / page table walk,
//     A and D bit updates, U/S and R/W protection including the 486's own
//     new CR0.WP, a TLB with INVLPG and CR3-write flushes, and #PF with
//     CR2 and a real error code.
//   - Protected-mode interrupts: an IDT of task / interrupt / trap gates,
//     16- and 32-bit, with gate-DPL checks for software INT n, inter-
//     privilege stack switching through the TSS, and error-code pushes.
//   - Task switching: a real 32-bit (and 16-bit) TSS, hardware task
//     switches through a far JMP/CALL to a TSS or task gate, the busy bit,
//     the back-link plus EFLAGS.NT, and IRET's task-return path.
//   - x87 FPU: the 8-register 80-bit stack with its tag/status/control
//     words, the ESC opcode space 0xD8-0xDF, and the CR0.EM/TS/MP
//     coprocessor-emulation faults (#NM).
//
// Deliberately still unimplemented, as documented gaps rather than silent
// guesses:
//   - Virtual-8086 mode. EFLAGS.VM has storage and reads back, but the
//     core never enters V86 and the mode's own address translation, I/O
//     permission bitmap and #GP-to-monitor path are absent. FreeDOS's
//     JEMMEX wants this (PC486_REVIEW.md §5.9); a DOS extender does not.
//   - Debug registers DR0-DR7 round-trip as storage but no breakpoint,
//     single-step-on-branch or data watchpoint ever fires from them.
//   - Test registers TR3-TR7 (the 486's cache and TLB test interface) read
//     as 0 and discard writes: there is no cache model to test, and the
//     TLB model here is not the silicon's 4-way structure.
//   - Alignment-check faults (#AC). EFLAGS.AC and CR0.AM are both real
//     storage, but no unaligned access ever faults.
//
// Like cpu80286 and i8080::Cpu this core is host-agnostic: it reaches the
// outside world only through the Bus callbacks below. The address bus is
// 32 bits wide (a real Intel486 has 32 address lines) and Bus::read/write
// take a *physical* address -- the segment and page translations both
// happen inside this core. In real mode the CPU computes seg*16 + offset,
// which tops out at 0x10FFEF, and never masks that sum itself. The 1MB
// wraparound some 8086-era software relies on is the A20 gate's job --
// motherboard logic (the 8042's output-port bit on a genuine AT-compatible
// board), not CPU logic -- so it belongs in the embedding chipset's
// Bus::read/write, exactly as in cpu80286.h.
//
// Cycle counts are the 486 column of the Quantasm "80x86 Integer
// Instruction Set (8088 - Pentium)" timing table, which reproduces Intel's
// own i486 Programmer's Reference Manual instruction-timing appendix; flag
// and instruction semantics are from the Intel 80486 Programmer's
// Reference Manual (1990/1992) and, for the shared 8086-legacy subset, the
// Intel 8086/8088 User's Manual. Cited by instruction inline wherever a
// specific figure or quirk is being preserved rather than guessed at. The
// 486 is far more pipelined than the 286, and this core is an
// aggregate-cost-per-step() interpreter with no pipeline and no cache or
// prefetch *timing*: the page cache and prefetch window below are there to
// avoid repeating work, and charge nothing and save nothing in cycles
// (PC486_REVIEW.md §15) -- see cpu80486.cpp's header for exactly which published
// figures are single values, which are data-dependent ranges, and how each
// range is modelled.

#ifndef PC486_CPU80486_H
#define PC486_CPU80486_H

#include <cstdint>
#include <functional>
#include <type_traits>

namespace cpu80486 {

// EFLAGS bit positions. Bit 1 always reads 1; bits 3, 5, 15 are reserved
// and read 0.
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
    FLAG_RF   = 1u << 16,  // resume (debug); storage only, no breakpoints here
    FLAG_VM   = 1u << 17,  // virtual-8086 mode; storage only, never entered
    // Alignment Check. Genuinely new on the Intel486: AP-485's "Intel386
    // processor check" says "The AC bit, bit #18, is a new bit introduced
    // in the EFLAGS register on the Intel486 processor to generate
    // alignment faults. This bit cannot be set on the Intel386
    // processor." Before CPUID existed on early 486 steppings that
    // toggle-and-read-back was *the* documented way software told a 386
    // from a 486, so this core implements real storage for it (see
    // kPopfdMask in cpu80486.cpp). Alignment *faults* themselves are a
    // documented gap (see the header comment), so the bit is
    // observable-but-inert.
    FLAG_AC   = 1u << 18,
    // Identification. Settable exactly on the Intel486 parts that carry
    // CPUID, and AP-485 makes toggling it *the* documented way to ask:
    // "the ability to set and clear the ID flag (bit 21) in the EFLAGS
    // register indicates whether the processor supports the CPUID
    // instruction". FreeDOS 1.3's own `VINFO` runs that exact test, so
    // this bit decides whether this machine's DOS identifies its CPU at
    // all -- see PC486_REVIEW.md §13.
    FLAG_ID   = 1u << 21,
};

// CR0 (Intel 80486 PRM, "Control Registers"). ET is hardwired to 1 on an
// Intel486 -- the FPU is on-die, so there is no "is a coprocessor
// installed" question to answer.
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

// Architectural exception vectors (Intel 80486 PRM, "Interrupts and
// Exceptions"). Only the ones this core can actually raise are named.
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
};

// Memory and port I/O callbacks supplied by the embedding chipset. `addr`
// is a full *physical* address -- this core performs segment translation
// and, when CR0.PG is set, the page-table walk, so the chipset never sees
// a linear address. (The A20 gate is still the chipset's job, see the
// header comment.) `port` is the 16-bit x86 I/O-space address.
//
// in16/out16 exist because a single 16-bit port access is one atomic bus
// cycle on real hardware, which for most ISA devices is equivalent to two
// adjacent 8-bit accesses but is NOT for a device whose data register is
// inherently 16-bit at one port address (the IDE/ATA data register at
// 0x1F0 is exactly this -- 0x1F1 is the Error/Features register, not "the
// high byte of 0x1F0"). See wd1003.h.
//
// Plain function pointers plus one `ctx` pointer, deliberately not
// std::function: every guest memory byte the interpreter touches goes
// through `read`/`write`, so this is the most frequently crossed boundary
// in the whole emulator. std::function adds a second indirect call (through
// its type-erased __func thunk) and blocks inlining at every one of those
// accesses, which cost real throughput -- see PC486_REVIEW.md §8. Build one
// with Bus::For(host) rather than filling the members in by hand.
struct Bus {
    void *ctx = nullptr;
    uint8_t  (*read) (void *ctx, uint32_t addr)             = nullptr;
    void     (*write)(void *ctx, uint32_t addr, uint8_t v)  = nullptr;
    uint8_t  (*in)   (void *ctx, uint16_t port)             = nullptr;
    void     (*out)  (void *ctx, uint16_t port, uint8_t v)  = nullptr;
    uint16_t (*in16) (void *ctx, uint16_t port)             = nullptr;
    void     (*out16)(void *ctx, uint16_t port, uint16_t v) = nullptr;

    // Optional bulk-access path: resolves a 4KB physical page to a host
    // pointer (null when the page is a device window, unpopulated, or
    // write-protected ROM), plus the generation counter that says when an
    // earlier resolution has stopped being valid. A host that offers
    // page_host()/map_epoch() gets it; one that does not leaves these null
    // and every byte goes through read/write above, unchanged.
    uint8_t *(*page)    (void *ctx, uint32_t page_base, bool write) = nullptr;
    const uint32_t *map_epoch = nullptr;

    // Binds a host object that supplies the six bus operations under these
    // names. The thunks are capture-less lambdas, so each is a direct,
    // inlinable call to the host method.
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

// The decoded, cached "hidden" half of a segment register -- what real
// silicon loads from the descriptor and then keeps using until the
// selector is loaded again (Intel 80486 PRM, "Segment Registers"). The
// cache surviving a mode change is exactly what makes "unreal mode" work
// on real hardware, and this core reproduces that: see PC486_REVIEW.md
// §5.4 and §6.2.
// Descriptor access-byte (descriptor byte 5) decoding: P DPL S TYPE.
// Intel 80486 PRM, "Segment Descriptors". Here rather than in the .cpp with
// the rest of the family because seg_linear()'s inline fast path below needs
// them; the .cpp keeps the ones only it uses.
inline bool acc_code(uint8_t a)        { return (a & 0x18) == 0x18; }
inline bool acc_data(uint8_t a)        { return (a & 0x18) == 0x10; }
inline bool acc_readable(uint8_t a)    { return acc_code(a) ? (a & 0x02) != 0 : true; }
inline bool acc_writable(uint8_t a)    { return acc_data(a) && (a & 0x02) != 0; }
inline bool acc_expand_down(uint8_t a) { return acc_data(a) && (a & 0x04) != 0; }

struct SegDesc {
    uint32_t base  = 0;
    uint32_t limit = 0xFFFFu;   // byte-granular, already scaled by G
    uint8_t  access = 0x93;     // descriptor byte 5: P DPL S TYPE
    bool     big   = false;     // D/B: 32-bit code segment / 32-bit stack
    bool     null  = false;     // a null selector was loaded (DS/ES/FS/GS only)
    // The selector this cache was loaded from. Kept so that assigning one
    // of the public selector fields directly -- which a host or a test
    // legitimately does in real mode -- is recognized as the segment load
    // it is and re-derives the base (see refresh_real_bases()).
    uint16_t sel = 0;
};

// GDTR / IDTR, and the LDTR / TR register pair's cached descriptor.
struct DescTableReg {
    uint32_t base  = 0;
    uint16_t limit = 0xFFFFu;
};

// 80-bit extended-precision x87 datum, stored exactly as it appears in
// memory: 64-bit significand (explicit integer bit at bit 63) plus a
// 16-bit sign/exponent word. Keeping the architectural format rather than
// a host float means FLD m80 / FSTP m80 round-trips are bit-exact for
// denormals, infinities and NaN payloads alike.
struct Float80 {
    uint64_t significand = 0;
    uint16_t sign_exp    = 0;
};

// A fault in flight. Thrown by the translation and privilege checks and
// caught by step(), which then performs the real exception delivery. An
// x86 fault abandons a partially executed instruction and restarts it, so
// a non-local exit is the faithful mechanism, not a convenience -- the
// same structure Bochs uses (its longjmp-based BX_CPU_C::exception).
struct Fault {
    int      vector;
    uint32_t error;
    bool     has_error;
};

class Cpu {
public:
    // General registers, 32 bits wide because an Intel486 genuinely has
    // 32-bit general registers -- not a compatibility layer over a
    // narrower architectural register file the way cpu80286.h's uint32_t
    // AX/BX/... deliberately is. AX/AL/AH etc. are simply the low 16 (or
    // 8) bits of this same storage: writing AX never disturbs EAX's upper
    // half, and writing AL never disturbs AH, matching real hardware.
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    uint32_t esp = 0, ebp = 0, esi = 0, edi = 0;

    // Segment registers -- the *selector* halves. All six are real: FS and
    // GS are 386 additions that a 486 certainly has, reachable in real
    // mode through the 0x64 and 0x65 override prefixes and loadable with
    // MOV/POP/LFS/LGS. Each has a hidden descriptor cache alongside it
    // (see desc() below); in real mode the base simply tracks selector*16,
    // so writing one of these fields directly still behaves exactly as it
    // did before protected mode existed.
    uint16_t cs = 0xF000, ds = 0, es = 0, ss = 0, fs = 0, gs = 0;

    // Instruction pointer. Masked to 16 bits in a 16-bit code segment
    // (which real mode always is) and full-width in a 32-bit one.
    uint32_t eip = 0xFFF0;

    uint32_t eflags = FLAG_R1;

    bool     halted = false;
    uint64_t cycles = 0;   // total clock cycles executed (66MHz core clocks)

    // Diagnostic hook: called with (CS, EIP-of-opcode, opcode-word)
    // whenever step() reaches the "unimplemented opcode" path -- a
    // genuinely unrecognized single-byte opcode (opcode-word = 0x00xx) or
    // an unrecognized 0x0F sub-opcode (0x0Fxx). Empty by default (costs
    // nothing); set by a diagnostic harness to find real opcode-coverage
    // gaps by evidence instead of by guessing, the same way ibmpc-at's
    // core found its BIOS's 386-baseline assumptions.
    std::function<void(uint16_t cs, uint32_t eip, uint16_t opcode_word)> on_unimplemented;

    // Diagnostic hook for delivered faults: (vector, error code, CS,
    // EIP-of-faulting-instruction). Empty by default. A protected-mode
    // guest that goes wrong almost always does it by taking a fault it
    // did not expect, and without this the only symptom is the guest
    // vanishing into its own (or a nonexistent) handler -- the exact
    // shape of the §5.4 unreal-mode bug hunt, which had to be done with
    // an instruction ring buffer instead.
    std::function<void(int vector, uint32_t error, uint16_t cs, uint32_t eip)> on_fault;

    explicit Cpu(Bus bus) : bus_(std::move(bus)) { init_state(); }

    // Power-on/RESET state: CS:IP = F000:FFF0, the x86 reset vector,
    // unchanged from the 8086 through the 486 (Intel 80486 PRM,
    // "Processor Initialization").
    void reset();

    // Decode and execute exactly one instruction at CS:EIP, returning the
    // clock cycles consumed.
    int step();

    // Deliver a hardware or software interrupt. In real mode this is the
    // 8086 sequence -- push FLAGS, CS, IP (16-bit each: the interrupt-frame
    // width in real mode is a property of the *mode*, not of the CPU
    // generation, so a 486 in real mode pushes the same 6-byte frame an
    // 8086 does), clear IF and TF, then vector through the real-mode IVT
    // at physical address vector*4. In protected mode it goes through the
    // IDT gate for that vector, switching stacks through the TSS if the
    // gate is more privileged than CPL. Real mode has no gate or privilege
    // check, so it is the caller's job to honor flag(FLAG_IF) before
    // calling this for a maskable (as opposed to NMI) source. Wakes HLT.
    // Returns cycles consumed.
    int interrupt(uint8_t vector);

    bool flag(Flag f) const { return (eflags & f) != 0; }
    void set_flag(Flag f, bool on) { eflags = on ? (eflags | f) : (eflags & ~uint32_t(f)); }

    // --- mode and protection state (public: a diagnostic harness, and the
    // --- tests, need to see these) ---------------------------------------
    bool protected_mode() const { return (cr_[0] & CR0_PE) != 0; }
    bool paging_enabled() const { return (cr_[0] & CR0_PG) != 0; }
    // Current privilege level. CPL lives in an internal register loaded from
    // the code segment's descriptor every time CS is loaded -- it is NOT
    // simply the low two bits of whatever is in the CS field. The two are
    // identical inside a well-formed protected-mode environment, because a
    // protected-mode CS load always sets RPL = CPL, but real mode maintains
    // no such invariant: a real-mode CS load sets CPL to 0 whatever the
    // segment value's low bits happen to be. That distinction is load-bearing
    // -- see PC486_REVIEW.md §6.5 for the FreeDOS boot it broke.
    int  cpl() const { return protected_mode() ? int(cpl_) : 0; }
    uint32_t cr(int i) const { return cr_[i & 3]; }
    uint32_t dr(int i) const { return dr_[i & 7]; }
    const SegDesc &desc(int seg_index) const { return sd_[seg_index & 7]; }
    const DescTableReg &gdtr() const { return gdtr_; }
    const DescTableReg &idtr() const { return idtr_; }
    const DescTableReg &ldtr() const { return ldtr_; }
    uint16_t ldt_selector() const { return ldt_sel_; }
    uint16_t tr_selector() const { return tr_sel_; }
    const DescTableReg &tr() const { return tr_; }

    // --- x87 state (public for the same diagnostic reason) ---------------
    const Float80 &st(int i) const { return fpu_reg_[(fpu_top_ + i) & 7]; }
    uint16_t fpu_control() const { return fpu_cw_; }
    uint16_t fpu_status() const { return uint16_t((fpu_sw_ & ~0x3800u) | (uint16_t(fpu_top_) << 11)); }
    uint16_t fpu_tag() const { return fpu_tw_; }
    int fpu_top() const { return fpu_top_; }
    // The value of ST(i) as a host long double -- for tests and
    // diagnostics; the architectural state is always the Float80 above.
    long double st_value(int i) const;

    // Segment-register index encoding used by the ModR/M reg field for
    // MOV sreg and by the override prefixes.
    enum SegIndex { SEG_ES = 0, SEG_CS = 1, SEG_SS = 2, SEG_DS = 3, SEG_FS = 4, SEG_GS = 5 };

    // 8-bit half-register access by the 3-bit ModR/M reg/rm encoding
    // (0=AL,1=CL,2=DL,3=BL,4=AH,5=CH,6=DH,7=BH).
    uint8_t  get_reg8(int idx) const;
    void     set_reg8(int idx, uint8_t v);
    // 16-bit access by the 3-bit encoding (0=AX,1=CX,2=DX,3=BX,4=SP,
    // 5=BP,6=SI,7=DI). Preserves the register's upper 16 bits.
    uint16_t get_reg16(int idx) const;
    void     set_reg16(int idx, uint16_t v);
    // Same encoding, full 32-bit width (EAX/ECX/EDX/EBX/ESP/EBP/ESI/EDI).
    uint32_t get_reg32(int idx) const;
    void     set_reg32(int idx, uint32_t v);

private:
    Bus bus_;

    // The one place the ctx pointer is threaded through, so the interpreter
    // proper reads the same as it did when Bus held std::functions.
    uint8_t  bus_read (uint32_t a)              { return bus_.read(bus_.ctx, a); }
    void     bus_write(uint32_t a, uint8_t v)   { bus_.write(bus_.ctx, a, v); }
    uint8_t  bus_in   (uint16_t p)              { return bus_.in(bus_.ctx, p); }
    void     bus_out  (uint16_t p, uint8_t v)   { bus_.out(bus_.ctx, p, v); }
    uint16_t bus_in16 (uint16_t p)              { return bus_.in16(bus_.ctx, p); }
    void     bus_out16(uint16_t p, uint16_t v)  { bus_.out16(bus_.ctx, p, v); }

    void init_state();

    // Prefix state for the instruction being decoded; all reset at the
    // start of each step().
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
    // The internal CPL register. Written by every CS load and by nothing
    // else; see cpl() above for why it cannot be derived from the CS field.
    uint8_t      cpl_ = 0;

    // Control/debug registers. Unlike Milestone 1 these are live: CR0's PE
    // and PG bits switch the core's mode, CR2 holds the last page-fault
    // linear address and CR3 the page-directory base.
    uint32_t cr_[4] = {0, 0, 0, 0};
    uint32_t dr_[8] = {0, 0, 0, 0, 0, 0, 0, 0};

    bool code32() const { return sd_[SEG_CS].big; }
    bool stack32() const { return sd_[SEG_SS].big; }
    uint32_t ip_mask() const { return code32() ? 0xFFFFFFFFu : 0xFFFFu; }
    void set_ip(uint32_t v) { eip = v & ip_mask(); }

    // --- faults -----------------------------------------------------------
    // Raising a fault unwinds out of the partially executed instruction.
    [[noreturn]] void raise(int vector);
    [[noreturn]] void raise_err(int vector, uint32_t error);
    // #GP / #NP / #SS / #TS all take a selector-shaped error code: the
    // selector's index with the table and external bits, or 0 when no
    // specific selector is at fault (Intel 80486 PRM, "Error Code").
    [[noreturn]] void raise_sel(int vector, uint16_t selector);

    // --- descriptors ------------------------------------------------------
    struct RawDesc { uint32_t lo, hi; };
    RawDesc read_desc(uint16_t selector, int fault_vector);
    // Non-faulting descriptor read: returns false instead of raising when
    // the selector is null or outside its table. LAR/LSL/VERR/VERW report
    // failure in ZF rather than faulting, which is the whole point of them.
    bool probe_desc(uint16_t selector, RawDesc &out);
    static SegDesc decode_desc(const RawDesc &d);
    static uint32_t desc_base(const RawDesc &d);
    static uint32_t desc_limit(const RawDesc &d);
    // Sets the descriptor's Accessed bit in the table, as a real segment
    // load does.
    void set_accessed(uint16_t selector);
    // Loads a data/stack segment register the way MOV sreg / POP sreg /
    // LDS / LES / LFS / LGS / LSS do, with every protected-mode check.
    void load_seg(int seg_index, uint16_t selector);
    // Loads CS:EIP for a far transfer. `is_call` selects CALL semantics
    // (return address pushed, call gates may switch stack) over JMP. Both
    // return the published cycle cost of the path actually taken, which in
    // protected mode differs by an order of magnitude between a plain
    // segment load, a call gate and a task switch.
    int far_transfer(uint16_t selector, uint32_t offset, bool is_call);
    int far_return(uint32_t stack_adjust, bool is_iret);
    // Real-mode segment loads: base = selector*16, everything else kept.
    void load_seg_real(int seg_index, uint16_t selector);
    void refresh_real_bases();

    // --- task switching ---------------------------------------------------
    // `link` distinguishes a CALL/interrupt-driven switch (sets the new
    // task's back-link and EFLAGS.NT) from a JMP (does not), and IRET's
    // return-to-outer-task path.
    enum class TaskLink { Jmp, Call, Iret };
    void task_switch(uint16_t tss_selector, TaskLink link, bool has_error, uint32_t error);
    uint32_t read_tss_dword(uint32_t tss_base, uint32_t off);
    uint16_t read_tss_word(uint32_t tss_base, uint32_t off);

    // --- paging -----------------------------------------------------------
    // A 486 has a 32-entry, 4-way set-associative TLB. This is a
    // correctness-first direct-mapped model of the same idea (CLAUDE.md:
    // correctness over speed) -- it never returns a stale translation
    // because every CR3 write and every INVLPG flushes it, which is the
    // architecturally visible contract; the associativity itself is not
    // observable to software that follows that contract.
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
    // Which backing store a physical address belongs to -- RAM, ROM, the VGA
    // window, open bus -- is a property of its 4KB page, and the bus was
    // re-deciding it for every byte. This asks the bus once per page and
    // keeps the answer; `host == nullptr` is itself a cached answer, and
    // sends the access back through the per-byte thunks (which is what keeps
    // the VGA window, ROM write-protection and open bus behaving exactly as
    // before). Any change that could make a resolution wrong -- the A20 gate
    // moving above all -- bumps the bus's generation counter, which flushes
    // the whole cache before the next access uses it.
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
        return e.host != nullptr ? e.host + (phys & 0xFFFu) : nullptr;
    }

    // --- instruction prefetch ---------------------------------------------
    // A real 486 fills a prefetch queue from the code segment rather than
    // running the full logical -> linear -> physical path for every opcode,
    // prefix, displacement and immediate byte (Intel 80486 PRM, "Instruction
    // Prefetch"). This resolves the run of EIPs that share one page once and
    // reads the rest of the instruction -- and the instructions after it, for
    // as long as execution stays on that page -- straight out of it.
    //
    // The window is re-validated once per instruction against every piece of
    // state its EIP -> host-pointer mapping rests on: CS's cached descriptor,
    // CR0 (mode and paging), CR3, CPL, and a counter every TLB flush and
    // INVLPG bumps. Comparing those values, rather than clearing the window
    // at each site that might write them, is what makes it safe -- and it
    // leans on the same architectural contract the TLB does, that software
    // changing a page mapping issues an INVLPG or reloads CR3. A page-map
    // generation change clears the window too (see page_map_flush). Nothing
    // mid-instruction can move any of them: a CS load, a CR3 write or a mode
    // change ends the instruction it happens in.
    const uint8_t *pf_base_ = nullptr;  // host pointer for EIP == pf_lo_
    uint32_t pf_lo_ = 0, pf_hi_ = 0;    // the EIP range pf_base_ covers
    SegDesc  pf_cs_{};                  // CS's descriptor when the window was filled
    uint32_t pf_cr0_ = 0, pf_cr3_ = 0, pf_tlb_gen_ = 0, pf_map_epoch_ = 0;
    uint8_t  pf_cpl_ = 0;
    uint32_t tlb_gen_ = 0;              // bumped by tlb_flush() / tlb_invalidate()
    void prefetch_clear() { pf_lo_ = pf_hi_ = 0; pf_base_ = nullptr; }
    // Compares the same ten values §15 chose, but accumulates the
    // differences with XOR/OR into one test instead of ten short-circuiting
    // branches: this is the most frequently executed line in the emulator
    // (11.5% of a BOOM run before, PC486_REVIEW.md §16), and every one of
    // those values is an L1 hit, so the branches cost more than the loads.
    // bus_.map_epoch is non-null whenever the window is non-empty, because
    // prefetch_fill() refuses to fill without it.
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
    // Translates a linear address, raising #PF (with CR2 and the real
    // error code) if it cannot. `write` and `user` select the protection
    // check; `user` is CPL==3.
    // The TLB hit is the whole point of having a TLB: it is a tag compare
    // and a rights test, and the interpreter runs it on every byte of every
    // guest memory access, so it is inline here and only the two-level page
    // walk (and every fault) stays out of line. translate_slow() is entered
    // in exactly the cases this returns nothing for, and re-runs the same
    // hit test itself, so a stale cached entry still faults off its cached
    // rights rather than off a fresh walk.
    uint32_t translate(uint32_t linear, bool write, bool user) {
        if (!paging_enabled()) return linear;
        uint32_t vpn = linear >> 12;
        const TlbEntry &e = tlb_[vpn & (kTlbEntries - 1)];
        if (e.valid && e.tag == vpn && (!write || e.dirty)) {
            bool denied = (user && !(e.rights & 2)) ||
                          (write && !(e.rights & 1) && (user || (cr_[0] & CR0_WP)));
            if (!denied) return e.frame | (linear & 0x00000FFFu);
        }
        return translate_slow(linear, write, user);
    }
    uint32_t translate_slow(uint32_t linear, bool write, bool user);

    // --- memory access ----------------------------------------------------
    // Every guest memory access goes logical -> linear -> physical here.
    // `si` is a SEG_* index, not a selector value: with descriptor caches
    // the base depends on *which* segment register is in use, which a bare
    // selector cannot tell us.
    uint32_t seg_base(int si) const { return sd_[si & 7].base; }
    // Checks `off`..`off+size-1` against the segment's limit and access
    // rights, then returns the linear address. A no-op in real mode, where
    // there are no descriptors to enforce (see PC486_REVIEW.md §4.3/§5.4).
    // Same split as translate() above, and for the same reason: the check
    // an ordinary present, expand-up, correctly-typed segment passes is one
    // comparison, and it runs on every guest memory access. Everything else
    // -- real mode, a null or expand-down segment, a wrapped access, any
    // fault -- is seg_linear_slow()'s, which is the complete check and is
    // entered in exactly the cases this does not answer.
    uint32_t seg_linear(int si, uint32_t off, int size, bool write) {
        const SegDesc &s = sd_[si & 7];
        if (!protected_mode()) return s.base + off;  // no descriptors to enforce
        if (!s.null && !acc_expand_down(s.access)) {
            uint32_t last = off + uint32_t(size) - 1u;
            if (last >= off && last <= s.limit &&
                (write ? acc_writable(s.access) : acc_readable(s.access)))
                return s.base + off;
        }
        return seg_linear_slow(si, off, size, write);
    }
    uint32_t seg_linear_slow(int si, uint32_t off, int size, bool write);
    uint8_t  read8(int si, uint32_t off);
    void     write8(int si, uint32_t off, uint8_t v);
    uint16_t read16(int si, uint32_t off);
    void     write16(int si, uint32_t off, uint16_t v);
    uint32_t read32(int si, uint32_t off);
    void     write32(int si, uint32_t off, uint32_t v);
    uint64_t read64(int si, uint32_t off);
    void     write64(int si, uint32_t off, uint64_t v);
    // A real 486 checks the segment limit once per access and consults the
    // TLB once per page touched, not once per byte (Intel 80486 PRM,
    // "Segment Translation" / "Page Translation"). When the whole access is
    // one contiguous run of offsets inside a single page -- the overwhelming
    // common case -- one limit check and one page walk answer every byte, so
    // this resolves the physical address of byte 0 and the rest follow it.
    // Returns false for a run that wraps the segment, overflows 32 bits, or
    // straddles a page boundary; those fall back to the byte-at-a-time path,
    // which is what keeps a straddling access faulting where it should.
    bool     access_phys(int si, uint32_t off, int size, bool write, uint32_t &phys);

    // Physical-address accessors for the descriptor tables, page tables and
    // TSS, which are addressed linearly (and, for page tables, physically)
    // rather than through a segment.
    uint8_t  phys_read8(uint32_t a)  { return bus_read(a); }
    void     phys_write8(uint32_t a, uint8_t v) { bus_write(a, v); }
    uint32_t phys_read32(uint32_t a);
    void     phys_write32(uint32_t a, uint32_t v);
    uint16_t lin_read16(uint32_t linear, bool write_access);
    uint32_t lin_read32(uint32_t linear, bool write_access);
    void     lin_write16(uint32_t linear, uint16_t v);
    void     lin_write32(uint32_t linear, uint32_t v);

    // True when a multi-byte access starting at `off` must wrap back to
    // offset 0 of the same segment. That is what real mode's fixed 64KB
    // limit means on real hardware; a protected-mode segment has an actual
    // descriptor limit instead, and an offset a 32-bit addressing form
    // produced above 0FFFFh is the "unreal mode" case (PC486_REVIEW.md
    // §5.4), where there is nothing at 0FFFFh to wrap at.
    bool wraps_at_64k(uint32_t off) const { return !protected_mode() && off <= 0xFFFFu; }
    uint32_t seg_off(uint32_t base_off, uint32_t delta) const {
        return wraps_at_64k(base_off) ? uint32_t(uint16_t(base_off + delta)) : base_off + delta;
    }

    // Instruction fetch. The window (see pf_base_ below) answers when the
    // bytes are on the code page already resolved for this instruction;
    // everything else -- the first fetch of an instruction, a page crossing,
    // a segment-limit or paging fault, a code page the bus will not hand
    // over -- goes to the *_slow forms, which are the original full path.
    // fetch8 alone is always_inline: the prefix loop runs it on every opcode
    // byte and nothing gets inlined into step_inner() on the optimizer's own
    // budget. Forcing the 16- and 32-bit forms as well measured 1% *slower*
    // in wasm -- they have far more call sites, and the code growth cost more
    // than the calls did (PC486_REVIEW.md §16).
#define PC486_ALWAYS_INLINE __attribute__((always_inline)) inline
    PC486_ALWAYS_INLINE uint8_t fetch8() {
        if (eip >= pf_lo_ && eip < pf_hi_) {
            uint8_t v = pf_base_[eip - pf_lo_];
            eip = (eip + 1) & ip_mask();
            return v;
        }
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

    // Stack. The width follows SS's B bit -- 16-bit in real mode always
    // (there is no descriptor there that could make it otherwise), and
    // whatever the stack descriptor says in protected mode. A 16-bit stack
    // moves SP and wraps mod 64KB while leaving ESP's upper half alone,
    // true even for the 32-bit (0x66-prefixed) push/pop forms, which move
    // SP by 4 rather than 2.
    uint16_t sp() const { return uint16_t(esp); }
    void set_sp(uint16_t v) { esp = (esp & 0xFFFF0000u) | v; }
    void add_sp(int32_t delta);
    void     push16(uint16_t v);
    uint16_t pop16();
    void     push32(uint32_t v);
    uint32_t pop32();

    // --- ModR/M decode ----------------------------------------------------
    // A resolved operand: either a register (is_mem=false, index in `reg`)
    // or a memory location (is_mem=true, seg being a SEG_* index that
    // already combines any override with the default-segment rule --
    // BP/EBP/ESP-based addressing defaults to SS, everything else to DS).
    // Eight bytes, so it comes back from decode_modrm() in registers rather
    // than through a hidden return slot -- decode_modrm() runs on most
    // instructions, and the copy was showing up in the profile
    // (PC486_REVIEW.md §16).
    struct RM {
        // The effective address as computed, untruncated: the 16-bit
        // addressing forms already wrapped every intermediate sum mod 64KB,
        // so only the 32-bit forms can produce anything above 0FFFFh, and
        // when they do this core lets the full offset through rather than
        // narrowing it (the "unreal mode" reasoning in the flat 32-bit
        // accessors below, and PC486_REVIEW.md §5.4). LEA reads exactly this,
        // since it never touches memory and a 32-bit addressing form there is
        // 32-bit *arithmetic*.
        uint32_t off;       // valid when is_mem
        bool     is_mem;
        uint8_t  reg;       // valid when !is_mem
        uint8_t  seg;       // valid when is_mem -- a SEG_* index
    };
    // Decodes the ModR/M byte, any SIB byte, and any displacement starting
    // at CS:EIP, advancing EIP past all of them. Honors addrsize32_ for
    // the memory-operand forms.
    //
    // The two forms 32-bit compiled code is mostly made of -- a register
    // operand (mod == 3) and a plain base register with an optional
    // displacement -- are decoded here so the result stays in the caller's
    // registers; everything else (SIB, disp32-only, all of 16-bit
    // addressing) is decode_modrm_slow()'s, which is the original decoder.
    // §16 measured the out-of-line call at 14.8% of a BOOM run in wasm.
    // always_inline for the same reason rm_read8 and friends need it: the
    // optimizer will not inline anything else into step_inner() on its own.
    __attribute__((always_inline)) inline RM decode_modrm() {
        uint8_t modrm = fetch8();
        last_reg_ = (modrm >> 3) & 7;
        uint8_t mod = modrm & 0xC0;
        if (mod == 0xC0) {  // mod == 3: the operand is a register
            RM out;
            out.off = 0;
            out.is_mem = false;
            out.reg = uint8_t(modrm & 7);
            out.seg = SEG_ES;  // unused when !is_mem; 0, as the decoder has always left it
            return out;
        }
        // rm == 4 is a SIB byte and rm == 5 with mod == 0 is disp32 with no
        // base register; both need the full decoder.
        uint8_t rm = modrm & 7;
        if (addrsize32_ && rm != 4 && (rm != 5 || mod != 0)) {
            uint32_t ea = get_reg32(rm);
            if (mod == 0x40) ea += uint32_t(int32_t(int8_t(fetch8())));
            else if (mod == 0x80) ea += fetch32();
            RM out;
            out.off = ea;
            out.is_mem = true;
            out.reg = 0;
            // ESP- or EBP-based addressing defaults to SS (Intel 80486 PRM,
            // "Default Segment Attribute"); ESP took the SIB path above, so
            // EBP is the only base that lands here. No index register, so
            // the base+index+disp clock penalty cannot apply.
            out.seg = uint8_t(seg_override_ >= 0 ? seg_override_
                                                 : (rm == 5 ? int(SEG_SS) : int(SEG_DS)));
            return out;
        }
        return decode_modrm_slow(modrm);
    }
    // Entered only with mod != 3, and with the ModR/M byte already consumed.
    RM decode_modrm_slow(uint8_t modrm);
    // One branch each. Left to its own judgement the optimizer keeps these
    // out of line, because step_inner() is already far past the size its
    // inlining budget allows -- so a two-instruction helper was costing a
    // call in the shipped wasm build (PC486_REVIEW.md §16).
#define PC486_ALWAYS_INLINE __attribute__((always_inline)) inline
    PC486_ALWAYS_INLINE uint8_t  rm_read8(const RM &rm)              { return rm.is_mem ? read8(rm.seg, rm.off) : get_reg8(rm.reg); }
    PC486_ALWAYS_INLINE void     rm_write8(const RM &rm, uint8_t v)  { if (rm.is_mem) write8(rm.seg, rm.off, v); else set_reg8(rm.reg, v); }
    PC486_ALWAYS_INLINE uint16_t rm_read16(const RM &rm)             { return rm.is_mem ? read16(rm.seg, rm.off) : get_reg16(rm.reg); }
    PC486_ALWAYS_INLINE void     rm_write16(const RM &rm, uint16_t v){ if (rm.is_mem) write16(rm.seg, rm.off, v); else set_reg16(rm.reg, v); }
    PC486_ALWAYS_INLINE uint32_t rm_read32(const RM &rm)             { return rm.is_mem ? read32(rm.seg, rm.off) : get_reg32(rm.reg); }
    PC486_ALWAYS_INLINE void     rm_write32(const RM &rm, uint32_t v){ if (rm.is_mem) write32(rm.seg, rm.off, v); else set_reg32(rm.reg, v); }
#undef PC486_ALWAYS_INLINE

    // flag helpers
    void set_pzs8(uint8_t r);
    void set_pzs16(uint16_t r);
    void set_pzs32(uint32_t r);
    static bool parity_even(uint8_t v);

    // ALU primitives, 8/16/32-bit triples. All set CF/OF/AF/PF/ZF/SF per
    // the Intel 80486 PRM's flag-affected tables and return the result.
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
    // Published 486 cost of one ALU-group operation, which depends on both
    // where the destination is and whether the op writes a result back --
    // CMP/TEST only read memory (2), while ADD and friends read-modify-write
    // it (3). See cpu80486.cpp.
    static int alu_cost(int alu, bool dst_is_mem, bool any_mem);

    // shift/rotate group. `count` is already masked mod 32 as real silicon
    // does; the 486's barrel shifter makes the plain shifts/rotates
    // count-independent in time, while RCL/RCR are not (see grp2_shift()).
    uint8_t  shiftrot8(int op, uint8_t v, int count);
    uint16_t shiftrot16(int op, uint16_t v, int count);
    uint32_t shiftrot32(int op, uint32_t v, int count);
    // SHLD/SHRD (386+): double-precision shift, 0x0F 0xA4/0xA5/0xAC/0xAD.
    void shld(const RM &rm, int src_reg, int count, bool right);

    // Data-dependent cost models for instructions whose published 486 cost
    // is a range rather than a single figure. Each is fitted to both
    // published endpoints and to the mechanism Intel documents; see
    // cpu80486.cpp for the derivation and the exact ranges.
    static int mul_cost(uint32_t multiplier, int width_bits);
    static int bitscan_cost(int bits_examined, bool is_mem);
    static int rotate_carry_cost(int count, bool is_mem);

    // BCD adjust and misc single-purpose instructions, one method each.
    void daa(); void das(); void aaa(); void aas(); void aam(); void aad();
    void pusha(); void popa();
    void bound();
    void imul_imm(int dst_reg, const RM &rm, uint32_t imm);  // IMUL r,r/m,imm (16- or 32-bit per opsize32_); returns cost via mul_cost
    int  enter(); void leave();

    bool cond(int cc) const;      // Jcc/SETcc/LOOPcc condition-code evaluation (cc = opcode low nibble)
    void jcc_rel8(bool taken);

    // Helpers that compute their own cited cycle cost and return it,
    // because the real 486 cost inside each group diverges far too much
    // for one flat per-group constant: DIV r/m32 is 40 clocks against
    // TEST r/m32,imm32's 1-2 (Quantasm 486 column), and the REP-prefixed
    // string forms depend on the iteration count that only the helper
    // itself knows.
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

    // I/O privilege: an IN/OUT/INS/OUTS/CLI/STI is only permitted when
    // CPL <= IOPL, or when the TSS's I/O permission bitmap grants the port.
    void check_io_permission(uint16_t port, int size);
    int  iopl() const { return int((eflags & FLAG_IOPL) >> 12); }

    // Performs the interrupt sequence -- the real-mode IVT path or the
    // protected-mode IDT-gate path -- *without* charging any cycles, so
    // the public interrupt() and step()'s INT/INT3/INTO/fault paths can
    // each charge their own published cost exactly once. `software` marks
    // an INT n / INT3 / INTO, which is the only case where the gate's DPL
    // is checked against CPL.
    void do_interrupt(uint8_t vector, bool software = true, bool has_error = false, uint32_t error = 0);
    void real_mode_interrupt(uint8_t vector);
    void protected_mode_interrupt(uint8_t vector, bool software, bool has_error, uint32_t error);

    // Decodes and executes one instruction, letting any fault propagate.
    // step() wraps it so the fault can be delivered with the instruction's
    // own starting state restored.
    int  step_inner();
    // Performs real exception delivery for a fault step_inner() raised:
    // restores the restartable state, then vectors through the IVT or IDT,
    // escalating a fault-during-delivery to #DF and a fault during *that*
    // to shutdown.
    int  deliver_fault(const Fault &first, uint32_t start_eip);

    int  extra_cycles_ = 0;  // set during decode (the base+index+disp effective-address penalty) and added to the opcode's cost by step()
    int  last_reg_ = 0;      // ModR/M reg field from the most recent decode_modrm(), read by the opcode-group helpers that use it as an operation selector
    uint32_t instr_start_eip_ = 0;  // CS:EIP at the start of the instruction (post-prefixes), so a fault can restore EIP to the faulting instruction the way real hardware does
    uint32_t instr_start_esp_ = 0;  // ESP likewise: a fault must not leave half-pushed operands behind
    uint16_t instr_start_ss_ = 0;
    SegDesc  instr_start_ss_desc_;  // and its descriptor cache, in case the fault hit mid stack-switch

    // --- x87 FPU ----------------------------------------------------------
    Float80  fpu_reg_[8];
    int      fpu_top_ = 0;    // TOP field of the status word (0-7)
    uint16_t fpu_cw_ = 0x037F;  // control word: all exceptions masked, extended precision, round to nearest
    uint16_t fpu_sw_ = 0;       // status word, minus TOP (see fpu_status())
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
    void fpu_stack_fault(bool overflow);
    void fpu_compare(long double a, long double b, bool unordered_ok);
    long double fpu_round_to_precision(long double v) const;
    static Float80 to_float80(long double v);
    static long double from_float80(const Float80 &f);
};

// The general-register file in the order the ModR/M reg/rm field encodes it
// (0=EAX,1=ECX,2=EDX,3=EBX,4=ESP,5=EBP,6=ESI,7=EDI), which is not the order
// the fields are declared in. Pointer-to-data-member is a plain byte offset
// in the Itanium ABI both toolchains here use, so indexing this is a load and
// an add rather than the eight-way branch the switch it replaces compiled to
// -- the ModR/M field is different on nearly every instruction, so that
// branch never predicted (PC486_REVIEW.md §16).
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
// Writing a 16-bit sub-register never disturbs the upper 16 bits of the
// 32-bit register -- real hardware behavior.
PC486_ALWAYS_INLINE void Cpu::set_reg16(int idx, uint16_t v) {
    uint32_t &r = this->*detail::kGpr32[idx & 7];
    r = (r & 0xFFFF0000u) | v;
}
PC486_ALWAYS_INLINE uint8_t Cpu::get_reg8(int idx) const {
    return uint8_t((this->*detail::kGpr8[idx & 3]) >> ((idx & 4) << 1));
}
// Only the addressed byte changes; bits 8-31 (or 16-31 for AH/CH/DH/BH) are
// left alone.
PC486_ALWAYS_INLINE void Cpu::set_reg8(int idx, uint8_t v) {
    uint32_t &r = this->*detail::kGpr8[idx & 3];
    const int sh = (idx & 4) << 1;
    r = (r & ~(uint32_t(0xFFu) << sh)) | (uint32_t(v) << sh);
}

#undef PC486_ALWAYS_INLINE

}  // namespace cpu80486

#endif // PC486_CPU80486_H
