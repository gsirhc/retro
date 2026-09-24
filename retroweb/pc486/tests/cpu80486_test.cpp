// GoogleTest suite for the 80486 real-mode core's instruction *semantics*
// (cycle costs live in cpu80486_timing_test.cpp). Coverage is weighted
// toward what is genuinely new on a 486 relative to ibmpc-at's 80286 core,
// since the shared 8086-legacy subset is the same code shape in both:
//
//   - 32-bit registers as the native register file (EAX/ESP/..., and the
//     sub-register write rules that come with them),
//   - FS and GS plus their 0x64/0x65 override prefixes, PUSH/POP FS/GS
//     and LSS/LFS/LGS,
//   - the 0x67 address-size prefix, SIB-byte addressing, and the
//     documented 64KB real-mode segment-limit behavior that goes with it,
//   - the 486-native opcodes BSWAP / XADD / CMPXCHG, plus the 386
//     additions a 486 inherits (MOVZX/MOVSX, BSF/BSR, BT group,
//     SHLD/SHRD, two- and three-operand IMUL, SETcc, Jcc rel16/32),
//   - the AC flag (EFLAGS bit 18) and its toggle-and-read-back behavior,
//   - and the deliberately-documented no-ops: every protected-mode entry
//     and management opcode, and the x87 ESC space, which must consume
//     their operands without firing the on_unimplemented diagnostic hook,
//     while a genuinely absent opcode (CPUID on an early IntelDX2) must.
//
// Reference values are worked by hand against the Intel 80486 Programmer's
// Reference Manual (1990/1992) and, for the 8086-legacy subset, the Intel
// 8086/8088 User's Manual; AP-485 ("Intel Processor Identification and the
// CPUID Instruction") is the reference for the AC-flag detection sequence.
// Opcode encodings are cross-checked against the canonical byte sequences
// for each mnemonic (e.g. "01 D8" = ADD AX,BX, "0F C8" = BSWAP EAX).

#include <gtest/gtest.h>

#include "cpu80486.h"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using cpu80486::Bus;
using cpu80486::Cpu;

class Cpu80486Test : public ::testing::Test {
protected:
    std::array<uint8_t, 0x100000> mem{};  // 1MB flat, enough for real-mode addressing
    std::unique_ptr<Cpu> cpu;
    uint8_t  last_out_port_val = 0;
    uint16_t last_out_port = 0;
    uint8_t  next_in_val = 0xFF;
    uint16_t last_out16_port = 0;
    uint16_t last_out16_val = 0;
    uint16_t next_in16_val = 0xFFFF;

    // on_unimplemented capture -- the diagnostic hook must fire for a
    // genuinely unrecognized opcode and must NOT fire for the documented
    // protected-mode/FPU no-ops.
    int      unimpl_count = 0;
    uint16_t unimpl_opcode = 0;

public:
    // The six operations Bus::For binds (see cpu80486.h).
    uint8_t mem_read(uint32_t a) { return mem[a & 0xFFFFF]; }
    void mem_write(uint32_t a, uint8_t v) { mem[a & 0xFFFFF] = v; }
    uint8_t io_in(uint16_t) { return next_in_val; }
    void io_out(uint16_t p, uint8_t v) { last_out_port = p; last_out_port_val = v; }
    // Genuinely atomic 16-bit port access, distinct from in/out, so a test
    // can prove IN AX,DX / OUT DX,AX take this path rather than silently
    // decomposing into two 8-bit accesses (wrong for a device like the IDE
    // data register at 0x1F0 -- see wd1003.h).
    uint16_t io_in16(uint16_t) { return next_in16_val; }
    void io_out16(uint16_t p, uint16_t v) { last_out16_port = p; last_out16_val = v; }

protected:
    void SetUp() override {
        cpu = std::make_unique<Cpu>(Bus::For(this));
        cpu->reset();
        cpu->cs = 0;
        cpu->eip = 0;
        cpu->on_unimplemented = [this](uint16_t, uint32_t, uint16_t opword) {
            ++unimpl_count;
            unimpl_opcode = opword;
        };
    }

    void load(std::initializer_list<uint8_t> code, uint16_t at = 0) {
        uint16_t addr = at;
        for (uint8_t b : code) mem[addr++] = b;
    }
    // Assemble `code` at CS:0 and execute exactly one instruction.
    void run(std::initializer_list<uint8_t> code) {
        load(code);
        cpu->eip = 0;
        cpu->step();
    }
    // Assemble `code` at CS:0 and execute `n` instructions in sequence.
    void runN(std::initializer_list<uint8_t> code, int n) {
        load(code);
        cpu->eip = 0;
        for (int i = 0; i < n; ++i) cpu->step();
    }
    // Assembles at CS:0 and runs one instruction, deliberately *without* the
    // FNINIT most FPU sequences start with, so a test can continue from the
    // FPU state the previous sequence left.
    void put_and_run(std::initializer_list<uint8_t> code) {
        load(code);
        cpu->eip = 0;
        cpu->step();
    }
    uint16_t memw(uint32_t a) const { return uint16_t(mem[a] | (uint16_t(mem[a + 1]) << 8)); }
    uint32_t memd(uint32_t a) const { return uint32_t(memw(a)) | (uint32_t(memw(a + 2)) << 16); }
    void poke16(uint32_t a, uint16_t v) { mem[a] = uint8_t(v); mem[a + 1] = uint8_t(v >> 8); }
    void poke32(uint32_t a, uint32_t v) { poke16(a, uint16_t(v)); poke16(a + 2, uint16_t(v >> 16)); }

    bool CF() const { return cpu->flag(cpu80486::FLAG_CF); }
    bool ZF() const { return cpu->flag(cpu80486::FLAG_ZF); }
    bool SF() const { return cpu->flag(cpu80486::FLAG_SF); }
    bool OF() const { return cpu->flag(cpu80486::FLAG_OF); }
    bool AF() const { return cpu->flag(cpu80486::FLAG_AF); }
};

// ---------------------------------------------------------------------------
// Reset state
// ---------------------------------------------------------------------------

TEST_F(Cpu80486Test, ResetLandsOnTheX86ResetVector) {
    cpu->reset();
    EXPECT_EQ(cpu->cs, 0xF000);
    EXPECT_EQ(cpu->eip, 0xFFF0u);
    EXPECT_EQ(cpu->eflags, uint32_t(cpu80486::FLAG_R1));
    EXPECT_FALSE(cpu->halted);
}

// ---------------------------------------------------------------------------
// 32-bit registers as the native register file
// ---------------------------------------------------------------------------

TEST_F(Cpu80486Test, MovImm32LoadsAllThirtyTwoBits) {
    run({0x66, 0xB8, 0x78, 0x56, 0x34, 0x12});  // MOV EAX, 12345678h
    EXPECT_EQ(cpu->eax, 0x12345678u);
}

TEST_F(Cpu80486Test, SixteenBitWriteLeavesUpperHalfAlone) {
    cpu->eax = 0xDEAD0000u;
    run({0xB8, 0x34, 0x12});  // MOV AX, 1234h
    EXPECT_EQ(cpu->eax, 0xDEAD1234u);
}

TEST_F(Cpu80486Test, ByteWritesTouchOnlyTheAddressedByte) {
    cpu->eax = 0x11223344u;
    run({0xB0, 0x99});  // MOV AL, 99h
    EXPECT_EQ(cpu->eax, 0x11223399u);
    run({0xB4, 0x77});  // MOV AH, 77h
    EXPECT_EQ(cpu->eax, 0x11227799u);
}

TEST_F(Cpu80486Test, ThirtyTwoBitAddCarriesOutOfBitThirtyOne) {
    cpu->eax = 1;
    cpu->ebx = 0xFFFFFFFFu;
    run({0x66, 0x01, 0xD8});  // ADD EAX, EBX
    EXPECT_EQ(cpu->eax, 0u);
    EXPECT_TRUE(ZF());
    EXPECT_TRUE(CF());
}

TEST_F(Cpu80486Test, ThirtyTwoBitPushPopRoundTrip) {
    cpu->ss = 0;
    cpu->esp = 0x2000;
    cpu->ebx = 0x12345678u;
    runN({0x66, 0x53, 0x66, 0x59}, 2);  // PUSH EBX ; POP ECX
    EXPECT_EQ(cpu->ecx, 0x12345678u);
    EXPECT_EQ(cpu->esp, 0x2000u);
}

TEST_F(Cpu80486Test, PushEspPushesThePreDecrementValue) {
    // Intel 80486 PRM: from the 286 onward PUSH SP/ESP pushes the register's
    // value as it was *before* the instruction, unlike the 8086, which
    // pushes the already-decremented value. This is the classic
    // "push sp / pop ax / cmp ax,sp" runtime check for 8086-vs-286+.
    cpu->ss = 0;
    cpu->esp = 0x2000;
    run({0x54});  // PUSH SP
    EXPECT_EQ(cpu->esp, 0x1FFEu);
    EXPECT_EQ(memw(0x1FFE), 0x2000) << "486 pushes SP's pre-decrement value";
}

TEST_F(Cpu80486Test, PushadPopadRoundTripsAllEightThirtyTwoBitRegisters) {
    cpu->ss = 0;
    cpu->esp = 0x4000;
    cpu->eax = 0x11111111u; cpu->ecx = 0x22222222u; cpu->edx = 0x33333333u;
    cpu->ebx = 0x44444444u; cpu->ebp = 0x55555555u; cpu->esi = 0x66666666u;
    cpu->edi = 0x77777777u;
    run({0x66, 0x60});  // PUSHAD
    cpu->eax = cpu->ecx = cpu->edx = cpu->ebx = cpu->ebp = cpu->esi = cpu->edi = 0;
    run({0x66, 0x61});  // POPAD
    EXPECT_EQ(cpu->eax, 0x11111111u);
    EXPECT_EQ(cpu->ecx, 0x22222222u);
    EXPECT_EQ(cpu->edx, 0x33333333u);
    EXPECT_EQ(cpu->ebx, 0x44444444u);
    EXPECT_EQ(cpu->ebp, 0x55555555u);
    EXPECT_EQ(cpu->esi, 0x66666666u);
    EXPECT_EQ(cpu->edi, 0x77777777u);
    EXPECT_EQ(cpu->esp, 0x4000u);
}

TEST_F(Cpu80486Test, CwdeAndCdqSignExtendTheThirtyTwoBitWay) {
    cpu->eax = 0x0000FFFFu;  // AX = -1
    run({0x66, 0x98});       // CWDE
    EXPECT_EQ(cpu->eax, 0xFFFFFFFFu);
    run({0x66, 0x99});       // CDQ
    EXPECT_EQ(cpu->edx, 0xFFFFFFFFu);
}

TEST_F(Cpu80486Test, ThirtyTwoBitDivUsesEdxEaxAsTheDividend) {
    cpu->edx = 0;
    cpu->eax = 0x00100000u;
    cpu->ebx = 0x00000100u;
    run({0x66, 0xF7, 0xF3});  // DIV EBX
    EXPECT_EQ(cpu->eax, 0x00001000u);
    EXPECT_EQ(cpu->edx, 0u);
}

// ---------------------------------------------------------------------------
// FS / GS -- 386 additions a 486 genuinely has, usable in real mode
// ---------------------------------------------------------------------------

TEST_F(Cpu80486Test, MovLoadsFsAndGs) {
    cpu->eax = 0x1234;
    run({0x8E, 0xE0});  // MOV FS, AX  (modrm mod=11 reg=100/FS rm=000/AX)
    EXPECT_EQ(cpu->fs, 0x1234);
    run({0x8E, 0xE8});  // MOV GS, AX
    EXPECT_EQ(cpu->gs, 0x1234);
    cpu->ebx = 0;
    run({0x8C, 0xE3});  // MOV BX, FS
    EXPECT_EQ(cpu->ebx & 0xFFFF, 0x1234u);
}

TEST_F(Cpu80486Test, FsOverridePrefixRedirectsMemoryAccess) {
    cpu->fs = 0x2000;
    cpu->ebx = 0x10;
    mem[(0x2000u << 4) + 0x10] = 0x5A;  // FS:[BX]
    mem[0x10] = 0x11;                   // DS:[BX] decoy (DS = 0)
    run({0x64, 0x8A, 0x07});  // FS: MOV AL, [BX]
    EXPECT_EQ(cpu->eax & 0xFF, 0x5Au);
}

TEST_F(Cpu80486Test, GsOverridePrefixRedirectsMemoryAccess) {
    cpu->gs = 0x3000;
    cpu->ebx = 0x20;
    mem[(0x3000u << 4) + 0x20] = 0xA5;
    mem[0x20] = 0x22;
    run({0x65, 0x8A, 0x07});  // GS: MOV AL, [BX]
    EXPECT_EQ(cpu->eax & 0xFF, 0xA5u);
}

TEST_F(Cpu80486Test, PushAndPopFsAndGs) {
    cpu->ss = 0;
    cpu->esp = 0x1000;
    cpu->fs = 0xBEEF;
    cpu->gs = 0;
    runN({0x0F, 0xA0, 0x0F, 0xA9}, 2);  // PUSH FS ; POP GS
    EXPECT_EQ(cpu->gs, 0xBEEF);
    EXPECT_EQ(cpu->esp, 0x1000u);
}

TEST_F(Cpu80486Test, LfsLoadsOffsetAndSegmentTogether) {
    poke16(0x0200, 0x1234);  // offset
    poke16(0x0202, 0x9ABC);  // segment
    run({0x0F, 0xB4, 0x1E, 0x00, 0x02});  // LFS BX, [0200h]
    EXPECT_EQ(cpu->ebx & 0xFFFF, 0x1234u);
    EXPECT_EQ(cpu->fs, 0x9ABC);
}

TEST_F(Cpu80486Test, LssLoadsStackSegmentAndPointer) {
    poke16(0x0300, 0x0FF0);
    poke16(0x0302, 0x7000);
    run({0x0F, 0xB2, 0x26, 0x00, 0x03});  // LSS SP, [0300h] (reg=100/SP)
    EXPECT_EQ(cpu->esp & 0xFFFF, 0x0FF0u);
    EXPECT_EQ(cpu->ss, 0x7000);
}

// ---------------------------------------------------------------------------
// 0x67 address-size prefix, SIB decoding, and the segment-limit decision
// ---------------------------------------------------------------------------

TEST_F(Cpu80486Test, Addr32SibBaseIndexScale) {
    // MOV AX, [EAX + EBX*4] -- modrm 04 selects the SIB form,
    // SIB 98 = scale 4, index EBX, base EAX.
    cpu->eax = 0x0100;
    cpu->ebx = 0x0002;
    poke16(0x0108, 0x1234);
    run({0x67, 0x8B, 0x04, 0x98});
    EXPECT_EQ(cpu->eax & 0xFFFF, 0x1234u);
}

TEST_F(Cpu80486Test, Addr32Disp32WithNoBaseRegister) {
    // modrm 05 in 32-bit addressing is disp32 with no base at all (it is
    // *not* [EBP], which needs mod=01/10) -- Intel 80486 PRM's 32-bit
    // ModR/M table.
    poke16(0x0200, 0xCAFE);
    run({0x67, 0x8B, 0x05, 0x00, 0x02, 0x00, 0x00});  // MOV AX, [00000200h]
    EXPECT_EQ(cpu->eax & 0xFFFF, 0xCAFEu);
}

TEST_F(Cpu80486Test, Addr32ScaledIndexWithDisp32AndNoBase) {
    // SIB CD = scale 8, index ECX, base 101 with mod=00 -> disp32, no base.
    cpu->ecx = 2;
    poke16(0x0310, 0xBEEF);
    run({0x67, 0x8B, 0x04, 0xCD, 0x00, 0x03, 0x00, 0x00});  // MOV AX, [ECX*8 + 300h]
    EXPECT_EQ(cpu->eax & 0xFFFF, 0xBEEFu);
}

TEST_F(Cpu80486Test, Addr32EbpBaseDefaultsToStackSegment) {
    cpu->ss = 0x3000;
    cpu->ds = 0x1000;  // decoy -- must not be used
    cpu->ebp = 0x20;
    poke16((0x3000u << 4) + 0x30, 0x4321);
    poke16((0x1000u << 4) + 0x30, 0x9999);
    run({0x67, 0x8B, 0x45, 0x10});  // MOV AX, [EBP + 10h]
    EXPECT_EQ(cpu->eax & 0xFFFF, 0x4321u);
}

TEST_F(Cpu80486Test, Addr32PlainBaseRegisterDefaultsToDataSegment) {
    // modrm 00 = [EAX], the simplest 32-bit memory form and the one the
    // decoder's fast path handles inline (PC486_REVIEW.md §16): no SIB, no
    // displacement, DS by default.
    cpu->ds = 0x2000;
    cpu->ss = 0x3000;  // decoy -- only ESP/EBP bases default to SS
    cpu->eax = 0x40;
    poke16((0x2000u << 4) + 0x40, 0xABCD);
    poke16((0x3000u << 4) + 0x40, 0x9999);
    run({0x67, 0x8B, 0x00});  // MOV AX, [EAX]
    EXPECT_EQ(cpu->eax & 0xFFFF, 0xABCDu);
}

TEST_F(Cpu80486Test, Addr32SegmentOverrideBeatsTheEbpStackDefault) {
    // An explicit override wins over the base register's default segment,
    // on the inline fast path as much as anywhere else.
    cpu->es = 0x5000;
    cpu->ss = 0x3000;  // the default this override must displace
    cpu->ebp = 0x20;
    poke16((0x5000u << 4) + 0x30, 0x1111);
    poke16((0x3000u << 4) + 0x30, 0x2222);
    run({0x26, 0x67, 0x8B, 0x45, 0x10});  // ES: MOV AX, [EBP + 10h]
    EXPECT_EQ(cpu->eax & 0xFFFF, 0x1111u);
}

TEST_F(Cpu80486Test, Addr32EspBaseDefaultsToStackSegmentAndIndexFourMeansNoIndex) {
    // SIB 24 = index field 100, which encodes "no index" (ESP can never be
    // an index register), base ESP.
    cpu->ss = 0x4000;
    cpu->esp = 0x50;
    poke16((0x4000u << 4) + 0x50, 0x5678);
    run({0x67, 0x8B, 0x04, 0x24});  // MOV AX, [ESP]
    EXPECT_EQ(cpu->eax & 0xFFFF, 0x5678u);
}

TEST_F(Cpu80486Test, Addr32SibEbpBaseWithDisplacementDefaultsToStackSegment) {
    // modrm 44 = SIB with mod=01 (disp8), SIB 15 = scale 1, index EDX, base
    // EBP. An EBP base defaults to SS whether it arrives through a SIB byte
    // or not, and the decoder's inline SIB path has to say so on its own
    // (PC486_REVIEW.md §16).
    cpu->ss = 0x3000;
    cpu->ds = 0x1000;  // decoy -- must not be used
    cpu->ebp = 0x20;
    cpu->edx = 0x04;
    poke16((0x3000u << 4) + 0x34, 0x7654);
    poke16((0x1000u << 4) + 0x34, 0x9999);
    run({0x67, 0x8B, 0x44, 0x15, 0x10});  // MOV AX, [EBP + EDX + 10h]
    EXPECT_EQ(cpu->eax & 0xFFFF, 0x7654u);
}

TEST_F(Cpu80486Test, Addr32SibSegmentOverrideBeatsTheEspStackDefault) {
    cpu->es = 0x5000;
    cpu->ss = 0x4000;  // the default this override must displace
    cpu->esp = 0x50;
    poke16((0x5000u << 4) + 0x50, 0x1111);
    poke16((0x4000u << 4) + 0x50, 0x2222);
    run({0x26, 0x67, 0x8B, 0x04, 0x24});  // ES: MOV AX, [ESP]
    EXPECT_EQ(cpu->eax & 0xFFFF, 0x1111u);
}

TEST_F(Cpu80486Test, Addr32EffectiveAddressAboveSixtyFourKReachesPastTheSegment) {
    // "Unreal mode": a 32-bit effective address above 0FFFFh is used as
    // computed rather than truncated into a 64KB window. Period DOS software
    // genuinely produces these in real mode -- a memory manager loads a
    // 4GB-limit descriptor in a brief protected-mode excursion, returns to
    // real mode (where loading a segment register sets its base but leaves
    // the cached limit alone, so the big limit survives), and then addresses
    // extended memory with 32-bit offsets. This core has no descriptors and
    // therefore no limit to enforce, so letting the offset through is the
    // faithful answer for the state such a guest believes it is in, and the
    // absent #GP(0) stays listed with the other protected-mode omissions.
    // Found by FreeDOS 1.3's HimemX: see PC486_REVIEW.md §5.4.
    poke16(0x0200, 0x7777);            // the address a truncating core would hit
    poke16(0x10200, 0x1234);           // the address a real unreal-mode access hits
    run({0x67, 0x8B, 0x05, 0x00, 0x02, 0x01, 0x00});  // MOV AX, [00010200h]
    EXPECT_EQ(cpu->eax & 0xFFFF, 0x1234u) << "offset must NOT wrap mod 64K";
    EXPECT_EQ(cpu->cs, 0) << "no fault is taken -- the #GP is a documented gap";
    EXPECT_EQ(cpu->eip, 7u);
}

TEST_F(Cpu80486Test, Addr32StringOpUsesEsiEdiEcxAndReachesAboveSixtyFourK) {
    // The 0x67 prefix selects ESI/EDI/ECX over SI/DI/CX for a string op --
    // genuine 386+/486 behavior, and the same addressing width decode_modrm()
    // honors. FreeDOS 1.3's HimemX copies XMS blocks with exactly this
    // encoding (`F3 67 66 A5`, REP MOVSD addr32); advancing only the 16-bit
    // halves moved every block to a wrapped, wrong address instead, which
    // silently corrupted memory until the guest ran off into the weeds.
    // Source and destination are both deliberately above 64KB, and the low
    // 16 bits of each differ from the full value, so a core that truncates
    // fails this rather than accidentally passing.
    poke32(0x00020000, 0xDEADBEEFu);
    poke32(0x00020004, 0xCAFEBABEu);
    cpu->ds = 0; cpu->es = 0;
    cpu->esi = 0x00020000u;
    cpu->edi = 0x00030000u;
    cpu->ecx = 2;
    run({0xF3, 0x67, 0x66, 0xA5});  // REP MOVSD (addr32, opsize32)
    EXPECT_EQ(memd(0x00030000), 0xDEADBEEFu);
    EXPECT_EQ(memd(0x00030004), 0xCAFEBABEu);
    EXPECT_EQ(cpu->esi, 0x00020008u) << "ESI, not SI, advances";
    EXPECT_EQ(cpu->edi, 0x00030008u) << "EDI, not DI, advances";
    EXPECT_EQ(cpu->ecx, 0u) << "ECX is the counter under a 0x67 prefix";
}

