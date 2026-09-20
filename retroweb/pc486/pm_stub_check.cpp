// Native end-to-end proof that this core's protected mode, paging, gates,
// task switching and FPU actually work together -- Milestone 2's equivalent
// of Milestone 1's "boots the real FreeDOS installer to a live C:\>".
//
// It assembles, byte by byte, the smallest program that does what a
// DOS4GW-class extender does at startup, and runs it against the real
// pc486::Chipset (32MB of RAM, the same bus the browser build uses):
//
//   1. real mode: LGDT / LIDT, set CR0.PE, far-JMP into a 32-bit flat code
//      segment -- the canonical protected-mode entry sequence,
//   2. 32-bit protected mode: load the flat data selector, do 32-bit
//      arithmetic, and store to linear 7MB, which no real-mode addressing
//      form can reach,
//   3. x87: exact-value arithmetic (2.5 * 4.0, sqrt(16)) plus an integer
//      conversion, stored back to memory,
//   4. paging: load CR3, set CR0.PG, and write through a *virtual alias* --
//      linear 8MB mapped onto a physical frame at a completely different
//      address, so the value only lands correctly if the page walk is real,
//   5. #PF: touch an unmapped linear address on purpose. The handler reads
//      CR2 and the error code, installs a page table for it, INVLPGs, and
//      IRETDs -- so the faulting instruction *restarts* and succeeds. That
//      is demand paging, and it only works if faults are restartable.
//   6. task switching: LTR, then a far CALL to a TSS selector. The second
//      task runs on its own stack, clobbers EAX, and IRETDs back through
//      the back-link; EAX must come back intact from the first task's TSS.
//   7. privilege: build an IRETD frame by hand and return *outward* to ring
//      3, run there, come back through a DPL-3 interrupt gate (which
//      switches to the ring-0 stack out of the TSS), and go back out again.
//   8. exit: clear CR0.PG, drop to a 16-bit code segment, clear CR0.PE,
//      far-JMP back to a real-mode CS, and store one last marker -- the
//      round trip a DOS extender performs on every DOS call.
//
// Every step leaves an observable value in a results block at physical
// 0x7000, and main() checks all of them. Nothing here is asserted by the
// emulator about itself: the values are what the *guest program* computed.
//
// Usage: pm_stub_check [max_steps]

#include "chipset.h"
#include "cpu80486.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

// --- physical memory layout the stub is built around ---------------------
constexpr uint32_t kGdtPtr    = 0x00000500;  // 6-byte LGDT operand
constexpr uint32_t kIdtPtr    = 0x00000506;  // 6-byte LIDT operand
constexpr uint32_t kGdt       = 0x00001000;
constexpr uint32_t kIdt       = 0x00001100;
constexpr uint32_t kPageDir   = 0x00002000;
constexpr uint32_t kPageTab0  = 0x00003000;  // linear 0-4MB, identity
constexpr uint32_t kPageTab2  = 0x00004000;  // linear 8MB-12MB
constexpr uint32_t kTss1      = 0x00005000;
constexpr uint32_t kTss2      = 0x00006000;
constexpr uint32_t kResults   = 0x00007000;
constexpr uint32_t kAliasFrame = 0x00009000; // what linear 8MB maps onto
constexpr uint32_t kPfPageTab  = 0x0000A000; // the table the #PF handler installs
constexpr uint32_t kPfFrame    = 0x0000B000; // what linear 12MB ends up mapping onto
constexpr uint32_t kCode16Base = 0x00010000; // real-mode CS 1000h, and GDT selector 18h
constexpr uint32_t kCode32     = 0x00020000; // the 32-bit protected-mode code

// Linear addresses the stub deliberately touches.
constexpr uint32_t kFarStore   = 0x00700000;  // 7MB -- beyond any real-mode reach
constexpr uint32_t kAliasLin   = 0x00800000;  // 8MB -- mapped onto kAliasFrame
constexpr uint32_t kFaultLin   = 0x00C00000;  // 12MB -- unmapped until the #PF handler acts