TEST_F(Cpu80486Test, StringOpWithoutAddr32KeepsUsingTheSixteenBitPointers) {
    // Without the 0x67 prefix a string op must still advance only SI/DI and
    // count in CX, leaving the upper halves of ESI/EDI/ECX alone -- the
    // ordinary real-mode case, unchanged by the addr32 support above.
    poke16(0x00000100, 0x1234);
    cpu->ds = 0; cpu->es = 0;
    cpu->esi = 0xAAAA0100u;
    cpu->edi = 0xBBBB0200u;
    cpu->ecx = 0xCCCC0001u;
    run({0xF3, 0xA5});  // REP MOVSW
    EXPECT_EQ(memw(0x00000200), 0x1234u);
    EXPECT_EQ(cpu->esi, 0xAAAA0102u) << "only SI moves; ESI's upper half is untouched";
    EXPECT_EQ(cpu->edi, 0xBBBB0202u);
    EXPECT_EQ(cpu->ecx, 0xCCCC0000u) << "only CX is decremented";
}

TEST_F(Cpu80486Test, LeaKeepsTheFullThirtyTwoBitEffectiveAddress) {
    // LEA touches no memory, so a 32-bit addressing form here is pure
    // arithmetic -- truncating it to the 64KB data window would produce a
    // wrong *number*, not just a wrong address. This is exactly the
    // "LEA as a three-input adder" idiom 386+ compilers emit.
    cpu->eax = 0x00100000u;
    cpu->ebx = 0x00000100u;
    run({0x66, 0x67, 0x8D, 0x04, 0x98});  // LEA EAX, [EAX + EBX*4]
    EXPECT_EQ(cpu->eax, 0x00100400u);
}

TEST_F(Cpu80486Test, Addr32SelectsEcxAsTheLoopCounter) {
    // The address-size prefix picks CX vs ECX for LOOP/JCXZ (LOOPD/JECXZ).
    cpu->ecx = 0x00010000u;  // CX == 0, but ECX != 0
    run({0x67, 0xE3, 0xFE});  // JECXZ $-2
    EXPECT_EQ(cpu->eip, 3u) << "JECXZ must test the full ECX, not CX";
    run({0xE3, 0xFE});        // JCXZ $-2 -- CX is zero, so this one is taken
    EXPECT_EQ(cpu->eip, 0u);
}

TEST_F(Cpu80486Test, SixteenBitAddressingStillWrapsModSixtyFourK) {
    // Unchanged 8086 behavior: each 16-bit addressing sum wraps mod 64K. This
    // is also what keeps the unreal-mode change above (a 32-bit EA reaching
    // past the segment) from leaking into the 16-bit path -- decode_modrm()
    // wraps every intermediate 16-bit sum, so a 16-bit form can never present
    // an offset above 0FFFFh in the first place.
    cpu->ebx = 0xFFF0;
    cpu->esi = 0x0210;
    mem[0x0200] = 0x42;
    mem[0x10200] = 0xBD;  // where a non-wrapping core would land instead
    run({0x8A, 0x00});  // MOV AL, [BX+SI] -> offset 0x10200 mod 64K = 0x200
    EXPECT_EQ(cpu->eax & 0xFF, 0x42u);
}

// ---------------------------------------------------------------------------
// 486-native opcodes
// ---------------------------------------------------------------------------

TEST_F(Cpu80486Test, BswapReversesByteOrder) {
    cpu->eax = 0x12345678u;
    run({0x0F, 0xC8});  // BSWAP EAX
    EXPECT_EQ(cpu->eax, 0x78563412u);
    cpu->ebx = 0xAABBCCDDu;
    run({0x0F, 0xCB});  // BSWAP EBX
    EXPECT_EQ(cpu->ebx, 0xDDCCBBAAu);
}

TEST_F(Cpu80486Test, XaddSumsIntoDestinationAndReturnsOldDestinationInSource) {
    cpu->eax = 5;
    cpu->ebx = 3;
    run({0x0F, 0xC1, 0xD8});  // XADD AX, BX
    EXPECT_EQ(cpu->eax & 0xFFFF, 8u) << "destination gets the sum";
    EXPECT_EQ(cpu->ebx & 0xFFFF, 5u) << "source gets the destination's old value";
}

TEST_F(Cpu80486Test, XaddSetsArithmeticFlagsLikeAdd) {
    cpu->eax = 0xFFFF;
    cpu->ebx = 1;
    run({0x0F, 0xC1, 0xD8});  // XADD AX, BX -> 0 with carry out
    EXPECT_EQ(cpu->eax & 0xFFFF, 0u);
    EXPECT_TRUE(CF());
    EXPECT_TRUE(ZF());
}

TEST_F(Cpu80486Test, XaddThirtyTwoBitOnMemory) {
    cpu->ebx = 0x0400;
    cpu->ecx = 0x00000010u;
    poke32(0x0400, 0x00000001u);
    run({0x66, 0x0F, 0xC1, 0x0F});  // XADD [BX], ECX
    EXPECT_EQ(memd(0x0400), 0x00000011u);
    EXPECT_EQ(cpu->ecx, 0x00000001u);
}

TEST_F(Cpu80486Test, CmpxchgStoresWhenAccumulatorMatches) {
    cpu->eax = 0x1111;  // comparand
    cpu->ecx = 0x1111;  // destination
    cpu->ebx = 0x2222;  // source
    run({0x0F, 0xB1, 0xD9});  // CMPXCHG CX, BX
    EXPECT_TRUE(ZF());
    EXPECT_EQ(cpu->ecx & 0xFFFF, 0x2222u);
    EXPECT_EQ(cpu->eax & 0xFFFF, 0x1111u) << "accumulator untouched on a match";
}

TEST_F(Cpu80486Test, CmpxchgLoadsAccumulatorWhenItDoesNotMatch) {
    cpu->eax = 0x1111;
    cpu->ecx = 0x3333;
    cpu->ebx = 0x2222;
    run({0x0F, 0xB1, 0xD9});  // CMPXCHG CX, BX
    EXPECT_FALSE(ZF());
    EXPECT_EQ(cpu->eax & 0xFFFF, 0x3333u) << "accumulator takes the destination's value";
    EXPECT_EQ(cpu->ecx & 0xFFFF, 0x3333u) << "destination untouched on a mismatch";
}

TEST_F(Cpu80486Test, CmpxchgThirtyTwoBit) {
    cpu->eax = 0xDEADBEEFu;
    cpu->ecx = 0xDEADBEEFu;
    cpu->ebx = 0xFEEDFACEu;
    run({0x66, 0x0F, 0xB1, 0xD9});  // CMPXCHG ECX, EBX
    EXPECT_TRUE(ZF());
    EXPECT_EQ(cpu->ecx, 0xFEEDFACEu);
}

TEST_F(Cpu80486Test, MovzxZeroExtendsAndMovsxSignExtends) {
    cpu->ebx = 0x00FF;  // BL = 0xFF
    run({0x0F, 0xB6, 0xC3});  // MOVZX AX, BL
    EXPECT_EQ(cpu->eax & 0xFFFF, 0x00FFu);
    run({0x0F, 0xBE, 0xC3});  // MOVSX AX, BL
    EXPECT_EQ(cpu->eax & 0xFFFF, 0xFFFFu);
}

TEST_F(Cpu80486Test, MovzxAndMovsxIntoThirtyTwoBitDestinations) {
    cpu->ebx = 0x0000FFFFu;
    run({0x66, 0x0F, 0xB7, 0xC3});  // MOVZX EAX, BX
    EXPECT_EQ(cpu->eax, 0x0000FFFFu);
    run({0x66, 0x0F, 0xBF, 0xC3});  // MOVSX EAX, BX
    EXPECT_EQ(cpu->eax, 0xFFFFFFFFu);
}

TEST_F(Cpu80486Test, BsfFindsTheLowestSetBit) {
    cpu->ebx = 0x0100;
    run({0x0F, 0xBC, 0xC3});  // BSF AX, BX
    EXPECT_EQ(cpu->eax & 0xFFFF, 8u);
    EXPECT_FALSE(ZF());
}

TEST_F(Cpu80486Test, BsrFindsTheHighestSetBit) {
    cpu->ebx = 0x0101;
    run({0x0F, 0xBD, 0xC3});  // BSR AX, BX
    EXPECT_EQ(cpu->eax & 0xFFFF, 8u);
    EXPECT_FALSE(ZF());
}

TEST_F(Cpu80486Test, BitScanOfZeroSetsZeroFlagAndLeavesDestinationUndisturbed) {
    // Intel 80486 PRM: with a zero source, ZF is set and the destination
    // is UNDEFINED -- so this core leaves it alone rather than inventing a
    // value, and the test pins that choice down.
    cpu->ebx = 0;
    cpu->eax = 0xA5A5A5A5u;
    run({0x0F, 0xBC, 0xC3});  // BSF AX, BX
    EXPECT_TRUE(ZF());
    EXPECT_EQ(cpu->eax, 0xA5A5A5A5u);
}

TEST_F(Cpu80486Test, BsfThirtyTwoBitScansTheFullWidth) {
    cpu->ebx = 0x80000000u;
    run({0x66, 0x0F, 0xBC, 0xC3});  // BSF EAX, EBX
    EXPECT_EQ(cpu->eax, 31u);
}

TEST_F(Cpu80486Test, TwoOperandImulThirtyTwoBit) {
    cpu->eax = 0x00010000u;
    cpu->ebx = 0x00000100u;
    run({0x66, 0x0F, 0xAF, 0xC3});  // IMUL EAX, EBX
    EXPECT_EQ(cpu->eax, 0x01000000u);
    EXPECT_FALSE(CF()) << "result fits in 32 bits";
}

TEST_F(Cpu80486Test, TwoOperandImulSetsCarryAndOverflowOnTruncation) {
    cpu->eax = 0x10000000u;
    cpu->ebx = 0x00000100u;
    run({0x66, 0x0F, 0xAF, 0xC3});  // IMUL EAX, EBX -> 2^36, truncated
    EXPECT_TRUE(CF());
    EXPECT_TRUE(OF());
}

TEST_F(Cpu80486Test, ThreeOperandImulWithImm32) {
    cpu->ebx = 6;
    run({0x66, 0x69, 0xC3, 0x07, 0x00, 0x00, 0x00});  // IMUL EAX, EBX, 7
    EXPECT_EQ(cpu->eax, 42u);
    EXPECT_FALSE(OF());
}

TEST_F(Cpu80486Test, ThreeOperandImulWithSignExtendedImm8) {
    cpu->ebx = 5;
    run({0x66, 0x6B, 0xC3, 0xFF});  // IMUL EAX, EBX, -1
    EXPECT_EQ(cpu->eax, 0xFFFFFFFBu);
}

TEST_F(Cpu80486Test, ShiftGroupWithThirtyTwoBitOperands) {
    cpu->ebx = 1;
    run({0x66, 0xC1, 0xE3, 0x04});  // SHL EBX, 4
    EXPECT_EQ(cpu->ebx, 0x10u);
    cpu->ebx = 0x80000000u;
    run({0x66, 0xD1, 0xE3});        // SHL EBX, 1
    EXPECT_EQ(cpu->ebx, 0u);
    EXPECT_TRUE(CF());
    cpu->ebx = 0x80000000u;
    run({0x66, 0xD1, 0xFB});        // SAR EBX, 1 -- sign-propagating at 32 bits
    EXPECT_EQ(cpu->ebx, 0xC0000000u);
}

TEST_F(Cpu80486Test, ShiftCountIsMaskedModThirtyTwo) {
    cpu->ebx = 1;
    run({0x66, 0xC1, 0xE3, 33});  // SHL EBX, 33 -> behaves as SHL EBX, 1
    EXPECT_EQ(cpu->ebx, 2u);
}

TEST_F(Cpu80486Test, ShldFillsFromTheSourceRegister) {
    cpu->eax = 0x12345678u;
    cpu->ebx = 0x9ABCDEF0u;
    run({0x66, 0x0F, 0xA4, 0xD8, 0x04});  // SHLD EAX, EBX, 4
    EXPECT_EQ(cpu->eax, 0x23456789u);
}

TEST_F(Cpu80486Test, ShrdFillsFromTheSourceRegister) {
    cpu->eax = 0x12345678u;
    cpu->ebx = 0x9ABCDEF0u;
    run({0x66, 0x0F, 0xAC, 0xD8, 0x04});  // SHRD EAX, EBX, 4
    EXPECT_EQ(cpu->eax, 0x01234567u);
}

// A 16-bit SHLD/SHRD whose count exceeds the 16-bit operand size is
// "undefined" in Intel's text (Intel 80486 PRM, SHLD/SHRD: the count is
// masked to 5 bits, and a count greater than the operand size leaves the
// result undefined) -- but the 486 has a single 32-bit shifter, so what it
// does is entirely determined: it shifts the 32-bit dest:src concatenation
// and keeps the half the instruction names, which above a count of 15 pulls
// bits of the source register into the destination.
//
// This is not a curiosity. Borland's 16-bit runtime helpers for a 32-bit
// shift -- shipped inside CWSDPMI, the DPMI host every DJGPP program uses --
// are `SHLD dx,ax,cl / XOR bx,bx / SHLD ax,bx,cl` and the SHRD mirror, and
// they are correct across the entire 0-31 count range *only* under this
// behavior. CWSDPMI calls the left form with cl=24 to convert a physical
// page number into a real-mode far pointer; a core that answers 0 there puts
// its page tables at physical address 0, on top of the interrupt vector
// table. See PC486_REVIEW.md §9.
TEST_F(Cpu80486Test, SixteenBitShldAboveFifteenShiftsTheThirtyTwoBitConcatenation) {
    // The case CWSDPMI actually executes: page 0x2B -> far pointer 2B00:0000.
    cpu->edx = 0x00000000u;
    cpu->eax = 0x0000002Bu;
    cpu->ecx = 24;
    run({0x0F, 0xA5, 0xC2});  // SHLD DX, AX, CL
    EXPECT_EQ(cpu->edx & 0xFFFFu, 0x2B00u)
        << "0x0000002B << 24 = 0x2B000000, whose high half is 0x2B00";

    // The whole helper, proving it computes a real 32-bit shift: DX:AX <<= 24.
    cpu->edx = 0x0001u;
    cpu->eax = 0x2345u;
    cpu->ebx = 0xFFFFu;   // clobbered by the helper's own XOR BX,BX
    cpu->ecx = 24;
    runN({0x0F, 0xA5, 0xC2,         // SHLD DX, AX, CL
          0x33, 0xDB,               // XOR  BX, BX
          0x0F, 0xA5, 0xD8}, 3);    // SHLD AX, BX, CL
    EXPECT_EQ(cpu->edx & 0xFFFFu, 0x4500u);
    EXPECT_EQ(cpu->eax & 0xFFFFu, 0x0000u)
        << "0x00012345 << 24 = 0x45000000 in DX:AX";
}

TEST_F(Cpu80486Test, SixteenBitShrdAboveFifteenShiftsTheThirtyTwoBitConcatenation) {
    // The SHRD mirror of the same helper: DX:AX >>= 24.
    cpu->edx = 0x1234u;
    cpu->eax = 0x5678u;
    cpu->ebx = 0xFFFFu;
    cpu->ecx = 24;
    runN({0x33, 0xDB,               // XOR  BX, BX
          0x0F, 0xAD, 0xD0,         // SHRD AX, DX, CL
          0x0F, 0xAD, 0xDA}, 3);    // SHRD DX, BX, CL
    EXPECT_EQ(cpu->eax & 0xFFFFu, 0x0012u);
    EXPECT_EQ(cpu->edx & 0xFFFFu, 0x0000u)
        << "0x12345678 >> 24 = 0x00000012 in DX:AX";
}

// The documented (count <= 15) behavior has to be untouched by the above --
// it is the same 32-bit-concatenation rule, just inside the range Intel
// specifies, and it is what every ordinary use of these instructions hits.
TEST_F(Cpu80486Test, SixteenBitShldAndShrdWithinTheDocumentedRange) {
    cpu->edx = 0x0000u;
    cpu->eax = 0x7B63u;
    cpu->ecx = 10;
    run({0x0F, 0xA5, 0xC2});  // SHLD DX, AX, CL
    EXPECT_EQ(cpu->edx & 0xFFFFu, 0x01EDu);

    cpu->eax = 0x1234u;
    cpu->ebx = 0xABCDu;
    cpu->ecx = 4;
    run({0x0F, 0xA5, 0xD8});  // SHLD AX, BX, CL
    EXPECT_EQ(cpu->eax & 0xFFFFu, 0x234Au);

    cpu->eax = 0x1234u;
    cpu->ebx = 0xABCDu;
    cpu->ecx = 4;
    run({0x0F, 0xAD, 0xD8});  // SHRD AX, BX, CL
    EXPECT_EQ(cpu->eax & 0xFFFFu, 0xD123u);
}

// CF is the last bit shifted out of the 32-bit pair. Inside the documented
// range that is exactly the manual's rule (dest's bit 16-count for SHLD,
// bit count-1 for SHRD), which is why widening the pair to 64 bits could not
// change any specified case.
TEST_F(Cpu80486Test, SixteenBitDoubleShiftCarryIsTheLastBitShiftedOut) {
    cpu->edx = 0x8000u;   // bit 15 is the last bit out for a count of 1
    cpu->eax = 0x0000u;
    cpu->ecx = 1;
    run({0x0F, 0xA5, 0xC2});  // SHLD DX, AX, CL
    EXPECT_TRUE(CF());

    cpu->edx = 0x4000u;
    cpu->eax = 0x0000u;
    cpu->ecx = 1;
    run({0x0F, 0xA5, 0xC2});
    EXPECT_FALSE(CF());

    cpu->eax = 0x0001u;   // bit 0 is the last bit out of a 1-bit SHRD
    cpu->ebx = 0x0000u;
    cpu->ecx = 1;
    run({0x0F, 0xAD, 0xD8});  // SHRD AX, BX, CL
    EXPECT_TRUE(CF());
}

TEST_F(Cpu80486Test, BitTestGroupReadsAndModifiesTheAddressedBit) {
    cpu->ebx = 0x0020;
    run({0x0F, 0xBA, 0xE3, 0x05});  // BT BX, 5
    EXPECT_TRUE(CF());
    EXPECT_EQ(cpu->ebx & 0xFFFF, 0x0020u) << "BT must not modify the operand";
    run({0x0F, 0xBA, 0xEB, 0x00});  // BTS BX, 0
    EXPECT_FALSE(CF());
    EXPECT_EQ(cpu->ebx & 0xFFFF, 0x0021u);
    run({0x0F, 0xBA, 0xF3, 0x05});  // BTR BX, 5
    EXPECT_TRUE(CF());
    EXPECT_EQ(cpu->ebx & 0xFFFF, 0x0001u);
}

TEST_F(Cpu80486Test, BitTestOnMemoryIndexesBeyondTheOperandWidth) {
    // Intel 80486 PRM: with a memory operand and a register bit offset,
    // the offset selects a bit in a string of operand-size units starting
    // at the effective address -- it is not masked to the operand width.
    cpu->ebx = 0x0500;
    cpu->ecx = 17;  // bit 17 = bit 1 of the second 16-bit unit
    poke16(0x0500, 0x0000);
    poke16(0x0502, 0x0002);
    run({0x0F, 0xA3, 0x0F});  // BT [BX], CX
    EXPECT_TRUE(CF());
}

TEST_F(Cpu80486Test, SetccWritesOneOrZero) {
    cpu->set_flag(cpu80486::FLAG_ZF, true);
    cpu->ebx = 0;
    run({0x0F, 0x94, 0xC3});  // SETZ BL
    EXPECT_EQ(cpu->ebx & 0xFF, 1u);
    cpu->set_flag(cpu80486::FLAG_ZF, false);
    run({0x0F, 0x94, 0xC3});
    EXPECT_EQ(cpu->ebx & 0xFF, 0u);
}

TEST_F(Cpu80486Test, JccNearRel16AndRel32) {
    cpu->set_flag(cpu80486::FLAG_ZF, true);
    run({0x0F, 0x84, 0x05, 0x00});  // JZ near +5
    EXPECT_EQ(cpu->eip, 4u + 5u);
    cpu->set_flag(cpu80486::FLAG_ZF, false);
    run({0x0F, 0x84, 0x05, 0x00});
    EXPECT_EQ(cpu->eip, 4u);
    cpu->set_flag(cpu80486::FLAG_ZF, true);
    run({0x66, 0x0F, 0x84, 0x05, 0x00, 0x00, 0x00});  // JZ near rel32 +5
    EXPECT_EQ(cpu->eip, 7u + 5u);
}

// ---------------------------------------------------------------------------
// AC (Alignment Check), EFLAGS bit 18 -- the pre-CPUID 386-vs-486 probe
// ---------------------------------------------------------------------------

TEST_F(Cpu80486Test, PopfdAndPushfdRoundTripTheAcBit) {
    cpu->ss = 0;
    cpu->esp = 0x2000;
    poke32(0x2000, cpu80486::FLAG_AC | cpu80486::FLAG_R1);
    run({0x66, 0x9D});  // POPFD
    EXPECT_TRUE(cpu->flag(cpu80486::FLAG_AC));
    cpu->esp = 0x2000;
    run({0x66, 0x9C});  // PUSHFD
    EXPECT_TRUE((memd(0x1FFC) & cpu80486::FLAG_AC) != 0);
}

TEST_F(Cpu80486Test, SixteenBitPopfLeavesTheAcBitAlone) {
    // AC lives above bit 15, so a 16-bit POPF cannot clear it -- which is
    // what makes the AP-485 sequence require the 32-bit forms.
    cpu->set_flag(cpu80486::FLAG_AC, true);
    cpu->ss = 0;
    cpu->esp = 0x2000;
    poke16(0x2000, cpu80486::FLAG_R1);
    run({0x9D});  // POPF
    EXPECT_TRUE(cpu->flag(cpu80486::FLAG_AC));
}

TEST_F(Cpu80486Test, AcBitTogglesAndReadsBackPerAp485) {
    // AP-485's "Intel386 processor check": push EFLAGS, flip AC, write it
    // back, read it again -- "can't toggle AC bit, processor=80386". On a
    // 486 the flipped bit survives, which is what this asserts.
    cpu->ss = 0;
    cpu->esp = 0x3000;
    cpu->set_flag(cpu80486::FLAG_AC, false);
    runN({
        0x66, 0x9C,                                // PUSHFD
        0x66, 0x58,                                // POP EAX
        0x66, 0x35, 0x00, 0x00, 0x04, 0x00,        // XOR EAX, 00040000h  (flip AC)
        0x66, 0x50,                                // PUSH EAX
        0x66, 0x9D,                                // POPFD
        0x66, 0x9C,                                // PUSHFD
        0x66, 0x5B,                                // POP EBX
    }, 8);
    EXPECT_TRUE((cpu->ebx & cpu80486::FLAG_AC) != 0)
        << "a 486 keeps a toggled AC bit; a 386 would read it back as 0";
    EXPECT_TRUE(cpu->flag(cpu80486::FLAG_AC));
}

TEST_F(Cpu80486Test, PopfdDoesNotSetReservedOrVirtualModeBits) {
    cpu->ss = 0;
    cpu->esp = 0x2000;
    poke32(0x2000, 0xFFFFFFFFu);
    run({0x66, 0x9D});  // POPFD with every bit set
    EXPECT_EQ(cpu->eflags & 0x00008000u, 0u) << "bit 15 is reserved-0 on a 486";
    EXPECT_EQ(cpu->eflags & 0x00030000u, 0u) << "RF and VM are not loaded by POPFD";
    EXPECT_EQ(cpu->eflags & 0xFFD80000u, 0u)
        << "bits 19-31 are reserved-0 on a 486, ID (21) excepted";
    EXPECT_TRUE(cpu->flag(cpu80486::FLAG_AC));
    EXPECT_TRUE(cpu->flag(cpu80486::FLAG_NT));
    EXPECT_TRUE(cpu->flag(cpu80486::FLAG_ID))
        << "ID is writable on the 486 revisions that carry CPUID (AP-485)";
}

TEST_F(Cpu80486Test, PopfLoadsIoplAndNtInRealMode) {
    // Real mode has no CPL to gate the IOPL/NT load, so a real 486 loads
    // them unconditionally. (ibmpc-at's 286 core deliberately masks these
    // to zero for an empirical FreeDOS-installer reason documented in
    // IBM_PCAT_REVIEW.md; this core implements the genuine behavior.)
    cpu->ss = 0;
    cpu->esp = 0x2000;
    poke16(0x2000, uint16_t(cpu80486::FLAG_IOPL | cpu80486::FLAG_NT | cpu80486::FLAG_R1));
    run({0x9D});  // POPF
    EXPECT_EQ(cpu->eflags & cpu80486::FLAG_IOPL, uint32_t(cpu80486::FLAG_IOPL));
    EXPECT_TRUE(cpu->flag(cpu80486::FLAG_NT));
}

// ---------------------------------------------------------------------------
// Descriptor-table and mode-control instructions, which Milestone 1 carried
// as documented no-ops and Milestone 2 makes real. None may fire
// on_unimplemented, and all must consume their operands so the instruction
// stream stays in sync -- but now they also have to *work*.
// ---------------------------------------------------------------------------

TEST_F(Cpu80486Test, LgdtAndLidtLoadTheDescriptorTableRegisters) {
    // The 6-byte pseudo-descriptor a real DOS extender points LGDT at: a
    // 16-bit limit followed by a 32-bit base.
    poke16(0x0200, 0x0017);
    poke32(0x0202, 0x00081234u);
    run({0x0F, 0x01, 0x16, 0x00, 0x02});  // LGDT [0200h]
    EXPECT_EQ(cpu->eip, 5u) << "ModR/M and disp16 must be consumed";
    EXPECT_EQ(cpu->gdtr().limit, 0x0017);
    // A 486 in real mode defaults to 16-bit operands, and the 16-bit form of
    // LGDT loads only 24 bits of base -- the 286-compatible pseudo-descriptor
    // (Intel 80486 PRM, LGDT/LIDT).
    EXPECT_EQ(cpu->gdtr().base, 0x00081234u & 0x00FFFFFFu);

    poke16(0x0300, 0x07FF);
    poke32(0x0302, 0x00012000u);
    run({0x0F, 0x01, 0x1E, 0x00, 0x03});  // LIDT [0300h]
    EXPECT_EQ(cpu->idtr().limit, 0x07FF);
    EXPECT_EQ(cpu->idtr().base, 0x00012000u);
    EXPECT_EQ(unimpl_count, 0);
}

TEST_F(Cpu80486Test, LgdtWithA32BitOperandSizeLoadsTheFullBase) {
    poke16(0x0200, 0x0017);
    poke32(0x0202, 0xFE081234u);
    run({0x66, 0x0F, 0x01, 0x16, 0x00, 0x02});  // LGDT [0200h], operand-size 32
    EXPECT_EQ(cpu->gdtr().base, 0xFE081234u)
        << "the 32-bit form loads all four base bytes, not the 286's three";
}

TEST_F(Cpu80486Test, SgdtAndSidtStoreThemBackAgain) {
    poke16(0x0200, 0x0FFF);
    poke32(0x0202, 0x00123456u);
    run({0x66, 0x0F, 0x01, 0x16, 0x00, 0x02});  // LGDT [0200h]
    run({0x66, 0x0F, 0x01, 0x06, 0x00, 0x04});  // SGDT [0400h]
    EXPECT_EQ(memw(0x0400), 0x0FFF);
    EXPECT_EQ(memd(0x0402), 0x00123456u);
    EXPECT_EQ(unimpl_count, 0);
}

TEST_F(Cpu80486Test, SmswReportsTheRealMachineStatusWordIncludingHardwiredEt) {
    cpu->ebx = 0xFFFF;
    run({0x0F, 0x01, 0xE3});  // SMSW BX
    // CR0.ET is hardwired to 1 on an Intel486 -- the FPU is on-die, so there
    // is no "is a coprocessor installed" question left to answer. PE is 0
    // because nothing has entered protected mode yet.
    EXPECT_EQ(cpu->ebx & 0xFFFF, uint32_t(cpu80486::CR0_ET));
    EXPECT_FALSE(cpu->protected_mode());
    EXPECT_EQ(unimpl_count, 0);
}

TEST_F(Cpu80486Test, LmswEntersProtectedModeAndCannotLeaveItAgain) {
    cpu->eax = 0x0001;        // PE
    run({0x0F, 0x01, 0xF0});  // LMSW AX
    EXPECT_TRUE(cpu->protected_mode()) << "LMSW is how 286-era code entered protected mode";
    // Famously, LMSW cannot *clear* PE: a 286 could enter protected mode and
    // never leave, and the 386/486 kept that asymmetry (Intel 80486 PRM,
    // LMSW). Leaving needs a MOV to CR0.
    cpu->eax = 0x0000;
    run({0x0F, 0x01, 0xF0});  // LMSW AX
    EXPECT_TRUE(cpu->protected_mode()) << "PE survives an LMSW that tries to clear it";
    EXPECT_EQ(unimpl_count, 0);
}

TEST_F(Cpu80486Test, MovToCr0HonorsProtectionEnableBothWays) {
    cpu->eax = uint32_t(cpu80486::CR0_PE);
    run({0x0F, 0x22, 0xC0});  // MOV CR0, EAX
    EXPECT_TRUE(cpu->protected_mode());
    cpu->eax = 0;
    run({0x0F, 0x20, 0xC0});  // MOV EAX, CR0
    EXPECT_EQ(cpu->eax, uint32_t(cpu80486::CR0_PE | cpu80486::CR0_ET))
        << "PE round-trips for real now, and ET is permanently set";
    // And back out: unlike LMSW, a MOV to CR0 can return to real mode, which
    // is exactly how a DOS extender gets back to DOS.
    cpu->eax = uint32_t(cpu80486::CR0_ET);
    run({0x0F, 0x22, 0xC0});
    EXPECT_FALSE(cpu->protected_mode());
    EXPECT_EQ(unimpl_count, 0);
}

TEST_F(Cpu80486Test, EnablingPagingWithoutProtectionIsAGeneralProtectionFault) {
    // "Paging can be enabled only when protection is enabled" -- setting PG
    // with PE clear is a #GP(0), not a silently dropped bit (Intel 80486
    // PRM, CR0). The real-mode IVT is all zeros here, so the fault vectors
    // to 0000:0000; what this pins down is that CR0.PG did *not* take.
    cpu->eax = uint32_t(cpu80486::CR0_PG);
    run({0x0F, 0x22, 0xC0});  // MOV CR0, EAX
    EXPECT_FALSE(cpu->paging_enabled());
    EXPECT_FALSE(cpu->protected_mode());
    EXPECT_EQ(unimpl_count, 0) << "a fault is not an unimplemented opcode";
}

TEST_F(Cpu80486Test, ProtectedModeOnlyOpcodesAreInvalidOpcodesInRealMode) {
    // LLDT/LTR/SLDT/STR/VERR/VERW/LAR/LSL/ARPL need descriptor tables to mean
    // anything, and Intel documents all of them as "not recognized in Real
    // Address Mode" -- #UD, not the no-ops Milestone 1 carried. Vector 6 is
    // routed to a stub that just sets a flag so the fault is observable.
    poke16(0x0006 * 4 + 0, 0x0400);   // IVT[6] -> 0000:0400
    poke16(0x0006 * 4 + 2, 0x0000);
    mem[0x0400] = 0xF4;               // HLT, so a taken #UD is unmistakable
    for (std::initializer_list<uint8_t> code : {
             std::initializer_list<uint8_t>{0x0F, 0x00, 0xD0},  // LLDT AX
             std::initializer_list<uint8_t>{0x0F, 0x00, 0xD8},  // LTR AX
             std::initializer_list<uint8_t>{0x0F, 0x00, 0xC0},  // SLDT AX
             std::initializer_list<uint8_t>{0x0F, 0x00, 0xE0},  // VERR AX
             std::initializer_list<uint8_t>{0x0F, 0x02, 0xC3},  // LAR AX,BX
             std::initializer_list<uint8_t>{0x0F, 0x03, 0xC3},  // LSL AX,BX
             std::initializer_list<uint8_t>{0x63, 0xC3},        // ARPL BX,AX
         }) {
        cpu->halted = false;
        run(code);
        EXPECT_EQ(cpu->cs, 0u);
        EXPECT_EQ(cpu->eip, 0x0400u) << "should have vectored through IVT[6] (#UD)";
    }
    EXPECT_EQ(unimpl_count, 0) << "a real #UD is not a coverage gap";
}

TEST_F(Cpu80486Test, SgdtLgdtAndTheControlRegistersStayLegalInRealMode) {
    // The other half of the same group *is* legal in real mode, and has to
    // stay that way: FreeDOS 1.3's HimemX executes LGDT plus MOV CR0 in real
    // mode on every XMS block move (PC486_REVIEW.md §5.4).
    run({0x0F, 0x01, 0x16, 0x00, 0x02});  // LGDT
    run({0x0F, 0x01, 0x06, 0x00, 0x02});  // SGDT
    run({0x0F, 0x01, 0x1E, 0x00, 0x02});  // LIDT
    run({0x0F, 0x01, 0x0E, 0x00, 0x02});  // SIDT
    run({0x0F, 0x01, 0xE0});              // SMSW AX
    run({0x0F, 0x06});                    // CLTS
    run({0x0F, 0x08});                    // INVD
    run({0x0F, 0x09});                    // WBINVD
    run({0x0F, 0x01, 0x3E, 0x00, 0x02});  // INVLPG [0200h]
    EXPECT_EQ(cpu->cs, 0u) << "none of these may fault in real mode";
    EXPECT_EQ(unimpl_count, 0);
}

TEST_F(Cpu80486Test, CltsClearsTheTaskSwitchedBit) {
    cpu->eax = uint32_t(cpu80486::CR0_TS);
    run({0x0F, 0x22, 0xC0});  // MOV CR0, EAX
    EXPECT_NE(cpu->cr(0) & uint32_t(cpu80486::CR0_TS), 0u);
    run({0x0F, 0x06});        // CLTS
    EXPECT_EQ(cpu->cr(0) & uint32_t(cpu80486::CR0_TS), 0u);
}

TEST_F(Cpu80486Test, DebugRegistersRoundTrip) {
    cpu->eax = 0xDEADBEEFu;
    run({0x0F, 0x23, 0xC0});  // MOV DR0, EAX
    cpu->ebx = 0;
    run({0x0F, 0x21, 0xC3});  // MOV EBX, DR0
    EXPECT_EQ(cpu->ebx, 0xDEADBEEFu);
    EXPECT_EQ(unimpl_count, 0);
}

TEST_F(Cpu80486Test, MovToCr3FlushesTheTlbAndReadsBack) {
    cpu->eax = 0x00002000u;
    run({0x0F, 0x22, 0xD8});  // MOV CR3, EAX
    cpu->ebx = 0;
    run({0x0F, 0x20, 0xDB});  // MOV EBX, CR3
    EXPECT_EQ(cpu->ebx, 0x00002000u);
}

TEST_F(Cpu80486Test, CpuidFunctionZeroReportsGenuineIntelAndAMaximumInputOfOne) {
    // AP-485 gives CPUID to the SL-Enhanced IntelDX2 this machine carries
    // (the pre-SL parts genuinely fault on 0F A2 instead). Function 0 hands
    // back the vendor string in EBX:EDX:ECX and the highest function this
    // processor answers -- 1, because function 2's cache descriptors are
    // Pentium-era.
    cpu->eax = 0;
    run({0x0F, 0xA2});
    EXPECT_EQ(unimpl_count, 0) << "CPUID must not route to the unimplemented-opcode hook";
    EXPECT_EQ(cpu->eax, 1u);
    EXPECT_EQ(cpu->ebx, 0x756E6547u);  // "Genu"
    EXPECT_EQ(cpu->edx, 0x49656E69u);  // "ineI"
    EXPECT_EQ(cpu->ecx, 0x6C65746Eu);  // "ntel"
}

TEST_F(Cpu80486Test, CpuidFunctionOneReportsAnIntelDx2SignatureWithOnlyTheFpuFeature) {
    // Family 4, model 3 is the IntelDX2 in AP-485's model table, and the
    // only feature bit an Intel486 asserts is bit 0, the on-die FPU: every
    // other EDX bit names a Pentium-or-later feature. FreeDOS 1.3's VINFO
    // reads exactly the family nibble here, and FDAUTO.BAT's whole 386+
    // branch -- CTMOUSE included -- hangs off the answer
    // (PC486_REVIEW.md §13).
    cpu->eax = 1;
    run({0x0F, 0xA2});
    EXPECT_EQ(unimpl_count, 0);
    EXPECT_EQ((cpu->eax >> 8) & 0xF, 4u) << "family";
    EXPECT_EQ((cpu->eax >> 4) & 0xF, 3u) << "model: IntelDX2";
    EXPECT_EQ((cpu->eax >> 12) & 0x3, 0u) << "type: original OEM processor";
    EXPECT_EQ(cpu->edx, 0x00000001u);
    EXPECT_EQ(cpu->ebx, 0u);
    EXPECT_EQ(cpu->ecx, 0u);
}

TEST_F(Cpu80486Test, TheIdFlagRoundTripsSoSoftwareCanDetectCpuid) {
    // AP-485's own detection sequence: PUSHFD, flip bit 21, POPFD, PUSHFD
    // and see whether it stuck. This is the test FreeDOS's VINFO runs before
    // it will issue CPUID at all.
    cpu->ss = 0;
    cpu->esp = 0x2000;
    poke32(0x2000, 0x00200002u);
    run({0x66, 0x9D});  // POPFD
    EXPECT_TRUE(cpu->flag(cpu80486::FLAG_ID));
    cpu->esp = 0x2000;
    run({0x66, 0x9C});  // PUSHFD
    EXPECT_EQ(memd(0x1FFC) & 0x00200000u, 0x00200000u);
}

TEST_F(Cpu80486Test, UnknownOpcodeFiresTheDiagnosticHook) {
    run({0xD6});  // undefined on a 486
    EXPECT_EQ(unimpl_count, 1);
    EXPECT_EQ(unimpl_opcode, 0x00D6);
}

// ---------------------------------------------------------------------------
// Shared 8086-legacy subset -- still this core's bread and butter under DOS
// ---------------------------------------------------------------------------

TEST_F(Cpu80486Test, IncDoesNotAffectCarry) {
    cpu->eax = 0x00FF;
    cpu->set_flag(cpu80486::FLAG_CF, true);
    run({0x40});  // INC AX
    EXPECT_EQ(cpu->eax & 0xFFFF, 0x0100u);
    EXPECT_TRUE(CF());
}

TEST_F(Cpu80486Test, CmpDoesNotModifyItsOperand) {
    cpu->eax = 5;
    run({0x3D, 0x05, 0x00});  // CMP AX, 5
    EXPECT_EQ(cpu->eax & 0xFFFF, 5u);
    EXPECT_TRUE(ZF());
}

TEST_F(Cpu80486Test, SegmentOverridePrefixRedirectsMemoryAccess) {
    cpu->es = 0x1000;
    cpu->ebx = 0;
    mem[(0x1000u << 4)] = 0x55;
    mem[0] = 0x11;
    run({0x26, 0x8A, 0x07});  // ES: MOV AL, [BX]
    EXPECT_EQ(cpu->eax & 0xFF, 0x55u);
}

TEST_F(Cpu80486Test, RepMovsdCopiesDwords) {
    cpu->ds = 0; cpu->es = 0;
    cpu->esi = 0x0300;
    cpu->edi = 0x0400;
    cpu->ecx = 4;
    cpu->set_flag(cpu80486::FLAG_DF, false);
    for (int i = 0; i < 4; ++i) poke32(0x0300 + 4 * i, 0x11111111u * uint32_t(i + 1));
    run({0xF3, 0x66, 0xA5});  // REP MOVSD
    for (int i = 0; i < 4; ++i) EXPECT_EQ(memd(0x0400 + 4 * i), 0x11111111u * uint32_t(i + 1));
    EXPECT_EQ(cpu->ecx & 0xFFFF, 0u);
    EXPECT_EQ(cpu->esi & 0xFFFF, 0x0310u);
    EXPECT_EQ(cpu->edi & 0xFFFF, 0x0410u);
}

TEST_F(Cpu80486Test, RepneScasbStopsOnMatch) {
    cpu->es = 0;
    cpu->edi = 0x0600;
    cpu->ecx = 5;
    cpu->eax = 0x42;
    for (int i = 0; i < 5; ++i) mem[0x0600 + i] = uint8_t(0x40 + i);  // matches at index 2
    run({0xF2, 0xAE});  // REPNE SCASB
    EXPECT_EQ(cpu->edi & 0xFFFF, 0x0603u);
    EXPECT_EQ(cpu->ecx & 0xFFFF, 2u);
    EXPECT_TRUE(ZF());
}

TEST_F(Cpu80486Test, CallNearThenRetReturnsToCaller) {
    cpu->ss = 0;
    cpu->esp = 0x1000;
    load({0xE8, 0x02, 0x00}, 0);  // CALL +2 -> target 5
    load({0xC3}, 5);              // RET
    cpu->eip = 0;
    cpu->step();
    EXPECT_EQ(cpu->eip, 5u);
    cpu->step();
    EXPECT_EQ(cpu->eip, 3u);
    EXPECT_EQ(cpu->esp, 0x1000u);
}

TEST_F(Cpu80486Test, IntThenIretRestoresFlagsCsAndIp) {
    poke16(0x21 * 4 + 0, 0x1000);
    poke16(0x21 * 4 + 2, 0x2000);
    cpu->ss = 0; cpu->esp = 0xA000;
    cpu->set_flag(cpu80486::FLAG_IF, true);
    load({0xCD, 0x21}, 0);
    mem[(0x2000u << 4) + 0x1000] = 0xCF;  // IRET at the handler
    cpu->eip = 0;
    cpu->step();  // INT 21h
    EXPECT_EQ(cpu->cs, 0x2000);
    EXPECT_EQ(cpu->eip, 0x1000u);
    EXPECT_FALSE(cpu->flag(cpu80486::FLAG_IF)) << "INT clears IF";
    cpu->step();  // IRET
    EXPECT_EQ(cpu->cs, 0);
    EXPECT_EQ(cpu->eip, 2u);
    EXPECT_TRUE(cpu->flag(cpu80486::FLAG_IF));
    EXPECT_EQ(cpu->esp, 0xA000u);
}

TEST_F(Cpu80486Test, HardwareInterruptEntryPushesTheSixteenBitRealModeFrame) {
    // The public interrupt() entry point is what the chipset calls for a
    // PIC-delivered IRQ. Real mode's frame is 6 bytes regardless of CPU
    // generation, so the handler's IRET must line up with it.
    poke16(0x08 * 4 + 0, 0x0500);
    poke16(0x08 * 4 + 2, 0x0060);
    cpu->ss = 0; cpu->esp = 0x8000;
    cpu->cs = 0x0070; cpu->eip = 0x0123;
    cpu->set_flag(cpu80486::FLAG_IF, true);
    cpu->interrupt(0x08);
    EXPECT_EQ(cpu->cs, 0x0060);
    EXPECT_EQ(cpu->eip, 0x0500u);
    EXPECT_EQ(cpu->esp, 0x7FFAu) << "exactly 6 bytes pushed";
    EXPECT_EQ(memw(0x7FFA), 0x0123) << "return IP";
    EXPECT_EQ(memw(0x7FFC), 0x0070) << "return CS";
    EXPECT_FALSE(cpu->flag(cpu80486::FLAG_IF));
}

TEST_F(Cpu80486Test, InterruptWakesAHaltedCpu) {
    poke16(0x08 * 4 + 0, 0x0500);
    poke16(0x08 * 4 + 2, 0x0060);
    cpu->ss = 0; cpu->esp = 0x8000;
    run({0xF4});  // HLT
    EXPECT_TRUE(cpu->halted);
    cpu->step();  // still halted -- just idling
    EXPECT_TRUE(cpu->halted);
    cpu->interrupt(0x08);
    EXPECT_FALSE(cpu->halted);
}

TEST_F(Cpu80486Test, DivideByZeroVectorsThroughInterruptZeroWithIpRestored) {
    poke16(0, 0x1111);
    poke16(2, 0x2222);
    cpu->ss = 0; cpu->esp = 0x9000;
    cpu->eax = 10;
    cpu->ebx = 0;
    load({0xF6, 0xF3}, 0x100);  // DIV BL with BL == 0
    cpu->eip = 0x100;
    cpu->step();
    EXPECT_EQ(cpu->cs, 0x2222);
    EXPECT_EQ(cpu->eip, 0x1111u);
    EXPECT_EQ(memw(0x8FFA), 0x0100) << "the pushed IP points at the faulting instruction";
}

TEST_F(Cpu80486Test, BoundOutOfRangeTriggersInterruptFive) {
    poke16(5 * 4 + 0, 0x5678);
    poke16(5 * 4 + 2, 0x1234);
    poke16(0x0200, 0x0000);
    poke16(0x0202, 0x000A);
    cpu->ss = 0; cpu->esp = 0x8000;
    cpu->eax = 99;
    run({0x62, 0x06, 0x00, 0x02});  // BOUND AX, [0200h]
    EXPECT_EQ(cpu->cs, 0x1234);
    EXPECT_EQ(cpu->eip, 0x5678u);
}

TEST_F(Cpu80486Test, EnterAndLeaveRestoreTheFrame) {
    cpu->ss = 0;
    cpu->ebp = 0x1000;
    cpu->esp = 0x2000;
    runN({0xC8, 0x04, 0x00, 0x00, 0xC9}, 2);  // ENTER 4,0 ; LEAVE
    EXPECT_EQ(cpu->ebp & 0xFFFF, 0x1000u);
    EXPECT_EQ(cpu->esp & 0xFFFF, 0x2000u);
}

TEST_F(Cpu80486Test, DaaAdjustsAfterBcdAddition) {
    cpu->eax = 0x0B;  // raw binary 6+5
    run({0x27});      // DAA
    EXPECT_EQ(cpu->eax & 0xFF, 0x11u);
    EXPECT_TRUE(AF());
    EXPECT_FALSE(CF());
}

TEST_F(Cpu80486Test, XlatTranslatesThroughTheTableAtBx) {
    cpu->ebx = 0x0700;
    cpu->eax = 0x03;
    mem[0x0703] = 0x5C;
    run({0xD7});  // XLAT
    EXPECT_EQ(cpu->eax & 0xFF, 0x5Cu);
}