// Result slots (offsets from kResults).
enum Result {
    R_ADD        = 0x00,  // 32-bit add result
    R_FMUL       = 0x08,  // 2.5 * 4.0 as a double
    R_FINT       = 0x10,  // the same, converted to an integer
    R_FSQRT      = 0x18,  // sqrt(16.0) as a double
    R_CR2        = 0x20,  // CR2 seen by the #PF handler
    R_PFERR      = 0x24,  // the #PF error code
    R_PFVALUE    = 0x28,  // what the restarted instruction read
    R_ALIAS      = 0x2C,  // read back through the virtual alias
    R_TASKEAX    = 0x30,  // EAX restored across the task switch
    R_TASK2      = 0x34,  // marker the second task wrote
    R_RING3      = 0x38,  // marker written at CPL 3
    R_HANDLERCS  = 0x3C,  // CS inside the DPL-3 gate handler
    R_FRAMECS    = 0x40,  // the interrupted CS the gate pushed
    R_REALMODE   = 0x44,  // marker written after returning to real mode
    R_FARSTORE   = 0x48,  // copy of what was stored at linear 7MB
};

// Expected values, all chosen to be exact.
constexpr uint32_t kAddResult   = 0x23456789u;  // 0x12345678 + 0x11111111
constexpr uint32_t kAliasMagic  = 0xCAFEBABEu;
constexpr uint32_t kPfMagic     = 0x600D600Du;
constexpr uint32_t kTaskEax     = 0x0BADF00Du;
constexpr uint32_t kTask2Magic  = 0x7A5C0DE5u;
constexpr uint32_t kRing3Magic  = 0x33334444u;
constexpr uint32_t kRealMagic   = 0x0000DEADu;

// --- a tiny two-pass assembler -------------------------------------------
// Labels resolve to *linear* addresses, which for the flat 32-bit segment
// are also physical ones. Two passes: the first computes label addresses
// with placeholder values, the second emits the real bytes.
struct Asm {
    std::vector<uint8_t> bytes;
    uint32_t origin = 0;
    std::map<std::string, uint32_t> *labels = nullptr;

    uint32_t here() const { return origin + uint32_t(bytes.size()); }
    void label(const char *name) { (*labels)[name] = here(); }
    uint32_t addr(const char *name) const {
        auto it = labels->find(name);
        return it == labels->end() ? 0u : it->second;
    }
    void db(int v) { bytes.push_back(uint8_t(v)); }
    void db(std::initializer_list<int> vs) { for (int v : vs) db(v); }
    void dw(uint32_t v) { db(int(v & 0xFF)); db(int((v >> 8) & 0xFF)); }
    void dd(uint32_t v) { dw(v & 0xFFFF); dw(v >> 16); }
    void dq(uint64_t v) { dd(uint32_t(v & 0xFFFFFFFFu)); dd(uint32_t(v >> 32)); }
    // The IEEE-754 bit pattern of a double, so the guest loads an exact value.
    void dq_double(double d) { uint64_t b; std::memcpy(&b, &d, 8); dq(b); }
};

// --- descriptor builders --------------------------------------------------
uint64_t seg_desc(uint32_t base, uint32_t limit, uint8_t access, bool big, bool granular) {
    uint32_t lim = granular ? (limit >> 12) : limit;
    uint64_t d = 0;
    d |= uint64_t(lim & 0xFFFFu);
    d |= uint64_t(base & 0xFFFFu) << 16;
    d |= uint64_t((base >> 16) & 0xFFu) << 32;
    d |= uint64_t(access) << 40;
    d |= uint64_t((lim >> 16) & 0x0Fu) << 48;
    if (big) d |= uint64_t(1) << 54;        // D/B
    if (granular) d |= uint64_t(1) << 55;   // G
    d |= uint64_t((base >> 24) & 0xFFu) << 56;
    return d;
}
uint64_t gate_desc(uint16_t selector, uint32_t offset, uint8_t access) {
    uint64_t d = 0;
    d |= uint64_t(offset & 0xFFFFu);
    d |= uint64_t(selector) << 16;
    d |= uint64_t(access) << 40;
    d |= uint64_t(offset >> 16) << 48;
    return d;
}