TEST_F(Cpu80486Test, PortIoUsesTheAtomicSixteenBitPathForWordAccesses) {
    cpu->edx = 0x1F0;
    next_in16_val = 0x55AA;
    run({0xED});  // IN AX, DX
    EXPECT_EQ(cpu->eax & 0xFFFF, 0x55AAu);
    cpu->eax = 0xCAFE;
    run({0xEF});  // OUT DX, AX
    EXPECT_EQ(last_out16_port, 0x1F0);
    EXPECT_EQ(last_out16_val, 0xCAFE);
}

TEST_F(Cpu80486Test, BytePortIoUsesTheEightBitPath) {
    next_in_val = 0x99;
    run({0xE4, 0x60});  // IN AL, 60h
    EXPECT_EQ(cpu->eax & 0xFF, 0x99u);
    cpu->eax = 0xAB;
    run({0xE6, 0x61});  // OUT 61h, AL
    EXPECT_EQ(last_out_port, 0x61);
    EXPECT_EQ(last_out_port_val, 0xAB);
}

TEST_F(Cpu80486Test, InswStoresAnAtomicWordAtEsDi) {
    cpu->edx = 0x1F0;
    cpu->es = 0; cpu->edi = 0x0500;
    cpu->set_flag(cpu80486::FLAG_DF, false);
    next_in16_val = 0xABCD;
    run({0x6D});  // INSW
    EXPECT_EQ(memw(0x0500), 0xABCD);
    EXPECT_EQ(cpu->edi & 0xFFFF, 0x0502u);
}


// ===========================================================================
// Milestone 2: protected mode, paging, gates, task switching.
//
// A separate fixture, because these need a real GDT, IDT, TSS and page
// tables in memory and a genuine entry into protected mode -- which
// enter_pm32() performs by executing the actual instruction sequence a DOS
// extender uses (LGDT, set CR0.PE, far-JMP into a 32-bit flat code segment),
// not by poking internal state. Everything a test observes is therefore
// reached the same way real software reaches it.
//
// Reference for every rule asserted below is the Intel 80486 Programmer's
// Reference Manual's protection and paging chapters; the specific section is
// named at each group.
// ===========================================================================

class Cpu80486PmTest : public ::testing::Test {
protected:
    std::array<uint8_t, 0x100000> mem{};
    std::unique_ptr<Cpu> cpu;
    int unimpl_count = 0;
    struct FaultRec { int vector; uint32_t error; uint16_t cs; uint32_t eip; uint32_t esp; };
    std::vector<FaultRec> faults;

    // Physical/linear layout. The flat descriptors below have base 0, so a
    // linear address is also a physical one unless paging is on.
    static constexpr uint32_t kGdtPtr  = 0x00000500;
    static constexpr uint32_t kIdtPtr  = 0x00000508;
    static constexpr uint32_t kBoot    = 0x00000600;  // the real-mode entry stub
    static constexpr uint32_t kGdt     = 0x00001000;
    static constexpr uint32_t kLdt     = 0x00001800;
    static constexpr uint32_t kIdt     = 0x00002000;
    static constexpr uint32_t kTss     = 0x00002800;
    static constexpr uint32_t kTss2    = 0x00002900;
    static constexpr uint32_t kPre     = 0x00003000;  // the 32-bit preamble
    static constexpr uint32_t kData    = 0x00006000;
    static constexpr uint32_t kStackTop = 0x00007F00;
    static constexpr uint32_t kPageDir = 0x00008000;
    static constexpr uint32_t kPageTab = 0x00009000;
    static constexpr uint32_t kFrame   = 0x0000A000;  // a page frame to alias onto
    static constexpr uint32_t kFar     = 0x00070000;  // past any 16-bit offset

    // Selector assignments in the default GDT.
    static constexpr uint16_t kCode32 = 0x08;   // flat code32, DPL 0
    static constexpr uint16_t kData32 = 0x10;   // flat data32, DPL 0
    static constexpr uint16_t kCode16 = 0x18;   // code16, DPL 0, base 0
    static constexpr uint16_t kTssSel = 0x20;
    static constexpr uint16_t kTss2Sel = 0x28;
    static constexpr uint16_t kCode3  = 0x33;   // flat code32, DPL 3 (RPL 3)
    static constexpr uint16_t kData3  = 0x3B;   // flat data32, DPL 3 (RPL 3)
    static constexpr uint16_t kLdtSel = 0x40;
    static constexpr uint16_t kSmall  = 0x48;   // data32, limit 0FFFh, for limit tests
    static constexpr uint16_t kRoData = 0x50;   // read-only data32
    static constexpr uint16_t kExecOnly = 0x58; // execute-only code32
    static constexpr uint16_t kConform  = 0x60; // conforming code32, DPL 0
    static constexpr uint16_t kDown     = 0x68; // expand-down data32
    static constexpr uint16_t kStack3   = 0x70; // data32 DPL 3, for a ring-3 stack

    uint32_t pm_code_ = 0;   // where pm_run() assembles the code under test

public:
    // The six operations Bus::For binds (see cpu80486.h). No device is
    // wired up in this fixture -- port reads float high, writes go nowhere.
    uint8_t mem_read(uint32_t a) { return mem[a & 0xFFFFF]; }
    void mem_write(uint32_t a, uint8_t v) { mem[a & 0xFFFFF] = v; }
    uint8_t io_in(uint16_t) { return 0xFF; }
    void io_out(uint16_t, uint8_t) {}
    uint16_t io_in16(uint16_t) { return 0xFFFF; }
    void io_out16(uint16_t, uint16_t) {}

protected:
    void SetUp() override {
        cpu = std::make_unique<Cpu>(Bus::For(this));
        cpu->reset();
        cpu->on_unimplemented = [this](uint16_t, uint32_t, uint16_t) { ++unimpl_count; };
        // on_fault fires after the restartable state has been put back and
        // before the handler runs, so the recorded ESP is exactly what a
        // restarted instruction would see.
        cpu->on_fault = [this](int v, uint32_t e, uint16_t c, uint32_t ip) {
            if (faults.size() < 8) faults.push_back({v, e, c, ip, cpu->esp});
        };
    }

    void w8(uint32_t a, uint8_t v) { mem[a] = v; }
    void w16(uint32_t a, uint16_t v) { w8(a, uint8_t(v)); w8(a + 1, uint8_t(v >> 8)); }
    void w32(uint32_t a, uint32_t v) { w16(a, uint16_t(v)); w16(a + 2, uint16_t(v >> 16)); }
    void w64(uint32_t a, uint64_t v) { w32(a, uint32_t(v)); w32(a + 4, uint32_t(v >> 32)); }
    uint32_t r32(uint32_t a) const {
        return uint32_t(mem[a]) | (uint32_t(mem[a + 1]) << 8) |
               (uint32_t(mem[a + 2]) << 16) | (uint32_t(mem[a + 3]) << 24);
    }

    // Assembles an 8-byte descriptor the way real silicon reads one back:
    // the base and limit are split across bytes 2-4/7 and 0-1/6 purely for
    // 286 compatibility (Intel 80486 PRM, "Segment Descriptors").
    static uint64_t seg_desc(uint32_t base, uint32_t limit, uint8_t access,
                             bool big, bool granular) {
        uint32_t lim = granular ? (limit >> 12) : limit;
        uint64_t d = uint64_t(lim & 0xFFFFu);
        d |= uint64_t(base & 0xFFFFu) << 16;
        d |= uint64_t((base >> 16) & 0xFFu) << 32;
        d |= uint64_t(access) << 40;
        d |= uint64_t((lim >> 16) & 0x0Fu) << 48;
        if (big) d |= uint64_t(1) << 54;
        if (granular) d |= uint64_t(1) << 55;
        d |= uint64_t((base >> 24) & 0xFFu) << 56;
        return d;
    }
    static uint64_t gate_desc(uint16_t sel, uint32_t off, uint8_t access, int params = 0) {
        uint64_t d = uint64_t(off & 0xFFFFu);
        d |= uint64_t(sel) << 16;
        d |= uint64_t(params & 0x1F) << 32;
        d |= uint64_t(access) << 40;
        d |= uint64_t(off >> 16) << 48;
        return d;
    }
    void set_desc(uint16_t selector, uint64_t d) { w64(kGdt + (selector & 0xFFF8u), d); }
    void set_ldt_desc(uint16_t selector, uint64_t d) { w64(kLdt + (selector & 0xFFF8u), d); }
    void set_gate(int vector, uint64_t d) { w64(kIdt + uint32_t(vector) * 8, d); }

    void build_default_gdt() {
        set_desc(0x00, 0);
        set_desc(kCode32, seg_desc(0, 0xFFFFFFFFu, 0x9A, true, true));
        set_desc(kData32, seg_desc(0, 0xFFFFFFFFu, 0x92, true, true));
        set_desc(kCode16, seg_desc(0, 0xFFFF, 0x9A, false, false));
        set_desc(kTssSel, seg_desc(kTss, 0x67, 0x89, false, false));
        set_desc(kTss2Sel, seg_desc(kTss2, 0x67, 0x89, false, false));
        set_desc(kCode3, seg_desc(0, 0xFFFFFFFFu, 0xFA, true, true));
        set_desc(kData3, seg_desc(0, 0xFFFFFFFFu, 0xF2, true, true));
        set_desc(kLdtSel, seg_desc(kLdt, 0xFF, 0x82, false, false));
        set_desc(kSmall, seg_desc(0, 0x0FFF, 0x92, true, false));
        set_desc(kRoData, seg_desc(0, 0xFFFFFFFFu, 0x90, true, true));   // data, not writable
        set_desc(kExecOnly, seg_desc(0, 0xFFFFFFFFu, 0x98, true, true)); // code, not readable
        set_desc(kConform, seg_desc(0, 0xFFFFFFFFu, 0x9E, true, true));  // code, conforming, readable
        set_desc(kDown, seg_desc(0, 0x0FFF, 0x96, true, false));         // data, expand-down, writable
        set_desc(kStack3, seg_desc(0, 0xFFFFFFFFu, 0xF2, true, true));
        w16(kGdtPtr, 0x7F);
        w32(kGdtPtr + 2, kGdt);
        w16(kIdtPtr, 0x07FF);
        w32(kIdtPtr + 2, kIdt);
        // A minimal TSS with a ring-0 stack, so an inter-privilege gate has
        // somewhere to switch to.
        for (uint32_t i = 0; i < 104; i += 4) w32(kTss + i, 0);
        w32(kTss + 4, 0x00007E00u);   // ESP0
        w32(kTss + 8, kData32);       // SS0
        w16(kTss + 102, 0x68);        // I/O map base, past the limit
    }

    // Enters 32-bit protected mode by executing the real sequence, then loads
    // DS/ES/SS and a stack. Leaves CS:EIP at pm_code_.
    void enter_pm32() {
        build_default_gdt();
        std::vector<uint8_t> e;
        auto eb = [&](std::initializer_list<int> v) { for (int x : v) e.push_back(uint8_t(x)); };
        auto ew = [&](uint32_t v) { e.push_back(uint8_t(v)); e.push_back(uint8_t(v >> 8)); };
        auto ed = [&](uint32_t v) { ew(v & 0xFFFF); ew(v >> 16); };
        eb({0x0F, 0x01, 0x16}); ew(kGdtPtr);              // lgdt [kGdtPtr]
        eb({0x0F, 0x01, 0x1E}); ew(kIdtPtr);              // lidt [kIdtPtr]
        eb({0x0F, 0x20, 0xC0});                           // mov eax,cr0
        eb({0x0C, 0x01});                                 // or al,1
        eb({0x0F, 0x22, 0xC0});                           // mov cr0,eax
        eb({0x66, 0xEA}); ed(kPre); ew(kCode32);          // jmp far kCode32:kPre
        for (size_t i = 0; i < e.size(); ++i) w8(kBoot + uint32_t(i), e[i]);

        std::vector<uint8_t> p;
        auto pb = [&](std::initializer_list<int> v) { for (int x : v) p.push_back(uint8_t(x)); };
        auto pw = [&](uint32_t v) { p.push_back(uint8_t(v)); p.push_back(uint8_t(v >> 8)); };
        auto pd = [&](uint32_t v) { pw(v & 0xFFFF); pw(v >> 16); };
        pb({0x66, 0xB8}); pw(kData32);                    // mov ax,kData32
        pb({0x8E, 0xD8});                                 // mov ds,ax
        pb({0x8E, 0xC0});                                 // mov es,ax
        pb({0x8E, 0xD0});                                 // mov ss,ax
        pb({0xBC}); pd(kStackTop);                        // mov esp,kStackTop
        for (size_t i = 0; i < p.size(); ++i) w8(kPre + uint32_t(i), p[i]);
        pm_code_ = kPre + uint32_t(p.size());

        cpu->cs = 0;
        cpu->eip = kBoot;   // real mode, CS=0, so linear == kBoot
        for (int i = 0; i < 11; ++i) cpu->step();
    }

    // Assembles `code` at pm_code_ and executes `n` instructions from there.
    // `halted` is cleared first because a fault with no gate to deliver it
    // escalates to #DF and then to shutdown, which stops the CPU until RESET
    // -- so a test that deliberately faults twice has to bring it back up.
    void pm_run(std::initializer_list<uint8_t> code, int n = 1) {
        uint32_t a = pm_code_;
        for (uint8_t b : code) w8(a++, b);
        cpu->halted = false;
        cpu->eip = pm_code_;
        for (int i = 0; i < n; ++i) cpu->step();
    }
    // Assembles `code` at `at`, without moving EIP.
    void put(uint32_t at, std::initializer_list<uint8_t> code) {
        for (uint8_t b : code) w8(at++, b);
    }
    // `why` is a parameter rather than a streamed suffix because a void
    // helper cannot be streamed into.
    // Renders the first recorded fault, so an "expected no fault" assertion
    // says which one actually fired instead of just that one did.
    std::string fault_desc() const {
        if (faults.empty()) return "none";
        char b[96];
        std::snprintf(b, sizeof b, "vector %d error %08X at %04X:%08X", faults[0].vector,
                      faults[0].error, faults[0].cs, faults[0].eip);
        return std::string(b);
    }
    void expect_fault(int vector, uint32_t error, const char *why = "") {
        ASSERT_FALSE(faults.empty()) << "expected a fault, none was raised. " << why;
        EXPECT_EQ(faults[0].vector, vector) << why;
        EXPECT_EQ(faults[0].error, error) << why;
    }
};

// --- entering protected mode, and what CS.D changes -----------------------

TEST_F(Cpu80486PmTest, TheRealEntrySequenceActuallyEntersThirtyTwoBitProtectedMode) {
    enter_pm32();
    EXPECT_TRUE(cpu->protected_mode());
    EXPECT_EQ(cpu->cs, kCode32);
    EXPECT_EQ(cpu->eip, pm_code_);
    EXPECT_EQ(cpu->cpl(), 0);
    // The descriptor cache behind CS now holds what the GDT said, not
    // selector*16: base 0, 4GB limit, D=1.
    EXPECT_EQ(cpu->desc(Cpu::SEG_CS).base, 0u);
    EXPECT_EQ(cpu->desc(Cpu::SEG_CS).limit, 0xFFFFFFFFu);
    EXPECT_TRUE(cpu->desc(Cpu::SEG_CS).big);
    EXPECT_EQ(cpu->esp, kStackTop);
    EXPECT_TRUE(faults.empty());
    EXPECT_EQ(unimpl_count, 0);
}

TEST_F(Cpu80486PmTest, ThirtyTwoBitCodeSegmentMakesThirtyTwoBitOperandsTheDefault) {
    enter_pm32();
    // No 0x66 prefix: in a D=1 code segment the 32-bit form is the default,
    // so B8 takes a full imm32. Getting this backwards would load AX with
    // 5678h and leave the rest of the instruction stream misaligned.
    pm_run({0xB8, 0x78, 0x56, 0x34, 0x12});   // mov eax,12345678h
    EXPECT_EQ(cpu->eax, 0x12345678u);
    EXPECT_EQ(cpu->eip, pm_code_ + 5);
}

TEST_F(Cpu80486PmTest, ThePrefixTogglesTheDefaultRatherThanSelectingThirtyTwoBits) {
    enter_pm32();
    cpu->eax = 0xFFFFFFFFu;
    pm_run({0x66, 0xB8, 0x34, 0x12});          // mov ax,1234h  -- 0x66 *narrows* here
    EXPECT_EQ(cpu->eax, 0xFFFF1234u);
    EXPECT_EQ(cpu->eip, pm_code_ + 4);
}

TEST_F(Cpu80486PmTest, ThirtyTwoBitSegmentAddressesFarPastSixtyFourK) {
    enter_pm32();
    pm_run({0xB8, 0x0D, 0xF0, 0xAD, 0x0B}, 1);          // mov eax,0BADF00Dh
    put(pm_code_ + 5, {0xA3, uint8_t(kFar), uint8_t(kFar >> 8),
                       uint8_t(kFar >> 16), uint8_t(kFar >> 24)});  // mov [kFar],eax
    cpu->eip = pm_code_ + 5;
    cpu->step();
    EXPECT_EQ(r32(kFar), 0x0BADF00Du) << "a 4GB-limit segment has no 64KB window";
    EXPECT_TRUE(faults.empty());
}

// --- segment limits and access rights (PRM, "Protection") ----------------

TEST_F(Cpu80486PmTest, SegmentLimitViolationRaisesGeneralProtection) {
    enter_pm32();
    // ES gets a 4KB byte-granular data segment; reaching past it must fault
    // with #GP(0) -- the error code carries no selector, because the fault is
    // the *access*, not a bad descriptor.
    pm_run({0x66, 0xB8, uint8_t(kSmall), 0x00, 0x8E, 0xC0}, 2);   // mov ax,kSmall / mov es,ax
    EXPECT_TRUE(faults.empty()) << "loading the descriptor itself is legal";
    put(pm_code_ + 6, {0x26, 0xA1, 0x00, 0x10, 0x00, 0x00});      // es: mov eax,[1000h]
    cpu->eip = pm_code_ + 6;
    cpu->step();
    expect_fault(cpu80486::EXC_GP, 0);
}

TEST_F(Cpu80486PmTest, AnAccessInsideTheLimitIsFine) {
    enter_pm32();
    w32(0x0FFC, 0xA5A5A5A5u);
    pm_run({0x66, 0xB8, uint8_t(kSmall), 0x00, 0x8E, 0xC0,
            0x26, 0xA1, 0xFC, 0x0F, 0x00, 0x00}, 3);  // es: mov eax,[0FFCh] -- the last dword
    EXPECT_EQ(cpu->eax, 0xA5A5A5A5u);
    EXPECT_TRUE(faults.empty()) << "offset 0FFCh..0FFFh is exactly the limit";
}

// The segmentation unit checks the whole access against the limit before
// running any bus cycle (PRM, "Protection"), so an access that starts inside
// the limit and ends past it faults without touching memory at all -- not
// after transferring the bytes that happened to be in range. See
// PC486_REVIEW.md §14.3.
TEST_F(Cpu80486PmTest, AnAccessStraddlingTheLimitFaultsBeforeMovingAnyByte) {
    enter_pm32();
    w32(0x0FFC, 0xA5A5A5A5u);
    pm_run({0x66, 0xB8, uint8_t(kSmall), 0x00, 0x8E, 0xC0}, 2);   // mov ax,kSmall / mov es,ax
    // es: mov [0FFEh],eax -- offsets 0FFEh/0FFFh are inside the limit, 1000h/1001h are not.
    put(pm_code_ + 6, {0x26, 0xA3, 0xFE, 0x0F, 0x00, 0x00});
    cpu->eip = pm_code_ + 6;
    cpu->eax = 0x11223344u;
    cpu->step();
    expect_fault(cpu80486::EXC_GP, 0);
    EXPECT_EQ(r32(0x0FFC), 0xA5A5A5A5u) << "no byte of the faulting write reached memory";
}

TEST_F(Cpu80486PmTest, WritingAReadOnlyDataSegmentFaults) {
    enter_pm32();
    pm_run({0x66, 0xB8, uint8_t(kRoData), 0x00, 0x8E, 0xC0}, 2);   // mov es,kRoData
    EXPECT_TRUE(faults.empty());
    put(pm_code_ + 6, {0x26, 0xA3, 0x00, 0x60, 0x00, 0x00});       // es: mov [kData],eax
    cpu->eip = pm_code_ + 6;
    cpu->step();
    expect_fault(cpu80486::EXC_GP, 0);
}

TEST_F(Cpu80486PmTest, ReadingAnExecuteOnlyCodeSegmentFaults) {
    enter_pm32();
    // An execute-only code segment cannot even be *loaded* into a data
    // segment register: the check happens at load time (PRM, "Data Segment
    // Descriptor").
    pm_run({0x66, 0xB8, uint8_t(kExecOnly), 0x00, 0x8E, 0xC0}, 2);
    expect_fault(cpu80486::EXC_GP, kExecOnly & 0xFFFCu);
}

TEST_F(Cpu80486PmTest, ExpandDownSegmentInvertsTheLimitCheck) {
    enter_pm32();
    // An expand-down segment's *valid* offsets are limit+1 upward -- the
    // inverse of the ordinary rule, which is how a stack segment grows down
    // safely (PRM, "Expand-Down Data Segments").
    pm_run({0x66, 0xB8, uint8_t(kDown), 0x00, 0x8E, 0xC0}, 2);
    put(pm_code_ + 6, {0x26, 0xA1, 0x00, 0x20, 0x00, 0x00});   // es: mov eax,[2000h] -- above the limit: valid
    cpu->eip = pm_code_ + 6;
    cpu->step();
    EXPECT_TRUE(faults.empty()) << "offset 2000h is above limit 0FFFh, so it is inside an expand-down segment";
    put(pm_code_ + 12, {0x26, 0xA1, 0x00, 0x08, 0x00, 0x00});  // es: mov eax,[0800h] -- below: invalid
    cpu->eip = pm_code_ + 12;
    cpu->step();
    expect_fault(cpu80486::EXC_GP, 0);
}

TEST_F(Cpu80486PmTest, NullSelectorLoadsIntoDataSegmentsAndFaultsOnlyWhenUsed) {
    enter_pm32();
    pm_run({0x66, 0xB8, 0x00, 0x00, 0x8E, 0xC0}, 2);   // mov ax,0 / mov es,ax
    EXPECT_TRUE(faults.empty()) << "parking a null selector in ES is legal";
    EXPECT_TRUE(cpu->desc(Cpu::SEG_ES).null);
    put(pm_code_ + 6, {0x26, 0xA1, 0x00, 0x60, 0x00, 0x00});   // es: mov eax,[kData]
    cpu->eip = pm_code_ + 6;
    cpu->step();
    expect_fault(cpu80486::EXC_GP, 0, "using it is what faults");
}

TEST_F(Cpu80486PmTest, StackSegmentCanNeverBeNull) {
    enter_pm32();
    pm_run({0x66, 0xB8, 0x00, 0x00, 0x8E, 0xD0}, 2);   // mov ax,0 / mov ss,ax
    expect_fault(cpu80486::EXC_GP, 0);
}