class Mem {
public:
    explicit Mem(pc486::Chipset &c) : c_(c) {}
    void w8(uint32_t a, uint8_t v) { c_.mem[a] = v; }
    void w16(uint32_t a, uint16_t v) { w8(a, uint8_t(v)); w8(a + 1, uint8_t(v >> 8)); }
    void w32(uint32_t a, uint32_t v) { w16(a, uint16_t(v)); w16(a + 2, uint16_t(v >> 16)); }
    void w64(uint32_t a, uint64_t v) { w32(a, uint32_t(v)); w32(a + 4, uint32_t(v >> 32)); }
    uint32_t r32(uint32_t a) const {
        return uint32_t(c_.mem[a]) | (uint32_t(c_.mem[a + 1]) << 8) |
               (uint32_t(c_.mem[a + 2]) << 16) | (uint32_t(c_.mem[a + 3]) << 24);
    }
    uint64_t r64(uint32_t a) const { return uint64_t(r32(a)) | (uint64_t(r32(a + 4)) << 32); }
    double rdouble(uint32_t a) const { uint64_t b = r64(a); double d; std::memcpy(&d, &b, 8); return d; }
private:
    pc486::Chipset &c_;
};

// Emits the 32-bit protected-mode body. Called twice (see Asm).
void emit_pm32(Asm &a) {
    const uint32_t res = kResults;

    a.label("pm_entry");
    // Load the flat 32-bit data selector into DS/ES/SS and set up a stack.
    // No 0x66 prefixes anywhere below: in a D=1 code segment 32-bit
    // operands are the *default*, which is itself a live check on the
    // CS.D-driven operand-size decode.
    a.db({0x66, 0xB8}); a.dw(0x0010);            // mov ax,0x10
    a.db({0x8E, 0xD8});                          // mov ds,ax
    a.db({0x8E, 0xC0});                          // mov es,ax
    a.db({0x8E, 0xD0});                          // mov ss,ax
    a.db({0xBC}); a.dd(0x0000FF00u);             // mov esp,0xFF00

    // --- step 2: 32-bit arithmetic and a store past 1MB -------------------
    a.db({0xB8}); a.dd(0x12345678u);             // mov eax,0x12345678
    a.db({0xBB}); a.dd(0x11111111u);             // mov ebx,0x11111111
    a.db({0x01, 0xD8});                          // add eax,ebx
    a.db({0xA3}); a.dd(kFarStore);               // mov [7MB],eax
    a.db({0xA3}); a.dd(res + R_ADD);             // mov [res+ADD],eax
    a.db({0xA1}); a.dd(kFarStore);               // mov eax,[7MB]
    a.db({0xA3}); a.dd(res + R_FARSTORE);        // mov [res+FARSTORE],eax

    // --- step 3: the on-die FPU, on exactly representable values ----------
    a.db({0xDB, 0xE3});                          // fninit
    a.db({0xDD, 0x05}); a.dd(a.addr("c_2_5"));   // fld qword [c_2_5]
    a.db({0xDD, 0x05}); a.dd(a.addr("c_4_0"));   // fld qword [c_4_0]
    a.db({0xDE, 0xC9});                          // fmulp st(1),st  -> 10.0
    a.db({0xDD, 0x15}); a.dd(res + R_FMUL);      // fst qword [res+FMUL]
    a.db({0xDB, 0x1D}); a.dd(res + R_FINT);      // fistp dword [res+FINT]
    a.db({0xDD, 0x05}); a.dd(a.addr("c_16_0"));  // fld qword [c_16_0]
    a.db({0xD9, 0xFA});                          // fsqrt -> 4.0
    a.db({0xDD, 0x1D}); a.dd(res + R_FSQRT);     // fstp qword [res+FSQRT]

    // --- step 4: turn on paging and write through a virtual alias ---------
    a.db({0xB8}); a.dd(kPageDir);                // mov eax,pagedir
    a.db({0x0F, 0x22, 0xD8});                    // mov cr3,eax
    a.db({0x0F, 0x20, 0xC0});                    // mov eax,cr0
    a.db({0x0D}); a.dd(0x80000000u);             // or eax,0x80000000
    a.db({0x0F, 0x22, 0xC0});                    // mov cr0,eax  -- paging live
    a.db({0xB8}); a.dd(kAliasMagic);             // mov eax,magic
    a.db({0xA3}); a.dd(kAliasLin);               // mov [8MB],eax   (virtual alias)
    a.db({0xA1}); a.dd(kAliasLin);               // mov eax,[8MB]
    a.db({0xA3}); a.dd(res + R_ALIAS);           // mov [res+ALIAS],eax

    // --- step 5: a deliberate page fault that the handler repairs ---------
    a.db({0xA1}); a.dd(kFaultLin);               // mov eax,[12MB]  -> #PF, restarts
    a.db({0xA3}); a.dd(res + R_PFVALUE);         // mov [res+PFVALUE],eax

    // --- step 6: LTR, then a task switch through a far CALL to a TSS ------
    a.db({0x0F, 0x00, 0x1D}); a.dd(a.addr("tss1_sel"));  // ltr [tss1_sel]
    a.db({0xB8}); a.dd(kTaskEax);                // mov eax,0x0BADF00D
    a.db({0x66, 0xFF, 0x1D}); a.dd(a.addr("tss2_far"));  // call far [tss2_far] (16-bit ptr form)
    a.db({0xA3}); a.dd(res + R_TASKEAX);         // mov [res+TASKEAX],eax

    // --- step 7: return outward to ring 3 with a hand-built IRETD frame ---
    a.db({0x68}); a.dd(0x0000003Bu);             // push 0x3B      (ring-3 SS)
    a.db({0x68}); a.dd(0x0000F200u);             // push 0xF200    (ring-3 ESP)
    a.db({0x68}); a.dd(0x00000202u);             // push 0x202     (EFLAGS, IF set)
    a.db({0x68}); a.dd(0x00000033u);             // push 0x33      (ring-3 CS)
    a.db({0x68}); a.dd(a.addr("ring3_entry"));   // push ring3_entry
    a.db({0xCF});                                // iretd -> CPL 3

    a.label("ring3_entry");
    // An outward return nulls any segment register the outer level may not
    // use, so DS/ES have to be reloaded here -- which is itself the check.
    a.db({0x66, 0xB8}); a.dw(0x003B);            // mov ax,0x3B
    a.db({0x8E, 0xD8});                          // mov ds,ax
    a.db({0x8E, 0xC0});                          // mov es,ax
    a.db({0xB8}); a.dd(kRing3Magic);             // mov eax,magic
    a.db({0xA3}); a.dd(res + R_RING3);           // mov [res+RING3],eax  (a CPL-3 store)
    a.db({0xCD, 0x20});                          // int 0x20  -> DPL-3 gate to ring 0
    a.db({0xCD, 0x21});                          // int 0x21  -> one-way trip back

    // --- step 8: leave protected mode the way a DOS extender does ---------
    a.label("pm_done");
    a.db({0x0F, 0x20, 0xC0});                    // mov eax,cr0
    a.db({0x25}); a.dd(0x7FFFFFFFu);             // and eax,0x7FFFFFFF  -- PG off first
    a.db({0x0F, 0x22, 0xC0});                    // mov cr0,eax
    // Drop into a 16-bit code segment *before* clearing PE, so CS.D is
    // already 0 when real mode resumes.
    // The 0x66 prefix here *narrows* the offset to 16 bits, because in a
    // D=1 segment 32 bits is the default -- the same toggle, read the other
    // way round.
    a.db({0x66, 0xEA}); a.dw(a.addr("back16") - kCode16Base); a.dw(0x0018);  // jmp far 0x18:back16

    // --- the #PF handler --------------------------------------------------
    a.label("pf_handler");
    a.db({0x0F, 0x20, 0xD0});                    // mov eax,cr2
    a.db({0xA3}); a.dd(res + R_CR2);             // mov [res+CR2],eax
    a.db({0x8B, 0x04, 0x24});                    // mov eax,[esp]   (error code)
    a.db({0xA3}); a.dd(res + R_PFERR);           // mov [res+PFERR],eax
    // Install a page directory entry and page table for linear 12MB.
    a.db({0xC7, 0x05}); a.dd(kPageDir + 3 * 4); a.dd(kPfPageTab | 3u);  // mov dword [pd+12],tab|RW|P
    a.db({0xC7, 0x05}); a.dd(kPfPageTab); a.dd(kPfFrame | 3u);          // mov dword [tab],frame|RW|P
    a.db({0x0F, 0x01, 0x3D}); a.dd(kFaultLin);   // invlpg [12MB]
    a.db({0x83, 0xC4, 0x04});                    // add esp,4  (drop the error code)
    a.db({0xCF});                                // iretd -> the faulting insn restarts

    // --- the DPL-3 gate handler (INT 20h): ring 3 -> ring 0 and back ------
    a.label("int20_handler");
    a.db({0x31, 0xC0});                          // xor eax,eax
    a.db({0xA3}); a.dd(res + R_HANDLERCS);       // mov [res+HANDLERCS],eax  (zero it)
    a.db({0x8C, 0x0D}); a.dd(res + R_HANDLERCS); // mov [res+HANDLERCS],cs   (must be ring 0)
    // With a privilege change the gate pushed SS:ESP too, so the frame is
    // EIP, CS, EFLAGS, ESP, SS -- [esp+4] is the interrupted CS.
    a.db({0x8B, 0x44, 0x24, 0x04});              // mov eax,[esp+4]
    a.db({0xA3}); a.dd(res + R_FRAMECS);         // mov [res+FRAMECS],eax
    a.db({0xCF});                                // iretd -> back out to ring 3

    // --- the INT 21h handler: a one-way return to the ring-0 main flow ----
    a.label("int21_handler");
    a.db({0xBC}); a.dd(0x0000FF00u);             // mov esp,0xFF00
    a.db({0xE9}); a.dd(a.addr("pm_done") - (a.here() + 4));  // jmp pm_done

    // --- the second task -------------------------------------------------
    a.label("task2_entry");
    a.db({0xB8}); a.dd(kTask2Magic);             // mov eax,magic
    a.db({0xA3}); a.dd(res + R_TASK2);           // mov [res+TASK2],eax
    a.db({0xB8}); a.dd(0x22222222u);             // mov eax,0x22222222 -- clobber EAX
    a.db({0xCF});                                // iretd -- NT is set, so this is a task return

    // --- constants and in-memory operands --------------------------------
    while (a.bytes.size() % 8) a.db(0x90);
    a.label("c_2_5");   a.dq_double(2.5);
    a.label("c_4_0");   a.dq_double(4.0);
    a.label("c_16_0");  a.dq_double(16.0);
    a.label("tss1_sel"); a.dw(0x0020);           // LTR's memory operand
    a.dw(0);
    a.label("tss2_far"); a.dw(0x0000); a.dw(0x0028);  // CALL FAR m16:16 -> TSS 2
}