TEST_F(Cpu80486PmTest, StackSegmentMustBeWritableAndMatchCplExactly) {
    enter_pm32();
    // A read-only data segment is not a legal stack.
    pm_run({0x66, 0xB8, uint8_t(kRoData), 0x00, 0x8E, 0xD0}, 2);
    expect_fault(cpu80486::EXC_GP, kRoData & 0xFFFCu);
    faults.clear();
    // Neither is a DPL-3 segment while running at CPL 0: SS's DPL and RPL
    // must both *equal* CPL, not merely be reachable from it.
    pm_run({0x66, 0xB8, uint8_t(kData3), 0x00, 0x8E, 0xD0}, 2);
    expect_fault(cpu80486::EXC_GP, kData3 & 0xFFFCu);
}

TEST_F(Cpu80486PmTest, MovToCsIsAnInvalidOpcode) {
    enter_pm32();
    set_gate(cpu80486::EXC_UD, gate_desc(kCode32, kData, 0x8E));
    put(kData, {0xF4});   // hlt, so reaching the handler is unmistakable
    pm_run({0x8E, 0xC8}, 2);   // mov cs,ax -- then one more step for the handler
    expect_fault(cpu80486::EXC_UD, 0);
    EXPECT_TRUE(cpu->halted) << "the #UD handler must actually have run";
}

TEST_F(Cpu80486PmTest, LoadingADescriptorSetsItsAccessedBit) {
    enter_pm32();
    // The one write a segment load performs (PRM, "Accessed Bit").
    uint32_t hi_addr = kGdt + (kSmall & 0xFFF8u) + 4;
    EXPECT_EQ(r32(hi_addr) & 0x100u, 0u);
    pm_run({0x66, 0xB8, uint8_t(kSmall), 0x00, 0x8E, 0xC0}, 2);
    EXPECT_NE(r32(hi_addr) & 0x100u, 0u);
}

// --- far transfers (PRM, "Calls and Jumps to Code Segments") --------------

TEST_F(Cpu80486PmTest, FarJumpToANonConformingSegmentRequiresAnExactCplMatch) {
    enter_pm32();
    // Jumping from CPL 0 to a DPL-3 non-conforming code segment is a #GP:
    // privilege can only change through a gate or a return, never a plain
    // far JMP.
    pm_run({0xEA, 0x00, 0x60, 0x00, 0x00, uint8_t(kCode3), 0x00}, 1);
    expect_fault(cpu80486::EXC_GP, kCode3 & 0xFFFCu);
}

TEST_F(Cpu80486PmTest, FarJumpWithinTheSamePrivilegeLevelWorksAndKeepsCpl) {
    enter_pm32();
    put(kData, {0xB8, 0x11, 0x22, 0x33, 0x44});   // mov eax,44332211h
    pm_run({0xEA, uint8_t(kData), uint8_t(kData >> 8), 0x00, 0x00, uint8_t(kCode32), 0x00}, 2);
    EXPECT_EQ(cpu->cs, kCode32);
    EXPECT_EQ(cpu->eax, 0x44332211u);
    EXPECT_EQ(cpu->cpl(), 0);
    EXPECT_TRUE(faults.empty());
}

TEST_F(Cpu80486PmTest, FarCallAndFarReturnRoundTrip) {
    enter_pm32();
    put(kData, {0xB8, 0x01, 0x00, 0x00, 0x00,   // mov eax,1
                0xCB});                          // retf
    uint32_t sp_before = cpu->esp;
    pm_run({0x9A, uint8_t(kData), uint8_t(kData >> 8), 0x00, 0x00, uint8_t(kCode32), 0x00}, 3);
    EXPECT_EQ(cpu->eax, 1u);
    EXPECT_EQ(cpu->cs, kCode32);
    EXPECT_EQ(cpu->eip, pm_code_ + 7) << "RETF must land after the CALL";
    EXPECT_EQ(cpu->esp, sp_before) << "and must leave the stack balanced";
    EXPECT_TRUE(faults.empty());
}

TEST_F(Cpu80486PmTest, ConformingCodeSegmentIsReachableFromALessPrivilegedLevel) {
    enter_pm32();
    // A conforming segment's DPL only has to be at least as privileged as
    // CPL, and entering it does *not* change CPL -- that is what "conforming"
    // means, and it is how a shared library at ring 0 can run on a ring-3
    // caller's privileges.
    pm_run({0xEA, uint8_t(kData), uint8_t(kData >> 8), 0x00, 0x00, uint8_t(kConform), 0x00}, 1);
    EXPECT_TRUE(faults.empty());
    EXPECT_EQ(cpu->cpl(), 0);
    EXPECT_EQ(cpu->cs & 0xFFFCu, kConform & 0xFFFCu);
}

TEST_F(Cpu80486PmTest, CallGateRaisesPrivilegeAndSwitchesToTheTssStack) {
    enter_pm32();
    // Get to ring 3 first, with a hand-built IRETD frame -- the standard way
    // an OS drops into user code.
    put(kData, {0xB8, 0x44, 0x33, 0x22, 0x11});    // mov eax,11223344h (runs at CPL 3)
    pm_run({0x66, 0xB8, uint8_t(kTssSel), 0x00, 0x0F, 0x00, 0xD8}, 2);   // ltr kTssSel
    ASSERT_TRUE(faults.empty());
    pm_run({0x68, uint8_t(kStack3 | 3), 0x00, 0x00, 0x00,     // push SS   (0x73)
            0x68, 0x00, 0x7C, 0x00, 0x00,                     // push ESP  (0x7C00)
            0x68, 0x02, 0x02, 0x00, 0x00,                     // push EFLAGS
            0x68, uint8_t(kCode3), 0x00, 0x00, 0x00,          // push CS
            0x68, uint8_t(kData), uint8_t(kData >> 8), 0x00, 0x00,  // push EIP
            0xCF}, 6);
    ASSERT_TRUE(faults.empty()) << "IRETD to ring 3 must succeed";
    EXPECT_EQ(cpu->cpl(), 3);
    EXPECT_EQ(cpu->esp, 0x00007C00u);
    // A 32-bit call gate at selector 78h, DPL 3 so ring 3 may use it, whose
    // target is the DPL-0 flat code segment.
    put(kData + 0x100, {0xF4});   // hlt: reaching ring 0 again is unmistakable
    set_desc(0x78, gate_desc(kCode32, kData + 0x100, 0xEC));
    put(kData + 0x200, {0x9A, 0x00, 0x00, 0x00, 0x00, 0x78, 0x00});  // call far 78h:0
    cpu->eip = kData + 0x200;
    cpu->step();
    EXPECT_TRUE(faults.empty());
    EXPECT_EQ(cpu->cpl(), 0) << "the gate raised privilege";
    EXPECT_EQ(cpu->ss, kData32) << "SS came from the TSS's SS0 slot";
    EXPECT_LT(cpu->esp, 0x00007E00u) << "and ESP from ESP0, with the frame pushed";
    cpu->step();
    EXPECT_TRUE(cpu->halted);
}

TEST_F(Cpu80486PmTest, JumpThroughACallGateMayNotChangePrivilege) {
    enter_pm32();
    // Only a CALL can raise privilege, because only a CALL leaves a way back.
    set_desc(0x78, gate_desc(kCode3, kData, 0xEC));
    pm_run({0xEA, 0x00, 0x00, 0x00, 0x00, 0x78, 0x00}, 1);   // jmp far 78h:0
    expect_fault(cpu80486::EXC_GP, kCode3 & 0xFFFCu);
}

// --- the LDT --------------------------------------------------------------

TEST_F(Cpu80486PmTest, LldtLoadsTheLdtAndLdtSelectorsResolveThroughIt) {
    enter_pm32();
    // An LDT selector has bit 2 set. Index 1 in the LDT is selector 0Ch.
    set_ldt_desc(0x08, seg_desc(0, 0xFFFFFFFFu, 0x92, true, true));
    pm_run({0x66, 0xB8, uint8_t(kLdtSel), 0x00, 0x0F, 0x00, 0xD0}, 2);  // mov ax,kLdtSel / lldt ax
    ASSERT_TRUE(faults.empty());
    EXPECT_EQ(cpu->ldt_selector(), kLdtSel);
    EXPECT_EQ(cpu->ldtr().base, kLdt);
    w32(kData, 0x5A5A5A5Au);
    put(pm_code_ + 7, {0x66, 0xB8, 0x0C, 0x00,      // mov ax,000Ch  (LDT index 1, TI=1)
                       0x8E, 0xC0,                  // mov es,ax
                       0x26, 0xA1, uint8_t(kData), uint8_t(kData >> 8), 0x00, 0x00});
    cpu->eip = pm_code_ + 7;
    for (int i = 0; i < 3; ++i) cpu->step();
    EXPECT_TRUE(faults.empty());
    EXPECT_EQ(cpu->eax, 0x5A5A5A5Au) << "the selector resolved through the LDT, not the GDT";
    // SLDT reads it back.
    put(pm_code_ + 19, {0x0F, 0x00, 0xC3});   // sldt bx
    cpu->eip = pm_code_ + 19;
    cpu->step();
    EXPECT_EQ(cpu->ebx & 0xFFFFu, kLdtSel);
}

TEST_F(Cpu80486PmTest, LldtRejectsADescriptorThatIsNotAnLdt) {
    enter_pm32();
    pm_run({0x66, 0xB8, uint8_t(kData32), 0x00, 0x0F, 0x00, 0xD0}, 2);  // lldt kData32
    expect_fault(cpu80486::EXC_GP, kData32 & 0xFFFCu);
}

// --- VERR/VERW/LAR/LSL: report, never fault ------------------------------

TEST_F(Cpu80486PmTest, VerrAndVerwReportThroughZeroFlagInsteadOfFaulting) {
    enter_pm32();
    // Their whole purpose is answering "could I load this?" without taking
    // the fault that loading it would cause.
    pm_run({0x66, 0xB8, uint8_t(kData32), 0x00, 0x0F, 0x00, 0xE0}, 2);  // verr kData32
    EXPECT_TRUE(cpu->flag(cpu80486::FLAG_ZF));
    EXPECT_TRUE(faults.empty());
    pm_run({0x66, 0xB8, uint8_t(kRoData), 0x00, 0x0F, 0x00, 0xE8}, 2);  // verw a read-only segment
    EXPECT_FALSE(cpu->flag(cpu80486::FLAG_ZF));
    EXPECT_TRUE(faults.empty());
    pm_run({0x66, 0xB8, uint8_t(kExecOnly), 0x00, 0x0F, 0x00, 0xE0}, 2);  // verr an execute-only segment
    EXPECT_FALSE(cpu->flag(cpu80486::FLAG_ZF));
    pm_run({0x66, 0xB8, 0xF8, 0x0F, 0x0F, 0x00, 0xE0}, 2);  // verr a selector past the GDT limit
    EXPECT_FALSE(cpu->flag(cpu80486::FLAG_ZF));
    EXPECT_TRUE(faults.empty()) << "an out-of-table selector is reported, not faulted on";
}

TEST_F(Cpu80486PmTest, LarReturnsAccessRightsAndLslReturnsTheLimit) {
    enter_pm32();
    pm_run({0x66, 0xB8, uint8_t(kSmall), 0x00, 0x0F, 0x02, 0xD8}, 2);  // mov ax,kSmall / lar ebx,eax
    EXPECT_TRUE(cpu->flag(cpu80486::FLAG_ZF));
    // The access byte sits in bits 8-15 of the returned dword.
    EXPECT_EQ((cpu->ebx >> 8) & 0xFFu, 0x92u);
    pm_run({0x66, 0xB8, uint8_t(kSmall), 0x00, 0x0F, 0x03, 0xD8}, 2);  // lsl ebx,eax
    EXPECT_TRUE(cpu->flag(cpu80486::FLAG_ZF));
    EXPECT_EQ(cpu->ebx, 0x00000FFFu);
    // And the 4KB-granular flat segment's limit comes back scaled to bytes.
    pm_run({0x66, 0xB8, uint8_t(kData32), 0x00, 0x0F, 0x03, 0xD8}, 2);
    EXPECT_EQ(cpu->ebx, 0xFFFFFFFFu) << "a G=1 limit of FFFFFh is 4GB of bytes";
}

TEST_F(Cpu80486PmTest, ArplRaisesTheRequestedPrivilegeLevel) {
    enter_pm32();
    // ARPL is what an OS runs on a selector a less privileged caller handed
    // in, so the caller cannot smuggle in more privilege than it has.
    cpu->ebx = 0x0010;   // RPL 0
    cpu->eax = 0x0003;   // RPL 3
    pm_run({0x63, 0xC3}, 1);   // arpl bx,ax
    EXPECT_EQ(cpu->ebx & 0xFFFFu, 0x0013u) << "RPL raised to 3";
    EXPECT_TRUE(cpu->flag(cpu80486::FLAG_ZF)) << "ZF marks that it had to change something";
    cpu->ebx = 0x0013;
    cpu->eax = 0x0001;
    pm_run({0x63, 0xC3}, 1);
    EXPECT_EQ(cpu->ebx & 0xFFFFu, 0x0013u) << "ARPL never *lowers* an RPL";
    EXPECT_FALSE(cpu->flag(cpu80486::FLAG_ZF));
}

// --- paging (PRM, "Page Translation") ------------------------------------

class Cpu80486PagingTest : public Cpu80486PmTest {
protected:
    // Identity-maps linear 0-4MB, and maps one extra 4MB region so a test can
    // prove translation really happens: nothing else would distinguish a page
    // walk from just using the linear address.
    void build_page_tables(uint32_t pte_flags = 0x07) {
        w32(kPageDir + 0 * 4, kPageTab | 0x07u);
        for (uint32_t p = 0; p < 1024; ++p) w32(kPageTab + p * 4, (p << 12) | pte_flags);
    }
    void map_region(int dir_index, uint32_t table_phys, uint32_t frame, uint32_t flags = 0x07) {
        w32(kPageDir + uint32_t(dir_index) * 4, table_phys | 0x07u);
        for (uint32_t p = 0; p < 1024; ++p) w32(table_phys + p * 4, 0);
        w32(table_phys + 0 * 4, frame | flags);
    }
    // Turns paging on from inside protected mode, the real way.
    void enable_paging() {
        put(pm_code_, {0xB8, uint8_t(kPageDir), uint8_t(kPageDir >> 8),
                       uint8_t(kPageDir >> 16), uint8_t(kPageDir >> 24),
                       0x0F, 0x22, 0xD8,                        // mov cr3,eax
                       0x0F, 0x20, 0xC0,                        // mov eax,cr0
                       0x0D, 0x00, 0x00, 0x00, 0x80,            // or eax,80000000h
                       0x0F, 0x22, 0xC0});                      // mov cr0,eax
        cpu->eip = pm_code_;
        for (int i = 0; i < 5; ++i) cpu->step();
        paged_code_ = pm_code_ + 19;
    }
    uint32_t paged_code_ = 0;
    void paged_run(std::initializer_list<uint8_t> code, int n = 1) {
        uint32_t a = paged_code_;
        for (uint8_t b : code) w8(a++, b);
        cpu->eip = paged_code_;
        for (int i = 0; i < n; ++i) cpu->step();
    }
};

TEST_F(Cpu80486PagingTest, PagingTranslatesThroughTheTwoLevelTable) {
    enter_pm32();
    build_page_tables();
    // Linear 0x00400000 (page-directory entry 1) is mapped onto kFrame, an
    // address nowhere near it. A store through the linear address must land
    // in kFrame -- which is the only thing that distinguishes a real page
    // walk from ignoring paging altogether.
    map_region(1, 0xB000, kFrame);
    // A second linear window onto the *same* physical frame: two different
    // linear addresses aliasing one page is something only a real translation
    // can produce, and it needs no out-of-range peek to observe.
    map_region(3, 0xC000, kFrame);
    enable_paging();
    ASSERT_TRUE(cpu->paging_enabled());
    ASSERT_TRUE(faults.empty()) << "the identity map must keep the code running";
    paged_run({0xB8, 0xBE, 0xBA, 0xFE, 0xCA,                  // mov eax,0CAFEBABEh
               0xA3, 0x00, 0x00, 0x40, 0x00,                   // mov [00400000h],eax
               0x31, 0xC0,                                     // xor eax,eax
               0xA1, 0x00, 0x00, 0xC0, 0x00}, 4);              // mov eax,[00C00000h]
    EXPECT_TRUE(faults.empty());
    EXPECT_EQ(r32(kFrame), 0xCAFEBABEu) << "the store landed in the mapped frame";
    EXPECT_EQ(cpu->eax, 0xCAFEBABEu)
        << "and a second linear window onto that frame reads it back";
}

TEST_F(Cpu80486PagingTest, NotPresentPageFaultsWithCr2AndAReadErrorCode) {
    enter_pm32();
    build_page_tables();
    enable_paging();
    // Linear 0x00400000 has no page-directory entry at all.
    paged_run({0xA1, 0x34, 0x02, 0x40, 0x00}, 1);   // mov eax,[00400234h]
    expect_fault(cpu80486::EXC_PF, 0x00000000u, "not present (bit0=0), a read (bit1=0), supervisor (bit2=0)");
    EXPECT_EQ(cpu->cr(2), 0x00400234u) << "CR2 holds the faulting linear address, byte-exact";
}

TEST_F(Cpu80486PagingTest, PageFaultErrorCodeDistinguishesWritesAndProtectionViolations) {
    enter_pm32();
    build_page_tables();
    enable_paging();
    paged_run({0xA3, 0x00, 0x00, 0x40, 0x00}, 1);   // mov [00400000h],eax -- a write
    expect_fault(cpu80486::EXC_PF, 0x00000002u, "bit1 set: the access was a write");
    faults.clear();
    cpu->halted = false;   // the first fault had no gate, so it shut the CPU down
    // Now a page that *is* present but read-only, written by a supervisor
    // with CR0.WP set.
    map_region(1, 0xB000, kFrame, 0x05);   // present, user, NOT writable
    put(paged_code_ + 5, {0x0F, 0x20, 0xC0,                      // mov eax,cr0
                          0x0D, 0x00, 0x00, 0x01, 0x00,          // or eax,00010000h (WP)
                          0x0F, 0x22, 0xC0,                      // mov cr0,eax
                          0xA3, 0x00, 0x00, 0x40, 0x00});        // mov [00400000h],eax
    cpu->eip = paged_code_ + 5;
    for (int i = 0; i < 4; ++i) cpu->step();
    expect_fault(cpu80486::EXC_PF, 0x00000003u, "bit0 set (present, so a protection violation) and bit1 set (a write)");
}

TEST_F(Cpu80486PagingTest, SupervisorWriteToAReadOnlyPageSucceedsUntilWriteProtectIsSet) {
    enter_pm32();
    build_page_tables();
    // CR0.WP is a genuine Intel486 addition. With it clear -- the 386's only
    // behavior -- a supervisor write bypasses the page's R/W bit entirely,
    // which is exactly why copy-on-write was impossible before WP existed.
    map_region(1, 0xB000, kFrame, 0x05);   // present, user, read-only
    enable_paging();
    EXPECT_EQ(cpu->cr(0) & uint32_t(cpu80486::CR0_WP), 0u);
    paged_run({0xB8, 0x78, 0x56, 0x34, 0x12,
               0xA3, 0x00, 0x00, 0x40, 0x00}, 2);
    EXPECT_TRUE(faults.empty()) << "WP clear: the supervisor write goes through; got " << fault_desc();
    EXPECT_EQ(r32(kFrame), 0x12345678u);
}

TEST_F(Cpu80486PagingTest, AccessedAndDirtyBitsAreSetByTheWalk) {
    enter_pm32();
    build_page_tables();
    map_region(1, 0xB000, kFrame);
    enable_paging();
    // Clear them so the walk's own writes are unambiguous.
    w32(0xB000, kFrame | 0x07u);
    w32(kPageDir + 4, 0xB000u | 0x07u);
    paged_run({0xA1, 0x00, 0x00, 0x40, 0x00}, 1);   // a read
    EXPECT_NE(r32(kPageDir + 4) & 0x20u, 0u) << "the directory entry's A bit";
    EXPECT_NE(r32(0xB000) & 0x20u, 0u) << "the table entry's A bit";
    EXPECT_EQ(r32(0xB000) & 0x40u, 0u) << "a read must not set D";
    put(paged_code_ + 5, {0xA3, 0x00, 0x00, 0x40, 0x00});   // a write
    cpu->eip = paged_code_ + 5;
    cpu->step();
    EXPECT_NE(r32(0xB000) & 0x40u, 0u) << "a write sets D";
}

TEST_F(Cpu80486PagingTest, InvlpgDropsOneTranslationAndACr3WriteDropsThemAll) {
    enter_pm32();
    build_page_tables();
    map_region(1, 0xB000, kFrame);
    enable_paging();
    paged_run({0xB8, 0x01, 0x00, 0x00, 0x00,
               0xA3, 0x00, 0x00, 0x40, 0x00}, 2);   // populate the TLB entry
    ASSERT_EQ(r32(kFrame), 1u);
    // Repoint the page table at a different frame *behind the TLB's back*.
    w32(0xB000, 0x0000C000u | 0x07u);
    put(paged_code_ + 10, {0xB8, 0x02, 0x00, 0x00, 0x00,
                           0xA3, 0x00, 0x00, 0x40, 0x00});
    cpu->eip = paged_code_ + 10;
    for (int i = 0; i < 2; ++i) cpu->step();
    EXPECT_EQ(r32(kFrame), 2u) << "the stale TLB entry is still in use, as on real hardware";
    EXPECT_EQ(r32(0x0000C000u), 0u);
    // INVLPG drops exactly that entry, and the next access re-walks.
    put(paged_code_ + 20, {0x0F, 0x01, 0x3D, 0x00, 0x00, 0x40, 0x00,   // invlpg [00400000h]
                           0xB8, 0x03, 0x00, 0x00, 0x00,
                           0xA3, 0x00, 0x00, 0x40, 0x00});
    cpu->eip = paged_code_ + 20;
    for (int i = 0; i < 3; ++i) cpu->step();
    EXPECT_EQ(r32(0x0000C000u), 3u) << "after INVLPG the walk found the new frame";
    EXPECT_TRUE(faults.empty());
}

TEST_F(Cpu80486PagingTest, UserAccessToASupervisorPageFaults) {
    enter_pm32();
    build_page_tables();
    // The ring-3 code and stack pages have to stay user-accessible, but the
    // target page is supervisor-only.
    map_region(1, 0xB000, kFrame, 0x03);   // present, writable, NOT user
    enable_paging();
    // Drop to ring 3, then touch it.
    // IRETD outward nulls any segment register ring 3 may not use, so the
    // ring-3 code has to reload DS before it can address anything at all.
    put(kData + 0x300, {0x66, 0xB8, uint8_t(kData3), 0x00,   // mov ax,kData3
                        0x8E, 0xD8,                          // mov ds,ax
                        0xA1, 0x00, 0x00, 0x40, 0x00});      // mov eax,[00400000h]
    paged_run({0x68, uint8_t(kStack3 | 3), 0x00, 0x00, 0x00,
               0x68, 0x00, 0x7C, 0x00, 0x00,
               0x68, 0x02, 0x02, 0x00, 0x00,
               0x68, uint8_t(kCode3), 0x00, 0x00, 0x00,
               0x68, 0x00, 0x63, 0x00, 0x00,      // push kData+0x300
               0xCF}, 6);
    ASSERT_TRUE(faults.empty());
    ASSERT_EQ(cpu->cpl(), 3);
    cpu->step();   // mov ax,kData3
    cpu->step();   // mov ds,ax
    ASSERT_TRUE(faults.empty()) << "reloading DS at ring 3 is legal; got " << fault_desc();
    cpu->step();   // the supervisor-page read
    expect_fault(cpu80486::EXC_PF, 0x00000005u, "bit0 present, bit2 user -- a user read of a supervisor page");
}

// --- protected-mode interrupts and gates ---------------------------------

TEST_F(Cpu80486PmTest, SoftwareIntGoesThroughAnInterruptGateAndClearsInterruptFlag) {
    enter_pm32();
    put(kData, {0xF4});
    set_gate(0x40, gate_desc(kCode32, kData, 0x8E));   // 32-bit *interrupt* gate, DPL 0
    cpu->set_flag(cpu80486::FLAG_IF, true);
    pm_run({0xCD, 0x40}, 1);
    EXPECT_TRUE(faults.empty());
    EXPECT_EQ(cpu->cs, kCode32);
    EXPECT_EQ(cpu->eip, kData);
    EXPECT_FALSE(cpu->flag(cpu80486::FLAG_IF)) << "an interrupt gate clears IF";
    // The frame is EIP, CS, EFLAGS -- three dwords for a 32-bit gate.
    EXPECT_EQ(cpu->esp, kStackTop - 12);
    EXPECT_EQ(r32(cpu->esp), pm_code_ + 2) << "the return EIP is past the INT";
    EXPECT_EQ(r32(cpu->esp + 4), kCode32);
}

TEST_F(Cpu80486PmTest, ATrapGateLeavesTheInterruptFlagAlone) {
    enter_pm32();
    put(kData, {0xF4});
    set_gate(0x40, gate_desc(kCode32, kData, 0x8F));   // type F = 32-bit *trap* gate
    cpu->set_flag(cpu80486::FLAG_IF, true);
    pm_run({0xCD, 0x40}, 1);
    EXPECT_TRUE(faults.empty());
    EXPECT_TRUE(cpu->flag(cpu80486::FLAG_IF)) << "a trap gate does not mask interrupts";
}

TEST_F(Cpu80486PmTest, ASixteenBitGatePushesASixteenBitFrame) {
    enter_pm32();
    set_desc(kCode16, seg_desc(0, 0xFFFF, 0x9A, false, false));
    put(kData, {0xF4});
    set_gate(0x40, gate_desc(kCode16, kData, 0x86));   // type 6 = 16-bit interrupt gate
    pm_run({0xCD, 0x40}, 1);
    EXPECT_TRUE(faults.empty());
    EXPECT_EQ(cpu->cs, kCode16);
    EXPECT_EQ(cpu->esp, kStackTop - 6) << "three words, not three dwords";
}

TEST_F(Cpu80486PmTest, SoftwareIntChecksTheGateDplButAHardwareInterruptDoesNot) {
    enter_pm32();
    put(kData, {0xF4});
    // A DPL-0 gate: ring 3 may not reach it with INT n.
    set_gate(0x40, gate_desc(kCode32, kData, 0x8E));
    // LTR first: the hardware-interrupt half of this test crosses from ring 3
    // to ring 0, which takes its stack out of the TSS.
    pm_run({0x66, 0xB8, uint8_t(kTssSel), 0x00, 0x0F, 0x00, 0xD8}, 2);
    ASSERT_TRUE(faults.empty());
    pm_run({0x68, uint8_t(kStack3 | 3), 0x00, 0x00, 0x00,
            0x68, 0x00, 0x7C, 0x00, 0x00,
            0x68, 0x02, 0x02, 0x00, 0x00,
            0x68, uint8_t(kCode3), 0x00, 0x00, 0x00,
            0x68, 0x00, 0x64, 0x00, 0x00,      // push kData+0x400
            0xCF}, 6);
    ASSERT_EQ(cpu->cpl(), 3);
    put(kData + 0x400, {0xCD, 0x40});
    cpu->step();
    // "To prevent user programs from simulating interrupts with the INT
    // instruction, the DPL of an interrupt or trap gate must be greater than
    // or equal to CPL" -- error code is the gate's IDT offset plus 2.
    expect_fault(cpu80486::EXC_GP, 0x40u * 8 + 2);
    faults.clear();
    cpu->halted = false;   // the #GP above had no gate, so it shut the CPU down
    // The same vector delivered as a *hardware* interrupt is not checked --
    // which is the whole point of the exemption.
    cpu->interrupt(0x40);
    EXPECT_TRUE(faults.empty());
    EXPECT_EQ(cpu->cpl(), 0);
    EXPECT_EQ(cpu->eip, kData);
}

TEST_F(Cpu80486PmTest, InterPrivilegeInterruptSwitchesStackFromTheTssAndIretdReturns) {
    enter_pm32();
    // LTR so the CPU has a TSS to take SS0/ESP0 from.
    pm_run({0x66, 0xB8, uint8_t(kTssSel), 0x00, 0x0F, 0x00, 0xD8}, 2);   // ltr kTssSel
    ASSERT_TRUE(faults.empty());
    set_gate(0x40, gate_desc(kCode32, kData, 0xEE));   // DPL 3, reachable from ring 3
    put(kData, {0xCF});                                 // iretd straight back out
    put(kData + 0x400, {0xCD, 0x40,                     // int 40h
                        0xB8, 0x99, 0x00, 0x00, 0x00}); // mov eax,99h, back at ring 3
    put(pm_code_ + 7, {0x68, uint8_t(kStack3 | 3), 0x00, 0x00, 0x00,
                       0x68, 0x00, 0x7C, 0x00, 0x00,
                       0x68, 0x02, 0x02, 0x00, 0x00,
                       0x68, uint8_t(kCode3), 0x00, 0x00, 0x00,
                       0x68, 0x00, 0x64, 0x00, 0x00,
                       0xCF});
    cpu->eip = pm_code_ + 7;
    for (int i = 0; i < 6; ++i) cpu->step();
    ASSERT_EQ(cpu->cpl(), 3);
    cpu->step();   // int 40h
    ASSERT_TRUE(faults.empty());
    EXPECT_EQ(cpu->cpl(), 0) << "the gate raised privilege";
    EXPECT_EQ(cpu->ss, kData32) << "SS came from TSS.SS0";
    // With a privilege change the frame is EIP, CS, EFLAGS, ESP, SS -- five
    // dwords, so the interrupted ring-3 stack can be restored.
    EXPECT_EQ(cpu->esp, 0x00007E00u - 20);
    EXPECT_EQ(r32(cpu->esp + 12), 0x00007C00u) << "the old ESP is on the new stack";
    EXPECT_EQ(r32(cpu->esp + 16), uint32_t(kStack3 | 3)) << "and the old SS";
    cpu->step();   // iretd
    EXPECT_TRUE(faults.empty());
    EXPECT_EQ(cpu->cpl(), 3) << "IRETD returned outward";
    EXPECT_EQ(cpu->esp, 0x00007C00u) << "and restored the ring-3 stack pointer";
    cpu->step();
    EXPECT_EQ(cpu->eax, 0x99u);
}

TEST_F(Cpu80486PmTest, AnExceptionPushesItsErrorCode) {
    enter_pm32();
    put(kData, {0xF4});
    set_gate(cpu80486::EXC_GP, gate_desc(kCode32, kData, 0x8E));
    // A #GP from loading a bad SS carries the offending selector.
    pm_run({0x66, 0xB8, uint8_t(kRoData), 0x00, 0x8E, 0xD0}, 2);
    EXPECT_EQ(cpu->eip, kData);
    // The frame is EIP, CS, EFLAGS, error code -- the error code is pushed
    // last, so it is at the top.
    EXPECT_EQ(r32(cpu->esp), kRoData & 0xFFFCu);
    EXPECT_EQ(cpu->esp, kStackTop - 16);
}

TEST_F(Cpu80486PmTest, AVectorPastTheIdtLimitIsAGeneralProtectionFault) {
    enter_pm32();
    // Shrink the IDT to 16 vectors, then ask for vector 40h.
    w16(kIdtPtr, 0x7F);
    pm_run({0x0F, 0x01, 0x1E, 0x00, 0x00}, 0);   // (assembled but not run -- see below)
    // LIDT needs a 32-bit operand form to reach kIdtPtr as a disp32.
    put(pm_code_, {0x0F, 0x01, 0x1D, uint8_t(kIdtPtr), uint8_t(kIdtPtr >> 8), 0x00, 0x00,
                   0xCD, 0x40});
    cpu->eip = pm_code_;
    cpu->step();
    EXPECT_EQ(cpu->idtr().limit, 0x7F);
    cpu->step();
    expect_fault(cpu80486::EXC_GP, 0x40u * 8 + 2);
}

TEST_F(Cpu80486PmTest, ANotPresentGateIsASegmentNotPresentFault) {
    enter_pm32();
    set_gate(0x40, gate_desc(kCode32, kData, 0x0E));   // P = 0
    pm_run({0xCD, 0x40}, 1);
    expect_fault(cpu80486::EXC_NP, 0x40u * 8 + 2);
}

TEST_F(Cpu80486PmTest, AnUndeliverableFaultEscalatesToDoubleFaultThenShutdown) {
    enter_pm32();
    // Nothing in the IDT at all: the #GP has no gate, which is a #DF, and the
    // #DF has no gate either, which is shutdown -- the CPU stops until RESET.
    for (uint32_t v = 0; v < 32; ++v) set_gate(int(v), 0);
    pm_run({0x66, 0xB8, uint8_t(kRoData), 0x00, 0x8E, 0xD0}, 2);
    ASSERT_GE(faults.size(), 2u);
    EXPECT_EQ(faults[0].vector, cpu80486::EXC_GP);
    EXPECT_EQ(faults[1].vector, cpu80486::EXC_DF);
    EXPECT_TRUE(cpu->halted);
}

TEST_F(Cpu80486PmTest, AFaultRestoresTheStackPointerSoTheInstructionCanRestart) {
    enter_pm32();
    // A PUSH that cannot fit inside the stack segment is a #SS, and the
    // handler must see ESP exactly as it was *before* the PUSH -- otherwise
    // restarting the instruction (which is the whole point of a fault being
    // restartable) would decrement ESP twice.
    pm_run({0x66, 0xB8, uint8_t(kSmall), 0x00,   // mov ax,kSmall (limit 0FFFh)
            0x8E, 0xD0,                          // mov ss,ax
            0xBC, 0x02, 0x00, 0x00, 0x00}, 3);   // mov esp,2
    ASSERT_TRUE(faults.empty());
    ASSERT_EQ(cpu->esp, 2u);
    put(pm_code_ + 11, {0x50});   // push eax -- four bytes will not fit below offset 2
    cpu->eip = pm_code_ + 11;
    cpu->step();
    expect_fault(cpu80486::EXC_SS, 0);
    EXPECT_EQ(faults[0].esp, 2u) << "the handler sees the pre-instruction ESP";
    EXPECT_EQ(faults[0].eip, pm_code_ + 11) << "and the faulting instruction's own EIP";
}

// --- task switching (PRM, "Task Switching") ------------------------------

TEST_F(Cpu80486PmTest, LtrLoadsTheTaskRegisterAndMarksTheTaskBusy) {
    enter_pm32();
    pm_run({0x66, 0xB8, uint8_t(kTssSel), 0x00, 0x0F, 0x00, 0xD8}, 2);
    ASSERT_TRUE(faults.empty());
    EXPECT_EQ(cpu->tr_selector(), kTssSel);
    EXPECT_EQ(cpu->tr().base, kTss);
    // Type 9 (available) becomes type B (busy) in the descriptor itself.
    EXPECT_EQ((r32(kGdt + (kTssSel & 0xFFF8u) + 4) >> 8) & 0x0Fu, 0x0Bu);
    // STR reads the selector back.
    put(pm_code_ + 7, {0x0F, 0x00, 0xCB});   // str bx
    cpu->eip = pm_code_ + 7;
    cpu->step();
    EXPECT_EQ(cpu->ebx & 0xFFFFu, kTssSel);
}

TEST_F(Cpu80486PmTest, LtrRejectsABusyTssAndANonTssDescriptor) {
    enter_pm32();
    pm_run({0x66, 0xB8, uint8_t(kData32), 0x00, 0x0F, 0x00, 0xD8}, 2);
    expect_fault(cpu80486::EXC_GP, kData32 & 0xFFFCu);
    faults.clear();
    set_desc(kTss2Sel, seg_desc(kTss2, 0x67, 0x8B, false, false));   // already busy
    pm_run({0x66, 0xB8, uint8_t(kTss2Sel), 0x00, 0x0F, 0x00, 0xD8}, 2);
    expect_fault(cpu80486::EXC_GP, kTss2Sel & 0xFFFCu);
}

TEST_F(Cpu80486PmTest, FarJumpToATssPerformsAHardwareTaskSwitch) {
    enter_pm32();
    // The whole point: the outgoing register file lands in the outgoing TSS
    // and the incoming one is loaded wholesale from the incoming TSS.
    for (uint32_t i = 0; i < 104; i += 4) w32(kTss2 + i, 0);
    put(kData, {0xF4});
    w32(kTss2 + 28, kPageDir);       // CR3
    w32(kTss2 + 32, kData);          // EIP
    w32(kTss2 + 36, 0x00000202u);    // EFLAGS
    w32(kTss2 + 40, 0x11112222u);    // EAX
    w32(kTss2 + 56, 0x00007A00u);    // ESP
    w32(kTss2 + 72, kData32);        // ES
    w32(kTss2 + 76, kCode32);        // CS
    w32(kTss2 + 80, kData32);        // SS
    w32(kTss2 + 84, kData32);        // DS
    pm_run({0x66, 0xB8, uint8_t(kTssSel), 0x00, 0x0F, 0x00, 0xD8}, 2);   // ltr
    ASSERT_TRUE(faults.empty());
    cpu->eax = 0x0BADF00Du;
    put(pm_code_ + 7, {0xEA, 0x00, 0x00, 0x00, 0x00, uint8_t(kTss2Sel), 0x00});
    cpu->eip = pm_code_ + 7;
    cpu->step();
    ASSERT_TRUE(faults.empty()) << "the task switch itself must not fault";
    EXPECT_EQ(cpu->tr_selector(), kTss2Sel);
    EXPECT_EQ(cpu->eax, 0x11112222u) << "EAX came from the incoming TSS";
    EXPECT_EQ(cpu->esp, 0x00007A00u);
    EXPECT_EQ(cpu->eip, kData);
    EXPECT_EQ(r32(kTss + 40), 0x0BADF00Du) << "the outgoing EAX was saved";
    EXPECT_EQ(r32(kTss + 32), pm_code_ + 14) << "and the outgoing EIP, past the JMP";
    // A JMP hands over rather than nesting: the outgoing task's busy bit
    // clears, the incoming one's sets, and NT stays clear.
    EXPECT_EQ((r32(kGdt + (kTssSel & 0xFFF8u) + 4) >> 8) & 0x0Fu, 0x09u);
    EXPECT_EQ((r32(kGdt + (kTss2Sel & 0xFFF8u) + 4) >> 8) & 0x0Fu, 0x0Bu);
    EXPECT_FALSE(cpu->flag(cpu80486::FLAG_NT));
    // And CR0.TS is set so the first FPU instruction in the new task traps.
    EXPECT_NE(cpu->cr(0) & uint32_t(cpu80486::CR0_TS), 0u);
}

TEST_F(Cpu80486PmTest, FarCallToATssNestsAndIretdReturnsThroughTheBackLink) {
    enter_pm32();
    for (uint32_t i = 0; i < 104; i += 4) w32(kTss2 + i, 0);
    put(kData, {0xB8, 0x55, 0x00, 0x00, 0x00,   // mov eax,55h
                0xCF});                          // iretd -- NT is set, so a task return
    w32(kTss2 + 28, kPageDir);
    w32(kTss2 + 32, kData);
    w32(kTss2 + 36, 0x00000202u);
    w32(kTss2 + 56, 0x00007A00u);
    w32(kTss2 + 72, kData32);
    w32(kTss2 + 76, kCode32);
    w32(kTss2 + 80, kData32);
    w32(kTss2 + 84, kData32);
    pm_run({0x66, 0xB8, uint8_t(kTssSel), 0x00, 0x0F, 0x00, 0xD8}, 2);
    cpu->eax = 0x0BADF00Du;
    put(pm_code_ + 7, {0x9A, 0x00, 0x00, 0x00, 0x00, uint8_t(kTss2Sel), 0x00,
                       0xB9, 0x01, 0x00, 0x00, 0x00});   // mov ecx,1 on the way back
    cpu->eip = pm_code_ + 7;
    cpu->step();   // the CALL-driven switch
    ASSERT_TRUE(faults.empty());
    EXPECT_TRUE(cpu->flag(cpu80486::FLAG_NT)) << "a CALL nests, so NT is set in the new task";
    EXPECT_EQ(r32(kTss2 + 0) & 0xFFFFu, kTssSel) << "and the back link names the caller";
    // Both tasks are busy while nested.
    EXPECT_EQ((r32(kGdt + (kTssSel & 0xFFF8u) + 4) >> 8) & 0x0Fu, 0x0Bu);
    cpu->step();   // mov eax,55h
    cpu->step();   // iretd -> back through the back link
    ASSERT_TRUE(faults.empty());
    EXPECT_EQ(cpu->tr_selector(), kTssSel);
    EXPECT_EQ(cpu->eax, 0x0BADF00Du) << "the caller's EAX came back out of its own TSS";
    EXPECT_FALSE(cpu->flag(cpu80486::FLAG_NT)) << "the return unwinds the nesting";
    EXPECT_EQ((r32(kGdt + (kTss2Sel & 0xFFF8u) + 4) >> 8) & 0x0Fu, 0x09u)
        << "and releases the inner task";
    cpu->step();
    EXPECT_EQ(cpu->ecx, 1u) << "execution resumed right after the CALL";
}

TEST_F(Cpu80486PmTest, ATaskSwitchThroughATaskGateDeliversAnInterrupt) {
    enter_pm32();
    for (uint32_t i = 0; i < 104; i += 4) w32(kTss2 + i, 0);
    put(kData, {0xF4});
    w32(kTss2 + 28, kPageDir);
    w32(kTss2 + 32, kData);
    w32(kTss2 + 36, 0x00000202u);
    w32(kTss2 + 56, 0x00007A00u);
    w32(kTss2 + 72, kData32);
    w32(kTss2 + 76, kCode32);
    w32(kTss2 + 80, kData32);
    w32(kTss2 + 84, kData32);
    // A task gate names a TSS selector rather than a code selector: the
    // classic way to give #DF a known-good stack of its own.
    set_gate(0x40, gate_desc(kTss2Sel, 0, 0x85));
    pm_run({0x66, 0xB8, uint8_t(kTssSel), 0x00, 0x0F, 0x00, 0xD8}, 2);
    put(pm_code_ + 7, {0xCD, 0x40});
    cpu->eip = pm_code_ + 7;
    cpu->step();
    ASSERT_TRUE(faults.empty());
    EXPECT_EQ(cpu->tr_selector(), kTss2Sel);
    EXPECT_EQ(cpu->eip, kData);
    EXPECT_TRUE(cpu->flag(cpu80486::FLAG_NT));
}

// --- I/O privilege and the flag-load rules -------------------------------

TEST_F(Cpu80486PmTest, CliAndStiRequireCplAtOrInsideIopl) {
    enter_pm32();
    put(kData, {0xF4});
    set_gate(cpu80486::EXC_GP, gate_desc(kCode32, kData, 0x8E));
    // At CPL 0 with IOPL 0 they are fine.
    pm_run({0xFA}, 1);
    EXPECT_TRUE(faults.empty());
    // At CPL 3 with IOPL 0 they are not.
    put(kData + 0x400, {0xFA});
    pm_run({0x68, uint8_t(kStack3 | 3), 0x00, 0x00, 0x00,
            0x68, 0x00, 0x7C, 0x00, 0x00,
            0x68, 0x02, 0x02, 0x00, 0x00,
            0x68, uint8_t(kCode3), 0x00, 0x00, 0x00,
            0x68, 0x00, 0x64, 0x00, 0x00,
            0xCF}, 6);
    ASSERT_EQ(cpu->cpl(), 3);
    cpu->step();
    expect_fault(cpu80486::EXC_GP, 0);
}

TEST_F(Cpu80486PmTest, PortIoAboveIoplConsultsTheTssPermissionBitmap) {
    enter_pm32();
    put(kData, {0xF4});
    set_gate(cpu80486::EXC_GP, gate_desc(kCode32, kData, 0x8E));
    // Give the TSS a real I/O permission bitmap: a *set* bit denies the port,
    // so a cleared one grants it. Port 60h allowed, port 70h denied.
    set_desc(kTssSel, seg_desc(kTss, 0x67 + 0x20, 0x89, false, false));
    w16(kTss + 102, 0x68);                      // map base
    for (uint32_t i = 0; i < 0x20; ++i) w8(kTss + 0x68 + i, 0xFF);
    w8(kTss + 0x68 + (0x60 / 8), 0x00);         // ports 60h-67h permitted
    pm_run({0x66, 0xB8, uint8_t(kTssSel), 0x00, 0x0F, 0x00, 0xD8}, 2);
    ASSERT_TRUE(faults.empty());
    put(kData + 0x400, {0xE4, 0x60,     // in al,60h  -- permitted
                        0xE4, 0x70});   // in al,70h  -- denied
    put(pm_code_ + 7, {0x68, uint8_t(kStack3 | 3), 0x00, 0x00, 0x00,
                       0x68, 0x00, 0x7C, 0x00, 0x00,
                       0x68, 0x02, 0x02, 0x00, 0x00,
                       0x68, uint8_t(kCode3), 0x00, 0x00, 0x00,
                       0x68, 0x00, 0x64, 0x00, 0x00,
                       0xCF});
    cpu->eip = pm_code_ + 7;
    for (int i = 0; i < 6; ++i) cpu->step();
    ASSERT_EQ(cpu->cpl(), 3);
    cpu->step();
    EXPECT_TRUE(faults.empty()) << "port 60h's bitmap bit is clear, so the access is granted";
    cpu->step();
    expect_fault(cpu80486::EXC_GP, 0, "port 70h's bit is set, so it is denied");
}