// Emits the 16-bit code that lives at physical kCode16Base: the real-mode
// entry sequence, and the 16-bit protected-mode stub that drops PE.
void emit_code16(Asm &a) {
    a.label("real_entry");
    a.db({0xFA});                                // cli
    a.db({0x31, 0xC0});                          // xor ax,ax
    a.db({0x8E, 0xD8});                          // mov ds,ax
    // Open the A20 gate through the 8042, exactly as period software must:
    // the gate is closed at power-on, and with it closed the motherboard
    // masks every address to 20 bits, so extended memory is unreachable no
    // matter what the CPU computes. Command D1h then DFh to the keyboard
    // controller's output port is the canonical sequence. (This is chipset
    // behavior, not CPU behavior -- cpu80486.h's header says so -- and
    // leaving it out is how this stub first "passed" a store to linear 7MB
    // that had actually landed at 0.)
    a.db({0xB0, 0xD1});                          // mov al,0xD1
    a.db({0xE6, 0x64});                          // out 0x64,al
    a.db({0xB0, 0xDF});                          // mov al,0xDF
    a.db({0xE6, 0x60});                          // out 0x60,al
    a.db({0x0F, 0x01, 0x16}); a.dw(kGdtPtr);     // lgdt [0500h]
    a.db({0x0F, 0x01, 0x1E}); a.dw(kIdtPtr);     // lidt [0506h]
    a.db({0x0F, 0x20, 0xC0});                    // mov eax,cr0
    a.db({0x0C, 0x01});                          // or al,1        -- set PE
    a.db({0x0F, 0x22, 0xC0});                    // mov cr0,eax
    // A 32-bit far jump: the 0x66 prefix makes the offset 32 bits wide,
    // which is the only way a 16-bit segment can reach a 32-bit entry point.
    a.db({0x66, 0xEA}); a.dd(kCode32); a.dw(0x0008);  // jmp far 0x08:pm_entry

    a.label("back16");                           // entered as 0x18:offset
    a.db({0x0F, 0x20, 0xC0});                    // mov eax,cr0
    a.db({0x24, 0xFE});                          // and al,0xFE    -- clear PE
    a.db({0x0F, 0x22, 0xC0});                    // mov cr0,eax
    a.db({0xEA}); a.dw(a.addr("back_real") - kCode16Base); a.dw(0x1000);  // jmp far 1000h:back_real

    a.label("back_real");
    a.db({0x31, 0xC0});                          // xor ax,ax
    a.db({0x8E, 0xD8});                          // mov ds,ax
    a.db({0xC7, 0x06}); a.dw(kResults + R_REALMODE); a.dw(uint16_t(kRealMagic));  // mov word [res],0xDEAD
    a.db({0xF4});                                // hlt
}