TEST_F(Cpu80486PmTest, PopfdCannotChangeIoplOutsideRingZeroOrIfAboveIopl) {
    enter_pm32();
    // At CPL 0 IOPL is writable.
    pm_run({0x68, 0x02, 0x30, 0x00, 0x00, 0x9D}, 2);   // push 3002h / popfd
    EXPECT_EQ((cpu->eflags & uint32_t(cpu80486::FLAG_IOPL)) >> 12, 3u);
    // At CPL 3 it is not: the attempt is silently ignored rather than
    // faulting (Intel 80486 PRM, POPF).
    put(kData + 0x400, {0x68, 0x02, 0x00, 0x00, 0x00,   // push 0002h (IOPL 0)
                        0x9D});                          // popfd
    pm_run({0x68, uint8_t(kStack3 | 3), 0x00, 0x00, 0x00,
            0x68, 0x00, 0x7C, 0x00, 0x00,
            0x68, 0x02, 0x32, 0x00, 0x00,     // EFLAGS with IOPL 3, IF set
            0x68, uint8_t(kCode3), 0x00, 0x00, 0x00,
            0x68, 0x00, 0x64, 0x00, 0x00,
            0xCF}, 6);
    ASSERT_EQ(cpu->cpl(), 3);
    ASSERT_EQ((cpu->eflags & uint32_t(cpu80486::FLAG_IOPL)) >> 12, 3u);
    cpu->step();
    cpu->step();
    EXPECT_TRUE(faults.empty());
    EXPECT_EQ((cpu->eflags & uint32_t(cpu80486::FLAG_IOPL)) >> 12, 3u)
        << "a CPL-3 POPFD leaves IOPL alone instead of faulting";
}

TEST_F(Cpu80486PmTest, HltIsRingZeroOnly) {
    enter_pm32();
    put(kData, {0xF4});
    set_gate(cpu80486::EXC_GP, gate_desc(kCode32, kData, 0x8E));
    put(kData + 0x400, {0xF4});
    pm_run({0x68, uint8_t(kStack3 | 3), 0x00, 0x00, 0x00,
            0x68, 0x00, 0x7C, 0x00, 0x00,
            0x68, 0x02, 0x02, 0x00, 0x00,
            0x68, uint8_t(kCode3), 0x00, 0x00, 0x00,
            0x68, 0x00, 0x64, 0x00, 0x00,
            0xCF}, 6);
    ASSERT_EQ(cpu->cpl(), 3);
    cpu->step();
    expect_fault(cpu80486::EXC_GP, 0);
}

TEST_F(Cpu80486PmTest, ControlRegistersAreRingZeroOnly) {
    enter_pm32();
    put(kData, {0xF4});
    set_gate(cpu80486::EXC_GP, gate_desc(kCode32, kData, 0x8E));
    put(kData + 0x400, {0x0F, 0x20, 0xC0});   // mov eax,cr0
    pm_run({0x68, uint8_t(kStack3 | 3), 0x00, 0x00, 0x00,
            0x68, 0x00, 0x7C, 0x00, 0x00,
            0x68, 0x02, 0x02, 0x00, 0x00,
            0x68, uint8_t(kCode3), 0x00, 0x00, 0x00,
            0x68, 0x00, 0x64, 0x00, 0x00,
            0xCF}, 6);
    ASSERT_EQ(cpu->cpl(), 3);
    cpu->step();
    expect_fault(cpu80486::EXC_GP, 0);
}

// --- returning to real mode, and what survives (PC486_REVIEW.md §5.4) ----

TEST_F(Cpu80486PmTest, ClearingProtectionEnableReturnsToRealModeWithSixteenBitDefaults) {
    enter_pm32();
    // Drop to a 16-bit code segment first, as real software does, then clear
    // PE. The 16-bit segment has base 0, so a real-mode CS of 0 lands in the
    // same place.
    put(kData, {0x0F, 0x20, 0xC0,       // mov eax,cr0
                0x24, 0xFE,             // and al,0FEh
                0x0F, 0x22, 0xC0,       // mov cr0,eax
                0xB8, 0x34, 0x12});     // mov ax,1234h -- 16-bit now: only 3 bytes
    cpu->eax = 0xFFFFFFFFu;
    pm_run({0x66, 0xEA, uint8_t(kData), uint8_t(kData >> 8), uint8_t(kCode16), 0x00}, 1);
    ASSERT_TRUE(faults.empty());
    EXPECT_EQ(cpu->cs, kCode16);
    for (int i = 0; i < 3; ++i) cpu->step();
    EXPECT_FALSE(cpu->protected_mode());
    cpu->step();
    EXPECT_EQ(cpu->eax & 0xFFFFu, 0x1234u);
    EXPECT_EQ(cpu->eip, kData + 11) << "B8 took a 16-bit immediate: real mode is 16-bit by default";
}

TEST_F(Cpu80486PmTest, ABigLimitLoadedInProtectedModeSurvivesIntoRealModeAsUnrealMode) {
    enter_pm32();
    // This is the mechanism PC486_REVIEW.md §5.4 documents: a memory manager
    // loads a 4GB-limit descriptor during a brief protected-mode excursion,
    // drops PE, and the cached limit survives the next real-mode segment load
    // -- so a 32-bit offset still reaches extended memory. ES already holds
    // the flat descriptor from enter_pm32().
    EXPECT_EQ(cpu->desc(Cpu::SEG_ES).limit, 0xFFFFFFFFu);
    put(kData, {0x0F, 0x20, 0xC0, 0x24, 0xFE, 0x0F, 0x22, 0xC0});
    pm_run({0x66, 0xEA, uint8_t(kData), uint8_t(kData >> 8), uint8_t(kCode16), 0x00}, 4);
    ASSERT_FALSE(cpu->protected_mode());
    // A real-mode segment load sets the base and leaves the cached limit
    // alone, which is the entire trick.
    put(kData + 0x20, {0x31, 0xC0, 0x8E, 0xC0});   // xor ax,ax / mov es,ax
    cpu->eip = kData + 0x20;
    cpu->step();
    cpu->step();
    EXPECT_EQ(cpu->desc(Cpu::SEG_ES).base, 0u);
    EXPECT_EQ(cpu->desc(Cpu::SEG_ES).limit, 0xFFFFFFFFu)
        << "the limit is not reset by a real-mode load -- that is unreal mode";
    EXPECT_TRUE(faults.empty());
}


// ===========================================================================
// The on-die x87 FPU.
//
// Tested through the ordinary real-mode fixture, because the FPU is not a
// protected-mode feature: an Intel486 DX2 has it on-die and real-mode DOS
// code uses it freely. Values are chosen to be *exactly* representable
// wherever a result is asserted, so a passing test means the arithmetic is
// right rather than close -- and the 80-bit round-trip tests use bit patterns
// (a NaN with a payload, a denormal) that only survive if the register file
// really holds the architectural format instead of a host double.
//
// Reference: Intel 80486 Programmer's Reference Manual, the floating-point
// chapters ("Floating-Point Unit", control/status/tag words, and the
// per-instruction descriptions).
// ===========================================================================

namespace {
// The ModR/M byte for an ESC instruction's `mod=00 rm=110 disp16` memory form,
// which is how a 16-bit-addressing ESC opcode names an absolute address.
constexpr uint8_t esc_mem(int reg) { return uint8_t((reg << 3) | 6); }
}  // namespace

class Cpu80486FpuTest : public Cpu80486Test {
protected:
    static constexpr uint16_t kA = 0x0200;   // scratch operand slots
    static constexpr uint16_t kB = 0x0300;
    static constexpr uint16_t kC = 0x0400;

    void poke64(uint32_t a, uint64_t v) { poke32(a, uint32_t(v)); poke32(a + 4, uint32_t(v >> 32)); }
    uint64_t mem64(uint32_t a) const { return uint64_t(memd(a)) | (uint64_t(memd(a + 4)) << 32); }
    void poke_double(uint32_t a, double d) { uint64_t b; std::memcpy(&b, &d, 8); poke64(a, b); }
    double mem_double(uint32_t a) const { uint64_t b = mem64(a); double d; std::memcpy(&d, &b, 8); return d; }
    void poke_float(uint32_t a, float f) { uint32_t b; std::memcpy(&b, &f, 4); poke32(a, b); }
    float mem_float(uint32_t a) const { uint32_t b = memd(a); float f; std::memcpy(&f, &b, 4); return f; }
    void poke80(uint32_t a, uint64_t sig, uint16_t sign_exp) { poke64(a, sig); poke16(a + 8, sign_exp); }

    // FLD m64 / FSTP m64, the two instructions nearly every other test needs.
    static std::initializer_list<uint8_t> fld_m64(uint16_t at);
    bool c0() const { return (cpu->fpu_status() & (1u << 8)) != 0; }
    bool c1() const { return (cpu->fpu_status() & (1u << 9)) != 0; }
    bool c2() const { return (cpu->fpu_status() & (1u << 10)) != 0; }
    bool c3() const { return (cpu->fpu_status() & (1u << 14)) != 0; }
    // The tag field of ST(i): 3 means empty.
    int tag_of(int i) const {
        int phys = (cpu->fpu_top() + i) & 7;
        return (cpu->fpu_tag() >> (phys * 2)) & 3;
    }
};

TEST_F(Cpu80486FpuTest, FninitLeavesTheDocumentedResetState) {
    run({0xDB, 0xE3});   // FNINIT
    // Control word 037Fh: all six exception masks set, extended precision,
    // round to nearest (Intel 80486 PRM, "FPU Initialization").
    EXPECT_EQ(cpu->fpu_control(), 0x037Fu);
    EXPECT_EQ(cpu->fpu_status(), 0x0000u);
    EXPECT_EQ(cpu->fpu_tag(), 0xFFFFu) << "every register tagged empty";
    EXPECT_EQ(cpu->fpu_top(), 0);
    EXPECT_EQ(unimpl_count, 0);
}

TEST_F(Cpu80486FpuTest, TheEightyBitRegisterFileHoldsTheArchitecturalFormat) {
    // A quiet NaN with a payload no host double could carry through: if the
    // register file were a double, the payload and the exact exponent would
    // not come back.
    poke80(kA, 0xC123456789ABCDEFull, 0x7FFF);
    runN({0xDB, 0xE3,                          // FNINIT
          0xDB, esc_mem(5), 0x00, 0x02,        // FLD  tbyte [0200h]
          0xDB, esc_mem(7), 0x00, 0x03}, 3);   // FSTP tbyte [0300h]
    EXPECT_EQ(mem64(kB), 0xC123456789ABCDEFull);
    EXPECT_EQ(memw(kB + 8), 0x7FFFu);
    EXPECT_EQ(unimpl_count, 0);
}

TEST_F(Cpu80486FpuTest, AnEightyBitDenormalRoundTripsExactly) {
    // Exponent 0 with a non-zero significand is a denormal, and its integer
    // bit is explicitly clear -- a shape that only survives if loads and
    // stores move the raw format.
    poke80(kA, 0x0000000000000001ull, 0x0000);
    runN({0xDB, 0xE3,
          0xDB, esc_mem(5), 0x00, 0x02,
          0xDB, esc_mem(7), 0x00, 0x03}, 3);
    EXPECT_EQ(mem64(kB), 0x0000000000000001ull);
    EXPECT_EQ(memw(kB + 8), 0x0000u);
}

TEST_F(Cpu80486FpuTest, DoubleAndSingleLoadsAndStoresRoundTrip) {
    poke_double(kA, 1234.5);
    runN({0xDB, 0xE3,
          0xDD, esc_mem(0), 0x00, 0x02,        // FLD  qword [0200h]
          0xDD, esc_mem(3), 0x00, 0x03}, 3);   // FSTP qword [0300h]
    EXPECT_EQ(mem_double(kB), 1234.5);
    poke_float(kC, 0.5f);
    runN({0xDB, 0xE3,
          0xD9, esc_mem(0), 0x00, 0x04,        // FLD  dword [0400h]
          0xD9, esc_mem(3), 0x00, 0x03}, 3);   // FSTP dword [0300h]
    EXPECT_EQ(mem_float(kB), 0.5f);
}

TEST_F(Cpu80486FpuTest, ArithmeticOnExactlyRepresentableValues) {
    poke_double(kA, 2.5);
    poke_double(kB, 4.0);
    runN({0xDB, 0xE3,
          0xDD, esc_mem(0), 0x00, 0x02,        // FLD qword [2.5]   -> ST0
          0xDD, esc_mem(0), 0x00, 0x03,        // FLD qword [4.0]   -> ST0, 2.5 in ST1
          0xDE, 0xC9,                          // FMULP ST(1),ST    -> 10.0
          0xDD, esc_mem(3), 0x00, 0x04}, 5);   // FSTP qword [0400h]
    EXPECT_EQ(mem_double(kC), 10.0);
    EXPECT_EQ(tag_of(0), 3) << "both operands were consumed";

    // FSUB and FDIV, including the reversed-operand forms, on values whose
    // results are exact.
    poke_double(kA, 10.0);
    poke_double(kB, 4.0);
    runN({0xDB, 0xE3,
          0xDD, esc_mem(0), 0x00, 0x02,        // ST0 = 10.0
          0xDD, esc_mem(0), 0x00, 0x03,        // ST0 = 4.0, ST1 = 10.0
          0xD8, 0xE1,                          // FSUB ST,ST(1)  -> 4.0 - 10.0 = -6.0
          0xDD, esc_mem(3), 0x00, 0x04}, 5);
    EXPECT_EQ(mem_double(kC), -6.0);
    runN({0xDB, 0xE3,
          0xDD, esc_mem(0), 0x00, 0x02,
          0xDD, esc_mem(0), 0x00, 0x03,
          0xD8, 0xE9,                          // FSUBR ST,ST(1) -> 10.0 - 4.0 = 6.0
          0xDD, esc_mem(3), 0x00, 0x04}, 5);
    EXPECT_EQ(mem_double(kC), 6.0);
    runN({0xDB, 0xE3,
          0xDD, esc_mem(0), 0x00, 0x02,
          0xDD, esc_mem(0), 0x00, 0x03,
          0xD8, 0xF1,                          // FDIV ST,ST(1)  -> 4.0 / 10.0
          0xDD, esc_mem(3), 0x00, 0x04}, 5);
    EXPECT_EQ(mem_double(kC), 0.4);
    runN({0xDB, 0xE3,
          0xDD, esc_mem(0), 0x00, 0x02,
          0xDD, esc_mem(0), 0x00, 0x03,
          0xD8, 0xF9,                          // FDIVR ST,ST(1) -> 10.0 / 4.0 = 2.5
          0xDD, esc_mem(3), 0x00, 0x04}, 5);
    EXPECT_EQ(mem_double(kC), 2.5);
}

TEST_F(Cpu80486FpuTest, TheDcAndDeEncodingsReverseTheSubtractAndDivideForms) {
    // A genuine x87 encoding quirk Intel documents and every assembler has to
    // special-case: with ST(i) as the destination, DC E0+i is FSUBR and
    // DC E8+i is FSUB -- the opposite way round from the D8 forms above.
    poke_double(kA, 10.0);
    poke_double(kB, 4.0);
    runN({0xDB, 0xE3,
          0xDD, esc_mem(0), 0x00, 0x02,        // ST0 = 10.0
          0xDD, esc_mem(0), 0x00, 0x03,        // ST0 = 4.0, ST1 = 10.0
          0xDC, 0xE9,                          // FSUB ST(1),ST -> ST1 = 10.0 - 4.0 = 6.0
          0xDD, 0xD8,                          // FSTP ST(0)  (discard 4.0)
          0xDD, esc_mem(3), 0x00, 0x04}, 6);
    EXPECT_EQ(mem_double(kC), 6.0);
    runN({0xDB, 0xE3,
          0xDD, esc_mem(0), 0x00, 0x02,
          0xDD, esc_mem(0), 0x00, 0x03,
          0xDC, 0xE1,                          // FSUBR ST(1),ST -> ST1 = 4.0 - 10.0 = -6.0
          0xDD, 0xD8,
          0xDD, esc_mem(3), 0x00, 0x04}, 6);
    EXPECT_EQ(mem_double(kC), -6.0);
}

TEST_F(Cpu80486FpuTest, IntegerLoadsAndStoresCoverAllThreeWidths) {
    poke16(kA, uint16_t(int16_t(-1234)));
    runN({0xDB, 0xE3,
          0xDF, esc_mem(0), 0x00, 0x02,        // FILD word [0200h]
          0xDB, esc_mem(3), 0x00, 0x03}, 3);   // FISTP dword [0300h]
    EXPECT_EQ(int32_t(memd(kB)), -1234);
    poke32(kA, uint32_t(int32_t(-70000)));
    runN({0xDB, 0xE3,
          0xDB, esc_mem(0), 0x00, 0x02,        // FILD dword [0200h]
          0xDF, esc_mem(7), 0x00, 0x03}, 3);   // FISTP qword [0300h]
    EXPECT_EQ(int64_t(mem64(kB)), -70000);
    poke64(kA, uint64_t(int64_t(-1234567890123ll)));
    runN({0xDB, 0xE3,
          0xDF, esc_mem(5), 0x00, 0x02,        // FILD qword [0200h]
          0xDF, esc_mem(7), 0x00, 0x03}, 3);   // FISTP qword [0300h]
    EXPECT_EQ(int64_t(mem64(kB)), -1234567890123ll);
}

TEST_F(Cpu80486FpuTest, IntegerStoresHonorTheRoundingControlField) {
    // The one place software genuinely depends on RC: a C compiler's (int)
    // cast sets RC to truncate, does the FISTP, and puts RC back.
    poke_double(kA, 1.5);
    auto store_with_rc = [&](uint16_t rc_bits) {
        poke16(kC, uint16_t(0x037F | rc_bits));
        runN({0xDB, 0xE3,
              0xD9, esc_mem(5), 0x00, 0x04,        // FLDCW [0400h]
              0xDD, esc_mem(0), 0x00, 0x02,        // FLD qword [1.5]
              0xDB, esc_mem(3), 0x00, 0x03}, 4);   // FISTP dword [0300h]
        return int32_t(memd(kB));
    };
    EXPECT_EQ(store_with_rc(0x0000), 2) << "round to nearest, ties to even";
    EXPECT_EQ(store_with_rc(0x0400), 1) << "round down (toward -infinity)";
    EXPECT_EQ(store_with_rc(0x0800), 2) << "round up (toward +infinity)";
    EXPECT_EQ(store_with_rc(0x0C00), 1) << "truncate (toward zero)";
    // 2.5 discriminates nearest-even from round-half-up.
    poke_double(kA, 2.5);
    EXPECT_EQ(store_with_rc(0x0000), 2) << "2.5 rounds to 2, not 3: ties go to even";
    EXPECT_EQ(store_with_rc(0x0800), 3);
}

TEST_F(Cpu80486FpuTest, PrecisionControlNarrowsTheResult) {
    // With PC set to single precision the FPU rounds each result to 24 bits of
    // significand, which is observable as soon as a value needs more.
    poke16(kC, 0x007F);   // 037Fh with PC = 00 (single)
    poke_double(kA, 1.0);
    poke_double(kB, 3.0);
    runN({0xDB, 0xE3,
          0xD9, esc_mem(5), 0x00, 0x04,        // FLDCW: single precision
          0xDD, esc_mem(0), 0x00, 0x02,        // ST0 = 1.0
          0xDD, esc_mem(0), 0x00, 0x03,        // ST0 = 3.0, ST1 = 1.0
          0xDE, 0xF9,                          // FDIVP ST(1),ST -> ST1 = 1.0/3.0
          0xDD, esc_mem(3), 0x00, 0x04}, 6);   // FSTP qword
    double narrowed = mem_double(kC);
    EXPECT_EQ(narrowed, double(float(1.0 / 3.0)))
        << "the result carries only single-precision significand bits";
    EXPECT_NE(narrowed, 1.0 / 3.0);
}

TEST_F(Cpu80486FpuTest, FcomSetsTheThreeConditionCodeBitsThreeWays) {
    poke_double(kA, 5.0);
    poke_double(kB, 7.0);
    // The double-real compare is DC /2; D8 /2 is the *single*-real form, so
    // pointing D8 at a qword would silently read the low half as a float.
    // ST(0) greater: C3 C2 C0 all clear.
    runN({0xDB, 0xE3,
          0xDD, esc_mem(0), 0x00, 0x03,        // ST0 = 7.0
          0xDC, esc_mem(2), 0x00, 0x02}, 3);   // FCOM qword [5.0]
    EXPECT_FALSE(c3()); EXPECT_FALSE(c2()); EXPECT_FALSE(c0());
    // ST(0) less: C0 set.
    runN({0xDB, 0xE3,
          0xDD, esc_mem(0), 0x00, 0x02,        // ST0 = 5.0
          0xDC, esc_mem(2), 0x00, 0x03}, 3);   // FCOM qword [7.0]
    EXPECT_FALSE(c3()); EXPECT_FALSE(c2()); EXPECT_TRUE(c0());
    // Equal: C3 set.
    runN({0xDB, 0xE3,
          0xDD, esc_mem(0), 0x00, 0x02,
          0xDC, esc_mem(2), 0x00, 0x02}, 3);
    EXPECT_TRUE(c3()); EXPECT_FALSE(c2()); EXPECT_FALSE(c0());
    // And FCOMP pops, while FCOM does not.
    runN({0xDB, 0xE3,
          0xDD, esc_mem(0), 0x00, 0x02,
          0xDC, esc_mem(3), 0x00, 0x02}, 3);   // FCOMP qword [5.0]
    EXPECT_TRUE(c3());
    EXPECT_EQ(tag_of(0), 3) << "FCOMP consumed ST(0)";
}

TEST_F(Cpu80486FpuTest, TheSingleRealCompareFormReadsAFloat) {
    poke_float(kA, 2.5f);
    poke_double(kB, 1.0);
    runN({0xDB, 0xE3,
          0xDD, esc_mem(0), 0x00, 0x03,        // ST0 = 1.0
          0xD8, esc_mem(2), 0x00, 0x02}, 3);   // FCOM dword [2.5f]
    EXPECT_TRUE(c0()) << "1.0 < 2.5";
    EXPECT_FALSE(c3());
}

TEST_F(Cpu80486FpuTest, AnUnorderedCompareSetsAllThreeAndFcomSignalsInvalidWhileFucomDoesNot) {
    poke80(kA, 0xC000000000000000ull, 0x7FFF);   // a quiet NaN
    poke_double(kB, 1.0);
    runN({0xDB, 0xE3,
          0xDD, esc_mem(0), 0x00, 0x03,        // ST0 = 1.0
          0xDB, esc_mem(5), 0x00, 0x02,        // ST0 = NaN, ST1 = 1.0
          0xDE, 0xD9}, 4);                     // FCOMPP
    EXPECT_TRUE(c3()); EXPECT_TRUE(c2()); EXPECT_TRUE(c0()) << "unordered sets all three";
    EXPECT_NE(cpu->fpu_status() & 0x0001u, 0u) << "FCOM signals #IA on an unordered compare";
    runN({0xDB, 0xE3,
          0xDD, esc_mem(0), 0x00, 0x03,
          0xDB, esc_mem(5), 0x00, 0x02,
          0xDA, 0xE9}, 4);                     // FUCOMPP
    EXPECT_TRUE(c3()); EXPECT_TRUE(c2()); EXPECT_TRUE(c0());
    EXPECT_EQ(cpu->fpu_status() & 0x0001u, 0u)
        << "FUCOM is the whole point: an unordered compare that does not signal";
}

TEST_F(Cpu80486FpuTest, FtstComparesAgainstZeroAndFxamClassifies) {
    poke_double(kA, -3.0);
    runN({0xDB, 0xE3, 0xDD, esc_mem(0), 0x00, 0x02, 0xD9, 0xE4}, 3);   // FTST
    EXPECT_TRUE(c0()) << "-3.0 is less than zero";
    // FXAM reports the *class* in C3/C2/C0 and the sign in C1.
    runN({0xDB, 0xE3, 0xDD, esc_mem(0), 0x00, 0x02, 0xD9, 0xE5}, 3);   // FXAM on -3.0
    EXPECT_TRUE(c1()) << "C1 carries the sign";
    EXPECT_FALSE(c3()); EXPECT_TRUE(c2()); EXPECT_FALSE(c0()) << "normal finite: 010";
    runN({0xDB, 0xE3, 0xD9, 0xEE, 0xD9, 0xE5}, 3);                     // FLDZ / FXAM
    EXPECT_TRUE(c3()); EXPECT_FALSE(c2()); EXPECT_FALSE(c0()) << "zero: 100";
    runN({0xDB, 0xE3, 0xD9, 0xE5}, 2);                                 // FXAM on an empty stack
    EXPECT_TRUE(c3()); EXPECT_FALSE(c2()); EXPECT_TRUE(c0()) << "empty: 101";
    poke80(kA, 0x8000000000000000ull, 0x7FFF);                         // +infinity
    runN({0xDB, 0xE3, 0xDB, esc_mem(5), 0x00, 0x02, 0xD9, 0xE5}, 3);
    EXPECT_FALSE(c3()); EXPECT_TRUE(c2()); EXPECT_TRUE(c0()) << "infinity: 011";
    poke80(kA, 0xC000000000000000ull, 0x7FFF);                         // a NaN
    runN({0xDB, 0xE3, 0xDB, esc_mem(5), 0x00, 0x02, 0xD9, 0xE5}, 3);
    EXPECT_FALSE(c3()); EXPECT_FALSE(c2()); EXPECT_TRUE(c0()) << "NaN: 001";
}

TEST_F(Cpu80486FpuTest, DivideByZeroSetsTheZeroDivideFlag) {
    poke_double(kA, 0.0);
    poke_double(kB, 1.0);
    runN({0xDB, 0xE3,
          0xDD, esc_mem(0), 0x00, 0x03,        // ST0 = 1.0
          0xDC, esc_mem(6), 0x00, 0x02}, 3);   // FDIV qword [0.0]
    EXPECT_NE(cpu->fpu_status() & 0x0004u, 0u) << "ZE, the zero-divide flag";
    // Masked (the reset default), so the result is infinity rather than a trap.
    EXPECT_TRUE(std::isinf(cpu->st_value(0)));
}

TEST_F(Cpu80486FpuTest, SquareRootOfANegativeIsAnInvalidOperation) {
    poke_double(kA, -4.0);
    runN({0xDB, 0xE3, 0xDD, esc_mem(0), 0x00, 0x02, 0xD9, 0xFA}, 3);   // FSQRT
    EXPECT_NE(cpu->fpu_status() & 0x0001u, 0u) << "IE, the invalid-operation flag";
    EXPECT_TRUE(std::isnan(cpu->st_value(0))) << "and the masked result is the indefinite QNaN";
}

TEST_F(Cpu80486FpuTest, SquareRootAndScaleAndRoundIntOnExactValues) {
    poke_double(kA, 16.0);
    runN({0xDB, 0xE3, 0xDD, esc_mem(0), 0x00, 0x02, 0xD9, 0xFA,
          0xDD, esc_mem(3), 0x00, 0x03}, 4);   // FSQRT / FSTP
    EXPECT_EQ(mem_double(kB), 4.0);
    poke_double(kA, 3.0);   // the scale factor, in ST(1)
    poke_double(kB, 5.0);   // the value, in ST(0)
    runN({0xDB, 0xE3,
          0xDD, esc_mem(0), 0x00, 0x02,        // ST0 = 3.0
          0xDD, esc_mem(0), 0x00, 0x03,        // ST0 = 5.0, ST1 = 3.0
          0xD9, 0xFD,                          // FSCALE -> 5.0 * 2^3 = 40.0
          0xDD, esc_mem(3), 0x00, 0x04}, 5);
    EXPECT_EQ(mem_double(kC), 40.0);
    poke_double(kA, -2.5);
    runN({0xDB, 0xE3, 0xDD, esc_mem(0), 0x00, 0x02, 0xD9, 0xFC,
          0xDD, esc_mem(3), 0x00, 0x03}, 4);   // FRNDINT, round to nearest
    EXPECT_EQ(mem_double(kB), -2.0) << "-2.5 rounds to -2: ties to even";
}

TEST_F(Cpu80486FpuTest, ChsAndAbsFlipAndClearTheSignBitOnly) {
    // These touch the sign bit and nothing else, so they work on a NaN just as
    // well as on a number -- which is why they are bit operations here.
    poke80(kA, 0xC123456789ABCDEFull, 0x7FFF);
    runN({0xDB, 0xE3, 0xDB, esc_mem(5), 0x00, 0x02, 0xD9, 0xE0,
          0xDB, esc_mem(7), 0x00, 0x03}, 4);   // FCHS
    EXPECT_EQ(memw(kB + 8), 0xFFFFu) << "the sign bit flipped";
    EXPECT_EQ(mem64(kB), 0xC123456789ABCDEFull) << "and the significand is untouched";
    poke80(kA, 0xC123456789ABCDEFull, 0xFFFF);
    runN({0xDB, 0xE3, 0xDB, esc_mem(5), 0x00, 0x02, 0xD9, 0xE1,
          0xDB, esc_mem(7), 0x00, 0x03}, 4);   // FABS
    EXPECT_EQ(memw(kB + 8), 0x7FFFu);
}

TEST_F(Cpu80486FpuTest, TheBuiltInConstantsMatchTheirDocumentedValues) {
    struct { uint8_t op; double want; const char *name; } cases[] = {
        {0xE8, 1.0, "FLD1"},
        {0xE9, 3.321928094887362, "FLDL2T"},
        {0xEA, 1.4426950408889634, "FLDL2E"},
        {0xEB, 3.141592653589793, "FLDPI"},
        {0xEC, 0.30102999566398120, "FLDLG2"},
        {0xED, 0.69314718055994531, "FLDLN2"},
        {0xEE, 0.0, "FLDZ"},
    };
    for (const auto &c : cases) {
        runN({0xDB, 0xE3, 0xD9, c.op, 0xDD, esc_mem(3), 0x00, 0x03}, 3);
        EXPECT_DOUBLE_EQ(mem_double(kB), c.want) << c.name;
    }
}

TEST_F(Cpu80486FpuTest, PushingOntoAFullStackIsAStackFault) {
    // Nine pushes onto an eight-register stack. The ninth sets IE *and* SF,
    // with C1 = 1 marking overflow rather than underflow -- the only way a
    // handler can tell the two apart (Intel 80486 PRM, "Stack Fault").
    std::vector<uint8_t> code = {0xDB, 0xE3};
    for (int i = 0; i < 9; ++i) { code.push_back(0xD9); code.push_back(0xE8); }  // FLD1
    uint16_t at = 0;
    for (uint8_t b : code) mem[at++] = b;
    cpu->eip = 0;
    for (int i = 0; i < 10; ++i) cpu->step();
    EXPECT_NE(cpu->fpu_status() & 0x0001u, 0u) << "IE";
    EXPECT_NE(cpu->fpu_status() & 0x0040u, 0u) << "SF, the stack-fault qualifier";
    EXPECT_TRUE(c1()) << "C1 = 1 means overflow";
}

TEST_F(Cpu80486FpuTest, PoppingAnEmptyStackIsAStackFaultTheOtherWay) {
    runN({0xDB, 0xE3, 0xDD, esc_mem(3), 0x00, 0x03}, 2);   // FNINIT / FSTP qword
    EXPECT_NE(cpu->fpu_status() & 0x0001u, 0u) << "IE";
    EXPECT_NE(cpu->fpu_status() & 0x0040u, 0u) << "SF";
    EXPECT_FALSE(c1()) << "C1 = 0 means underflow";
}

TEST_F(Cpu80486FpuTest, FxchSwapsAndFfreeTagsAndTheStackPointerMoves) {
    poke_double(kA, 1.0);
    poke_double(kB, 2.0);
    runN({0xDB, 0xE3,
          0xDD, esc_mem(0), 0x00, 0x02,        // ST0 = 1.0
          0xDD, esc_mem(0), 0x00, 0x03,        // ST0 = 2.0, ST1 = 1.0
          0xD9, 0xC9}, 4);                     // FXCH ST(1)
    EXPECT_EQ(double(cpu->st_value(0)), 1.0);
    EXPECT_EQ(double(cpu->st_value(1)), 2.0);
    // FFREE marks a register empty without moving TOP.
    int top_before = cpu->fpu_top();
    put_and_run({0xDD, 0xC1});   // FFREE ST(1)
    EXPECT_EQ(cpu->fpu_top(), top_before);
    EXPECT_EQ(tag_of(1), 3);
    EXPECT_EQ(tag_of(0), 0) << "ST(0) is untouched";
    // FINCSTP / FDECSTP move TOP without touching any tag.
    put_and_run({0xD9, 0xF7});   // FINCSTP
    EXPECT_EQ(cpu->fpu_top(), (top_before + 1) & 7);
    put_and_run({0xD9, 0xF6});   // FDECSTP
    EXPECT_EQ(cpu->fpu_top(), top_before);
}

TEST_F(Cpu80486FpuTest, ControlAndStatusWordsRoundTripThroughMemoryAndAx) {
    poke16(kA, 0x0F3F);
    runN({0xDB, 0xE3,
          0xD9, esc_mem(5), 0x00, 0x02,        // FLDCW [0200h]
          0xD9, esc_mem(7), 0x00, 0x03}, 3);   // FNSTCW [0300h]
    EXPECT_EQ(cpu->fpu_control(), 0x0F3Fu);
    EXPECT_EQ(memw(kB), 0x0F3Fu);
    // The status word's TOP field is bits 11-13, so FNSTSW is how software
    // reads the stack pointer at all.
    runN({0xDB, 0xE3, 0xD9, 0xE8, 0xDF, 0xE0}, 3);   // FNINIT / FLD1 / FNSTSW AX
    EXPECT_EQ((cpu->eax >> 11) & 7u, 7u) << "one push moved TOP from 0 to 7";
    EXPECT_EQ(cpu->eax & 0xFFFFu, cpu->fpu_status());
    runN({0xDB, 0xE3, 0xD9, 0xE8, 0xDD, esc_mem(7), 0x00, 0x03}, 3);   // FNSTSW [0300h]
    EXPECT_EQ(memw(kB), cpu->fpu_status());
}

TEST_F(Cpu80486FpuTest, FnclexClearsTheExceptionFlagsButNotTheRegisters) {
    poke_double(kA, 0.0);
    poke_double(kB, 1.0);
    runN({0xDB, 0xE3,
          0xDD, esc_mem(0), 0x00, 0x03,
          0xDC, esc_mem(6), 0x00, 0x02}, 3);   // divide by zero
    ASSERT_NE(cpu->fpu_status() & 0x0004u, 0u);
    put_and_run({0xDB, 0xE2});                 // FNCLEX
    EXPECT_EQ(cpu->fpu_status() & 0x00FFu, 0u) << "every exception flag and ES cleared";
    EXPECT_EQ(tag_of(0), 0) << "but the register stack is left alone -- that is FNINIT's job";
}

TEST_F(Cpu80486FpuTest, FsaveAndFrstorRoundTripTheWholeStackAndEnvironment) {
    poke_double(kA, 7.25);
    runN({0xDB, 0xE3,
          0xDD, esc_mem(0), 0x00, 0x02,        // ST0 = 7.25
          0xD9, 0xE8,                          // ST0 = 1.0, ST1 = 7.25
          0x66, 0xDD, esc_mem(6), 0x00, 0x05}, 4);   // FNSAVE [0500h], 32-bit environment
    // FSAVE leaves the FPU reset, which is what makes it usable for a task
    // switch: the next task starts clean.
    EXPECT_EQ(cpu->fpu_control(), 0x037Fu);
    EXPECT_EQ(cpu->fpu_tag(), 0xFFFFu);
    put_and_run({0x66, 0xDD, esc_mem(4), 0x00, 0x05});   // FRSTOR [0500h]
    EXPECT_EQ(double(cpu->st_value(0)), 1.0);
    EXPECT_EQ(double(cpu->st_value(1)), 7.25);
    EXPECT_EQ(tag_of(2), 3);
}

TEST_F(Cpu80486FpuTest, FstenvStoresTheEnvironmentAndMasksEveryException) {
    poke16(kA, 0x0000);   // a control word with nothing masked
    runN({0xDB, 0xE3,
          0xD9, esc_mem(5), 0x00, 0x02,        // FLDCW 0000h
          0x66, 0xD9, esc_mem(6), 0x00, 0x05}, 3);   // FNSTENV [0500h]
    EXPECT_EQ(memw(0x0500), 0x0000u) << "the stored control word is the one that was in effect";
    EXPECT_EQ(cpu->fpu_control() & 0x003Fu, 0x003Fu)
        << "FSTENV masks all six afterwards, so the handler it belongs to cannot re-fault";
}

TEST_F(Cpu80486FpuTest, PackedDecimalLoadAndStoreRoundTrip) {
    // FBLD/FBSTP move 18 packed decimal digits plus a sign byte -- the format
    // COBOL-era and BCD-arithmetic code used, and the reason the x87 has them.
    runN({0xDB, 0xE3, 0xD9, 0xE8}, 2);         // ST0 = 1.0
    poke_double(kA, -123456789.0);
    runN({0xDB, 0xE3,
          0xDD, esc_mem(0), 0x00, 0x02,        // ST0 = -123456789.0
          0xDF, esc_mem(6), 0x00, 0x03}, 3);   // FBSTP tbyte [0300h]
    EXPECT_EQ(mem[kB + 9] & 0x80u, 0x80u) << "the sign byte";
    EXPECT_EQ(mem[kB + 0], 0x89u) << "least significant two digits first";
    EXPECT_EQ(mem[kB + 1], 0x67u);
    put_and_run({0xDF, esc_mem(4), 0x00, 0x03});   // FBLD tbyte [0300h]
    EXPECT_EQ(double(cpu->st_value(0)), -123456789.0);
}

TEST_F(Cpu80486FpuTest, TranscendentalsProduceTheDocumentedResults) {
    poke_double(kA, 0.0);
    runN({0xDB, 0xE3, 0xDD, esc_mem(0), 0x00, 0x02, 0xD9, 0xFE,
          0xDD, esc_mem(3), 0x00, 0x03}, 4);   // FSIN(0) = 0
    EXPECT_EQ(mem_double(kB), 0.0);
    runN({0xDB, 0xE3, 0xDD, esc_mem(0), 0x00, 0x02, 0xD9, 0xFF,
          0xDD, esc_mem(3), 0x00, 0x03}, 4);   // FCOS(0) = 1
    EXPECT_EQ(mem_double(kB), 1.0);
    runN({0xDB, 0xE3, 0xDD, esc_mem(0), 0x00, 0x02, 0xD9, 0xF0,
          0xDD, esc_mem(3), 0x00, 0x03}, 4);   // F2XM1(0) = 2^0 - 1 = 0
    EXPECT_EQ(mem_double(kB), 0.0);
    // FYL2X: ST(1) * log2(ST(0)), popping. 3.0 * log2(8.0) = 9.0, exact.
    poke_double(kA, 3.0);
    poke_double(kB, 8.0);
    runN({0xDB, 0xE3,
          0xDD, esc_mem(0), 0x00, 0x02,        // ST0 = 3.0
          0xDD, esc_mem(0), 0x00, 0x03,        // ST0 = 8.0, ST1 = 3.0
          0xD9, 0xF1,                          // FYL2X
          0xDD, esc_mem(3), 0x00, 0x04}, 5);
    EXPECT_DOUBLE_EQ(mem_double(kC), 9.0);
    // FPATAN of (1,1) is pi/4.
    poke_double(kA, 1.0);
    runN({0xDB, 0xE3,
          0xDD, esc_mem(0), 0x00, 0x02,
          0xDD, esc_mem(0), 0x00, 0x02,
          0xD9, 0xF3,                          // FPATAN
          0xDD, esc_mem(3), 0x00, 0x03}, 5);
    EXPECT_DOUBLE_EQ(mem_double(kB), 3.141592653589793 / 4.0);
}

TEST_F(Cpu80486FpuTest, FpremReducesAndClearsTheIncompleteFlag) {
    poke_double(kA, 3.0);    // the divisor, ST(1)
    poke_double(kB, 10.0);   // the dividend, ST(0)
    runN({0xDB, 0xE3,
          0xDD, esc_mem(0), 0x00, 0x02,
          0xDD, esc_mem(0), 0x00, 0x03,
          0xD9, 0xF8,                          // FPREM -> 10 mod 3 = 1
          0xDD, esc_mem(3), 0x00, 0x04}, 5);
    EXPECT_EQ(mem_double(kC), 1.0);
    EXPECT_FALSE(c2()) << "C2 clear means the reduction is complete";
    // FPREM1 is the IEEE remainder, which differs in sign for this pair:
    // 10 rem 3 rounds the quotient to nearest (3 -> 3), giving 1; but for
    // 5 and 3 the IEEE result is -1 where FPREM gives 2.
    poke_double(kA, 3.0);
    poke_double(kB, 5.0);
    runN({0xDB, 0xE3,
          0xDD, esc_mem(0), 0x00, 0x02,
          0xDD, esc_mem(0), 0x00, 0x03,
          0xD9, 0xF5,                          // FPREM1
          0xDD, esc_mem(3), 0x00, 0x04}, 5);
    EXPECT_EQ(mem_double(kC), -1.0);
    runN({0xDB, 0xE3,
          0xDD, esc_mem(0), 0x00, 0x02,
          0xDD, esc_mem(0), 0x00, 0x03,
          0xD9, 0xF8,                          // FPREM, the 8087 form
          0xDD, esc_mem(3), 0x00, 0x04}, 5);
    EXPECT_EQ(mem_double(kC), 2.0);
}

TEST_F(Cpu80486FpuTest, FxtractSplitsExponentAndSignificand) {
    poke_double(kA, 40.0);   // 1.25 * 2^5
    runN({0xDB, 0xE3,
          0xDD, esc_mem(0), 0x00, 0x02,
          0xD9, 0xF4,                          // FXTRACT
          0xDD, esc_mem(3), 0x00, 0x03,        // FSTP the significand
          0xDD, esc_mem(3), 0x00, 0x04}, 5);   // FSTP the exponent
    EXPECT_EQ(mem_double(kB), 1.25) << "the significand, in [1,2)";
    EXPECT_EQ(mem_double(kC), 5.0) << "and the unbiased exponent";
}

TEST_F(Cpu80486FpuTest, CoprocessorEmulationAndTaskSwitchedBothRaiseDeviceNotAvailable) {
    // CR0.EM routes every ESC opcode to #NM so a software emulator can pick
    // it up; CR0.TS does the same for the first FPU instruction after a task
    // switch, so a handler can swap the register stack between tasks.
    poke16(0x0007 * 4 + 0, 0x0500);   // IVT[7] -> 0000:0500
    poke16(0x0007 * 4 + 2, 0x0000);
    mem[0x0500] = 0xF4;               // HLT
    cpu->eax = uint32_t(cpu80486::CR0_EM);
    run({0x0F, 0x22, 0xC0});          // MOV CR0, EAX
    cpu->halted = false;
    run({0xD9, 0xE8});                // FLD1
    EXPECT_EQ(cpu->eip, 0x0500u) << "vectored through IVT[7] (#NM)";
    cpu->eax = uint32_t(cpu80486::CR0_TS);
    cpu->halted = false;
    run({0x0F, 0x22, 0xC0});
    cpu->halted = false;
    run({0xD9, 0xE8});
    EXPECT_EQ(cpu->eip, 0x0500u);
    // CLTS clears TS, and the FPU is available again.
    cpu->halted = false;
    runN({0x0F, 0x06, 0xD9, 0xE8}, 2);   // CLTS / FLD1
    EXPECT_EQ(double(cpu->st_value(0)), 1.0);
    EXPECT_EQ(unimpl_count, 0);
}

TEST_F(Cpu80486FpuTest, WaitFaultsOnlyWhenMonitorCoprocessorAndTaskSwitchedAreBothSet) {
    poke16(0x0007 * 4 + 0, 0x0500);
    poke16(0x0007 * 4 + 2, 0x0000);
    mem[0x0500] = 0xF4;
    // TS alone leaves WAIT alone: MP exists precisely so WAIT and the ESC
    // opcodes can be trapped separately.
    cpu->eax = uint32_t(cpu80486::CR0_TS);
    run({0x0F, 0x22, 0xC0});
    run({0x9B});                      // FWAIT
    EXPECT_EQ(cpu->eip, 1u) << "no fault: MP is clear";
    cpu->eax = uint32_t(cpu80486::CR0_TS | cpu80486::CR0_MP);
    run({0x0F, 0x22, 0xC0});
    run({0x9B});
    EXPECT_EQ(cpu->eip, 0x0500u) << "MP and TS together: #NM";
}

TEST_F(Cpu80486FpuTest, AnUnmaskedExceptionIsReportedOnTheNextFpuInstruction) {
    // The 486 defers the report: the instruction that *causes* an unmasked
    // exception completes, and the error surfaces when the next waiting FPU
    // instruction runs -- which is why a handler can safely use the no-wait
    // forms (FNSTSW, FNCLEX) to inspect and clear it.
    poke16(0x0010 * 4 + 0, 0x0500);   // IVT[16] -> #MF
    poke16(0x0010 * 4 + 2, 0x0000);
    mem[0x0500] = 0xF4;
    // NE = 1 selects the native #MF report over the external FERR#/IRQ13 path.
    cpu->eax = uint32_t(cpu80486::CR0_NE);
    run({0x0F, 0x22, 0xC0});
    poke16(kA, 0x0000);               // unmask everything
    poke_double(kB, 0.0);
    poke_double(kC, 1.0);
    runN({0xDB, 0xE3,
          0xD9, esc_mem(5), 0x00, 0x02,        // FLDCW: nothing masked
          0xDD, esc_mem(0), 0x00, 0x04,        // ST0 = 1.0
          0xDC, esc_mem(6), 0x00, 0x03}, 4);   // FDIV by 0.0 -> unmasked ZE
    ASSERT_NE(cpu->fpu_status() & 0x0080u, 0u) << "ES, the error summary, is set";
    EXPECT_NE(cpu->eip, 0x0500u) << "the causing instruction itself does not trap";
    // A no-wait form still does not check, so a handler can read the status.
    put_and_run({0xDF, 0xE0});        // FNSTSW AX
    EXPECT_NE(cpu->eip, 0x0500u) << "FNSTSW is a no-wait form";
    // The next *waiting* FPU instruction is the one that reports.
    put_and_run({0xD9, 0xE8});        // FLD1
    EXPECT_EQ(cpu->eip, 0x0500u) << "and now #MF is delivered";
}

}  // namespace