template <typename F>
std::vector<uint8_t> assemble(uint32_t origin, F emit, std::map<std::string, uint32_t> &labels) {
    for (int pass = 0; pass < 2; ++pass) {
        Asm a;
        a.origin = origin;
        a.labels = &labels;
        emit(a);
        if (pass == 1) return a.bytes;
    }
    return {};
}

}  // namespace

int main(int argc, char **argv) {
    uint64_t max_steps = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 2'000'000ull;

    pc486::Chipset chipset;
    Mem m(chipset);

    // The 16-bit and 32-bit halves share one label table, because the
    // 16-bit half's far jump names a 32-bit entry point and vice versa.
    // Assembling 32-bit first gives the 16-bit pass the labels it needs;
    // a second 32-bit pass then picks up "back16".
    std::map<std::string, uint32_t> labels;
    assemble(kCode32, emit_pm32, labels);
    std::vector<uint8_t> c16 = assemble(kCode16Base, emit_code16, labels);
    std::vector<uint8_t> c32 = assemble(kCode32, emit_pm32, labels);
    c16 = assemble(kCode16Base, emit_code16, labels);

    for (size_t i = 0; i < c16.size(); ++i) m.w8(kCode16Base + uint32_t(i), c16[i]);
    for (size_t i = 0; i < c32.size(); ++i) m.w8(kCode32 + uint32_t(i), c32[i]);
    std::printf("assembled %zu bytes of 16-bit entry code at %06X, "
                "%zu bytes of 32-bit protected-mode code at %06X\n",
                c16.size(), kCode16Base, c32.size(), kCode32);

    // --- the GDT ---------------------------------------------------------
    m.w64(kGdt + 0x00, 0);                                                        // null
    m.w64(kGdt + 0x08, seg_desc(0, 0xFFFFFFFFu, 0x9A, true, true));               // 08: flat code32, DPL0
    m.w64(kGdt + 0x10, seg_desc(0, 0xFFFFFFFFu, 0x92, true, true));               // 10: flat data32, DPL0
    m.w64(kGdt + 0x18, seg_desc(kCode16Base, 0xFFFF, 0x9A, false, false));        // 18: code16, DPL0
    m.w64(kGdt + 0x20, seg_desc(kTss1, 0x67, 0x89, false, false));                // 20: TSS 1 (32-bit, available)
    m.w64(kGdt + 0x28, seg_desc(kTss2, 0x67, 0x89, false, false));                // 28: TSS 2
    m.w64(kGdt + 0x30, seg_desc(0, 0xFFFFFFFFu, 0xFA, true, true));               // 30: flat code32, DPL3
    m.w64(kGdt + 0x38, seg_desc(0, 0xFFFFFFFFu, 0xF2, true, true));               // 38: flat data32, DPL3
    m.w16(kGdtPtr, 0x3F);
    m.w32(kGdtPtr + 2, kGdt);

    // --- the IDT: a #PF gate, a #GP gate, and two DPL-3 gates ------------
    for (uint32_t v = 0; v <= 0x21; ++v) m.w64(kIdt + v * 8, 0);
    m.w64(kIdt + 14 * 8, gate_desc(0x08, labels["pf_handler"], 0x8E));      // #PF, 32-bit int gate, DPL0
    m.w64(kIdt + 0x20 * 8, gate_desc(0x08, labels["int20_handler"], 0xEE)); // DPL3 so ring 3 may call it
    m.w64(kIdt + 0x21 * 8, gate_desc(0x08, labels["int21_handler"], 0xEE));
    m.w16(kIdtPtr, 0x10F);
    m.w32(kIdtPtr + 2, kIdt);

    // --- page tables: linear 0-4MB identity, linear 8MB -> kAliasFrame ----
    // Deliberately *not* mapping linear 12MB: step 5's fault handler installs
    // that itself, which is the point of the exercise.
    m.w32(kPageDir + 0 * 4, kPageTab0 | 0x07u);   // present, writable, user
    m.w32(kPageDir + 2 * 4, kPageTab2 | 0x07u);
    for (uint32_t p = 0; p < 1024; ++p) m.w32(kPageTab0 + p * 4, (p << 12) | 0x07u);
    m.w32(kPageTab2 + 0 * 4, kAliasFrame | 0x07u);  // linear 0x00800000 -> kAliasFrame
    m.w32(kPfFrame, kPfMagic);                      // what the restarted read must find

    // --- the two task state segments -------------------------------------
    for (uint32_t i = 0; i < 104; i += 4) { m.w32(kTss1 + i, 0); m.w32(kTss2 + i, 0); }
    // TSS 1 only needs a ring-0 stack, for the DPL-3 gate to switch onto.
    m.w32(kTss1 + 4, 0x0000FA00u);   // ESP0
    m.w32(kTss1 + 8, 0x00000010u);   // SS0
    m.w32(kTss1 + 28, kPageDir);     // CR3
    m.w16(kTss1 + 102, 0x68);        // I/O map base: past the limit, so all ports denied
    // TSS 2 is a complete task image: the CPU loads every one of these.
    m.w32(kTss2 + 4, 0x0000F400u);   // ESP0
    m.w32(kTss2 + 8, 0x00000010u);   // SS0
    m.w32(kTss2 + 28, kPageDir);     // CR3
    m.w32(kTss2 + 32, labels["task2_entry"]);  // EIP
    m.w32(kTss2 + 36, 0x00000202u);  // EFLAGS
    m.w32(kTss2 + 56, 0x0000F600u);  // ESP
    m.w32(kTss2 + 72, 0x00000010u);  // ES
    m.w32(kTss2 + 76, 0x00000008u);  // CS
    m.w32(kTss2 + 80, 0x00000010u);  // SS
    m.w32(kTss2 + 84, 0x00000010u);  // DS
    m.w16(kTss2 + 102, 0x68);

    // --- run it ----------------------------------------------------------
    cpu80486::Cpu cpu(chipset.make_bus());
    cpu.reset();
    cpu.cs = 0x1000;   // real-mode CS:IP = 1000:0000, where emit_code16 starts
    cpu.eip = 0;

    struct Trace { int vector; uint32_t error; uint16_t cs; uint32_t eip; };
    std::vector<Trace> faults;
    cpu.on_fault = [&](int v, uint32_t e, uint16_t c, uint32_t ip) {
        if (faults.size() < 64) faults.push_back({v, e, c, ip});
    };
    int unimpl = 0;
    cpu.on_unimplemented = [&](uint16_t, uint32_t, uint16_t opword) {
        if (unimpl < 8) std::printf("  !! unimplemented opcode %04X\n", opword);
        ++unimpl;
    };

    // Milestone markers, so a failure says *where* the stub stopped rather
    // than only that it did.
    bool saw_pm = false, saw_paging = false, saw_ring3 = false, saw_realmode_again = false;
    uint64_t steps = 0;
    for (; steps < max_steps && !cpu.halted; ++steps) {
        if (cpu.protected_mode()) saw_pm = true;
        if (cpu.paging_enabled()) saw_paging = true;
        if (cpu.protected_mode() && cpu.cpl() == 3) saw_ring3 = true;
        if (saw_pm && !cpu.protected_mode()) saw_realmode_again = true;
        cpu.step();
    }

    std::printf("ran %llu instructions, %llu cycles, halted=%d\n",
                (unsigned long long)steps, (unsigned long long)cpu.cycles, cpu.halted ? 1 : 0);
    std::printf("reached: protected mode=%d, paging=%d, ring 3=%d, back to real mode=%d\n",
                saw_pm, saw_paging, saw_ring3, saw_realmode_again);
    for (const Trace &t : faults)
        std::printf("  fault: vector %2d error %08X at %04X:%08X\n", t.vector, t.error, t.cs, t.eip);

    struct Check { const char *what; bool ok; std::string got, want; };
    std::vector<Check> checks;
    auto hex = [](uint64_t v) { char b[32]; std::snprintf(b, sizeof b, "%08llX", (unsigned long long)v); return std::string(b); };
    auto dec = [](double d) { char b[48]; std::snprintf(b, sizeof b, "%.17g", d); return std::string(b); };
    auto chk32 = [&](const char *what, uint32_t off, uint32_t want) {
        uint32_t got = m.r32(kResults + off);
        checks.push_back({what, got == want, hex(got), hex(want)});
    };
    auto chkd = [&](const char *what, uint32_t off, double want) {
        double got = m.rdouble(kResults + off);
        checks.push_back({what, got == want, dec(got), dec(want)});
    };

    // The stub takes exactly one fault on purpose (the #PF in step 5).
    // Anything else means something went wrong -- and because an
    // undeliverable fault escalates to #DF and then to shutdown, which also
    // sets `halted`, "halted" on its own is not evidence of success.
    int unexpected = 0;
    for (const Trace &t : faults) if (t.vector != 14) ++unexpected;
    checks.push_back({"CPU halted (the stub ran to its end)", cpu.halted, cpu.halted ? "yes" : "no", "yes"});
    checks.push_back({"exactly one fault, the deliberate #PF",
                      faults.size() == 1 && unexpected == 0,
                      std::to_string(faults.size()) + " fault(s), " + std::to_string(unexpected) + " unexpected",
                      "1 fault(s), 0 unexpected"});
    checks.push_back({"no unimplemented opcodes", unimpl == 0, std::to_string(unimpl), "0"});
    checks.push_back({"entered protected mode", saw_pm, saw_pm ? "yes" : "no", "yes"});
    checks.push_back({"enabled paging", saw_paging, saw_paging ? "yes" : "no", "yes"});
    checks.push_back({"ran at CPL 3", saw_ring3, saw_ring3 ? "yes" : "no", "yes"});
    checks.push_back({"returned to real mode", saw_realmode_again, saw_realmode_again ? "yes" : "no", "yes"});
    chk32("32-bit ADD in a D=1 code segment", R_ADD, kAddResult);
    chk32("store to linear 7MB, read back", R_FARSTORE, kAddResult);
    checks.push_back({"linear 7MB really holds it (physical check)",
                      m.r32(kFarStore) == kAddResult, hex(m.r32(kFarStore)), hex(kAddResult)});
    chkd("x87: 2.5 * 4.0", R_FMUL, 10.0);
    chk32("x87: FISTP of 10.0", R_FINT, 10);
    chkd("x87: sqrt(16.0)", R_FSQRT, 4.0);
    chk32("paging: alias read back through linear 8MB", R_ALIAS, kAliasMagic);
    checks.push_back({"paging: the value landed in the mapped frame, not at linear 8MB",
                      m.r32(kAliasFrame) == kAliasMagic, hex(m.r32(kAliasFrame)), hex(kAliasMagic)});
    chk32("#PF: CR2 held the faulting linear address", R_CR2, kFaultLin);
    // Error code: not-present (bit0=0), a read (bit1=0), supervisor (bit2=0).
    chk32("#PF: error code (not present, read, supervisor)", R_PFERR, 0x00000000u);
    chk32("#PF: the restarted instruction read the newly mapped page", R_PFVALUE, kPfMagic);
    chk32("task switch: EAX restored from TSS 1", R_TASKEAX, kTaskEax);
    chk32("task switch: the second task ran", R_TASK2, kTask2Magic);
    chk32("ring 3: a CPL-3 store", R_RING3, kRing3Magic);
    chk32("DPL-3 gate: handler entered at ring 0", R_HANDLERCS, 0x00000008u);
    chk32("DPL-3 gate: pushed the ring-3 CS", R_FRAMECS, 0x00000033u);
    chk32("real mode again: final store", R_REALMODE, kRealMagic);

    int failed = 0;
    std::printf("\n--- checks ---\n");
    for (const Check &c : checks) {
        if (!c.ok) ++failed;
        std::printf("  [%s] %-58s got %s want %s\n", c.ok ? "PASS" : "FAIL", c.what,
                    c.got.c_str(), c.want.c_str());
    }
    std::printf("\n%d of %zu checks passed\n", int(checks.size()) - failed, checks.size());
    if (failed) { std::printf("PROTECTED-MODE STUB FAILED\n"); return 1; }
    std::printf("PROTECTED-MODE STUB OK\n");
    return 0;
}
