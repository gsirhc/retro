// 80486 real-mode instruction semantics (cycle costs are in cpu80486_timing_test.cpp)
// Weighted toward what is new on a 486 vs the 286 core: 32-bit registers, FS/GS, 0x67 and SIB addressing,
// BSWAP/XADD/CMPXCHG, the 386 additions, the AC flag, and the protected-mode and x87 no-ops.
// References: Intel 80486 Programmer's Reference Manual, 8086/8088 User's Manual, AP-485 for AC and CPUID.

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
    // every 16-bit port cycle, in order
    std::vector<std::pair<uint16_t, uint16_t>> in16_log, out16_log;

    // on_unimplemented capture: fires for an unrecognized opcode, never for the documented no-ops
    int      unimpl_count = 0;
    uint16_t unimpl_opcode = 0;
    bool     log_reads = false;
    std::vector<uint32_t> reads;
    // first fault vector since catch_faults(), or -1
    int      first_fault = -1;

public:
    // the six Bus::For operations
    uint8_t mem_read(uint32_t a) {
        if (log_reads) reads.push_back(a);
        return mem[a & 0xFFFFF];
    }
    void mem_write(uint32_t a, uint8_t v) { mem[a & 0xFFFFF] = v; }
    uint8_t io_in(uint16_t) { return next_in_val; }
    void io_out(uint16_t p, uint8_t v) { last_out_port = p; last_out_port_val = v; }
    // atomic 16-bit port access, so IN AX,DX / OUT DX,AX are not split into two 8-bit accesses (IDE data register 0x1F0)
    uint16_t io_in16(uint16_t p) {
        uint16_t v = uint16_t(next_in16_val + in16_log.size());
        in16_log.push_back({p, v});
        return v;
    }
    void io_out16(uint16_t p, uint16_t v) { last_out16_port = p; last_out16_val = v; out16_log.push_back({p, v}); }

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

    void catch_faults() {
        first_fault = -1;
        cpu->on_fault = [this](int v, uint32_t, uint16_t, uint32_t) { if (first_fault < 0) first_fault = v; };
    }
    void load(std::initializer_list<uint8_t> code, uint16_t at = 0) {
        uint16_t addr = at;
        for (uint8_t b : code) mem[addr++] = b;
    }
    // assemble at CS:0, execute one instruction
    void run(std::initializer_list<uint8_t> code) {
        load(code);
        cpu->eip = 0;
        cpu->step();
    }
    // assemble at CS:0, execute n instructions
    void runN(std::initializer_list<uint8_t> code, int n) {
        load(code);
        cpu->eip = 0;
        for (int i = 0; i < n; ++i) cpu->step();
    }
    // runs one instruction without the leading FNINIT, so FPU state carries over
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
    // Intel 80486 PRM: PUSH SP/ESP pushes the pre-decrement value (286+, unlike 8086)
    cpu->ss = 0;
    cpu->esp = 0x2000;
    run({0x54});  // PUSH SP
    EXPECT_EQ(cpu->esp, 0x1FFEu);
    EXPECT_EQ(memw(0x1FFE), 0x2000) << "486 pushes SP's pre-decrement value";
}

TEST_F(Cpu80486Test, PopEspRelativeMemoryResolvesAgainstThePostIncrementEsp) {
    // Intel SDM, POP: with ESP as the base of a memory destination, ESP is incremented before the address is computed
    cpu->ss = 0;
    cpu->esp = 0x2000;
    mem[0x2000] = 0xAA; mem[0x2001] = 0xAA; mem[0x2002] = 0xAA; mem[0x2003] = 0xAA;
    mem[0x2004] = 0xBB; mem[0x2005] = 0xBB; mem[0x2006] = 0xBB; mem[0x2007] = 0xBB;
    mem[0x2008] = 0xCC; mem[0x2009] = 0xCC; mem[0x200A] = 0xCC; mem[0x200B] = 0xCC;
    run({0x66, 0x67, 0x8F, 0x44, 0x24, 0x04});  // POP DWORD [ESP+4]
    EXPECT_EQ(cpu->esp, 0x2004u) << "one dword popped, ordinary increment";
    EXPECT_EQ(memd(0x2008), 0xAAAAAAAAu)
        << "the popped value lands at the post-increment ESP+4, not the pre-increment one";
    EXPECT_EQ(memd(0x2004), 0xBBBBBBBBu) << "the pre-increment ESP+4 slot is untouched";
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
    // SIB 98: scale 4, index EBX, base EAX
    cpu->eax = 0x0100;
    cpu->ebx = 0x0002;
    poke16(0x0108, 0x1234);
    run({0x67, 0x8B, 0x04, 0x98});
    EXPECT_EQ(cpu->eax & 0xFFFF, 0x1234u);
}

TEST_F(Cpu80486Test, Addr32Disp32WithNoBaseRegister) {
    // modrm 05 in 32-bit addressing is disp32 with no base, not [EBP] (Intel 80486 PRM ModR/M table)
    poke16(0x0200, 0xCAFE);
    run({0x67, 0x8B, 0x05, 0x00, 0x02, 0x00, 0x00});  // MOV AX, [00000200h]
    EXPECT_EQ(cpu->eax & 0xFFFF, 0xCAFEu);
}

TEST_F(Cpu80486Test, Addr32ScaledIndexWithDisp32AndNoBase) {
    // SIB CD: scale 8, index ECX, base 101 with mod=00 is disp32, no base
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
    // modrm 00 = [EAX], the inline fast path (PC486_REVIEW.md §16)
    cpu->ds = 0x2000;
    cpu->ss = 0x3000;  // decoy -- only ESP/EBP bases default to SS
    cpu->eax = 0x40;
    poke16((0x2000u << 4) + 0x40, 0xABCD);
    poke16((0x3000u << 4) + 0x40, 0x9999);
    run({0x67, 0x8B, 0x00});  // MOV AX, [EAX]
    EXPECT_EQ(cpu->eax & 0xFFFF, 0xABCDu);
}

TEST_F(Cpu80486Test, Addr32SegmentOverrideBeatsTheEbpStackDefault) {
    // an override beats the base register's default segment
    cpu->es = 0x5000;
    cpu->ss = 0x3000;  // the default this override must displace
    cpu->ebp = 0x20;
    poke16((0x5000u << 4) + 0x30, 0x1111);
    poke16((0x3000u << 4) + 0x30, 0x2222);
    run({0x26, 0x67, 0x8B, 0x45, 0x10});  // ES: MOV AX, [EBP + 10h]
    EXPECT_EQ(cpu->eax & 0xFFFF, 0x1111u);
}

TEST_F(Cpu80486Test, Addr32EspBaseDefaultsToStackSegmentAndIndexFourMeansNoIndex) {
    // SIB index 100 means no index
    cpu->ss = 0x4000;
    cpu->esp = 0x50;
    poke16((0x4000u << 4) + 0x50, 0x5678);
    run({0x67, 0x8B, 0x04, 0x24});  // MOV AX, [ESP]
    EXPECT_EQ(cpu->eax & 0xFFFF, 0x5678u);
}

TEST_F(Cpu80486Test, Addr32SibEbpBaseWithDisplacementDefaultsToStackSegment) {
    // an EBP base defaults to SS, including through the inline SIB path
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

TEST_F(Cpu80486Test, Addr32OffsetPastTheRealModeLimitRaisesGp) {
    // real mode checks the cached limit, 64KB unless a protected-mode excursion left a bigger one (Intel 80486 PRM, Real-Address Mode Exceptions)
    catch_faults();
    poke16(0x10200, 0x1234);
    run({0x67, 0x8B, 0x05, 0x00, 0x02, 0x01, 0x00});  // MOV AX, [00010200h]
    EXPECT_EQ(first_fault, 13);
    EXPECT_NE(cpu->eax & 0xFFFF, 0x1234u);
}

TEST_F(Cpu80486Test, AWordAtOffsetFfffFaultsInsteadOfWrapping) {
    catch_faults();
    run({0xA1, 0xFF, 0xFF});  // MOV AX, [0FFFFh]
    EXPECT_EQ(first_fault, 13) << "an 8086 wrapped to offset 0; a 486 raises #GP";
}

TEST_F(Cpu80486Test, PushWithSpAtOneRaisesStackFault) {
    catch_faults();
    cpu->ss = 0x1000;
    cpu->esp = 1;
    run({0x50});  // PUSH AX: the word lands at FFFFh
    EXPECT_EQ(first_fault, 12);
}



TEST_F(Cpu80486Test, StringOpWithoutAddr32KeepsUsingTheSixteenBitPointers) {
    // without 0x67 a string op advances only SI/DI/CX
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
    // LEA is pure arithmetic: truncating to the 64KB window gives a wrong number (the 386+ three-input adder idiom)
    cpu->eax = 0x00100000u;
    cpu->ebx = 0x00000100u;
    run({0x66, 0x67, 0x8D, 0x04, 0x98});  // LEA EAX, [EAX + EBX*4]
    EXPECT_EQ(cpu->eax, 0x00100400u);
}

TEST_F(Cpu80486Test, Addr32SelectsEcxAsTheLoopCounter) {
    // the address-size prefix picks CX vs ECX for LOOP/JCXZ
    cpu->ecx = 0x00010000u;  // CX == 0, but ECX != 0
    run({0x67, 0xE3, 0xFE});  // JECXZ $-2
    EXPECT_EQ(cpu->eip, 3u) << "JECXZ must test the full ECX, not CX";
    run({0xE3, 0xFE});        // JCXZ $-2 -- CX is zero, so this one is taken
    EXPECT_EQ(cpu->eip, 0u);
}

TEST_F(Cpu80486Test, SixteenBitAddressingStillWrapsModSixtyFourK) {
    // 16-bit addressing sums wrap mod 64K; decode_modrm() never lets a 16-bit form exceed 0FFFFh
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
    // Intel 80486 PRM: zero source sets ZF and leaves the destination undefined; this core leaves it alone
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

// 16-bit SHLD/SHRD with count > 15: the 486's single 32-bit shifter shifts the 32-bit dest:src pair
// and keeps the named half (Intel calls it undefined). Borland's 32-bit shift helpers in CWSDPMI depend on it
// (cl=24 converts a physical page to a far pointer). See PC486_REVIEW.md §9.
TEST_F(Cpu80486Test, SixteenBitShldAboveFifteenShiftsTheThirtyTwoBitConcatenation) {
    // CWSDPMI's case: page 0x2B -> far pointer 2B00:0000
    cpu->edx = 0x00000000u;
    cpu->eax = 0x0000002Bu;
    cpu->ecx = 24;
    run({0x0F, 0xA5, 0xC2});  // SHLD DX, AX, CL
    EXPECT_EQ(cpu->edx & 0xFFFFu, 0x2B00u)
        << "0x0000002B << 24 = 0x2B000000, whose high half is 0x2B00";

    // the whole helper: DX:AX <<= 24
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
    // SHRD mirror: DX:AX >>= 24
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

// counts <= 15 follow the same rule inside Intel's specified range
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

// CF is the last bit shifted out of the 32-bit pair
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
    // Intel 80486 PRM: with a memory operand the register bit offset is not masked to the operand width
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

TEST_F(Cpu80486Test, AllSixteenConditionsDecodeTheirFlags) {
    using namespace cpu80486;
    // all flag combinations against the Intel 80486 PRM Jcc table
    for (int bits = 0; bits < 32; ++bits) {
        bool of = bits & 1, cf = bits & 2, zf = bits & 4, sf = bits & 8, pf = bits & 16;
        const bool want[16] = {
            of, !of, cf, !cf, zf, !zf, cf || zf, !cf && !zf,
            sf, !sf, pf, !pf, sf != of, sf == of, zf || (sf != of), !zf && (sf == of),
        };
        for (int cc = 0; cc < 16; ++cc) {
            cpu->set_flag(FLAG_OF, of); cpu->set_flag(FLAG_CF, cf); cpu->set_flag(FLAG_ZF, zf);
            cpu->set_flag(FLAG_SF, sf); cpu->set_flag(FLAG_PF, pf);
            cpu->ebx = 0xFF;
            run({0x0F, uint8_t(0x90 + cc), 0xC3});      // SETcc BL
            EXPECT_EQ(cpu->ebx & 0xFF, want[cc] ? 1u : 0u) << "SETcc " << cc << " flags " << bits;
            run({uint8_t(0x70 + cc), 0x10});            // Jcc short +10h
            EXPECT_EQ(cpu->eip, want[cc] ? 0x12u : 0x02u) << "Jcc " << cc << " flags " << bits;
        }
    }
}

TEST_F(Cpu80486Test, SixteenBitShiftsAndRotates) {
    struct Case { uint8_t modrm; uint16_t in; bool cf_in; uint16_t out; bool cf; bool of; };
    // D1 /r on BX (count 1), so OF is defined
    const Case cases[] = {
        {0xC3, 0x8001, false, 0x0003, true,  true },   // ROL
        {0xCB, 0x8001, false, 0xC000, true,  false},   // ROR
        {0xD3, 0x4000, true,  0x8001, false, true },   // RCL pulls CF in at the bottom
        {0xDB, 0x0001, true,  0x8000, true,  true },   // RCR pulls CF in at the top
        {0xE3, 0xC000, false, 0x8000, true,  false},   // SHL
        {0xEB, 0x8001, false, 0x4000, true,  true },   // SHR: OF is the old top bit
        {0xF3, 0x4000, false, 0x8000, false, true },   // /6, the undocumented SAL alias
        {0xFB, 0x8001, false, 0xC000, true,  false},   // SAR keeps the sign
    };
    for (const Case &t : cases) {
        cpu->ebx = 0xABCD0000u | t.in;
        cpu->set_flag(cpu80486::FLAG_CF, t.cf_in);
        run({0xD1, t.modrm});
        EXPECT_EQ(cpu->ebx, 0xABCD0000u | t.out) << "modrm " << int(t.modrm) << ": the upper half is untouched";
        EXPECT_EQ(CF(), t.cf) << "modrm " << int(t.modrm);
        EXPECT_EQ(OF(), t.of) << "modrm " << int(t.modrm);
    }

    cpu->ebx = 0x00F0;
    cpu->ecx = 4;
    run({0xD3, 0xE3});                  // SHL BX, CL
    EXPECT_EQ(cpu->ebx & 0xFFFF, 0x0F00u);
    run({0xC1, 0xEB, 8});               // SHR BX, 8
    EXPECT_EQ(cpu->ebx & 0xFFFF, 0x000Fu);
    EXPECT_FALSE(ZF());
    run({0xC1, 0xEB, 4});               // SHR BX, 4 -> 0, CF = last bit out
    EXPECT_TRUE(ZF());
    EXPECT_TRUE(CF());

    cpu->ebx = 0x1234;
    cpu->set_flag(cpu80486::FLAG_CF, true);
    cpu->set_flag(cpu80486::FLAG_ZF, true);
    run({0xC1, 0xE3, 32});              // SHL BX, 32: the count masks to 0
    EXPECT_EQ(cpu->ebx & 0xFFFF, 0x1234u);
    EXPECT_TRUE(CF()) << "a zero count leaves every flag alone";
    EXPECT_TRUE(ZF());

    cpu->ebx = 0x8000;
    cpu->set_flag(cpu80486::FLAG_CF, false);
    run({0xC1, 0xD3, 17});              // RCL BX, 17: a full 17-bit lap
    EXPECT_EQ(cpu->ebx & 0xFFFF, 0x8000u);
    EXPECT_FALSE(CF());
}

TEST_F(Cpu80486Test, EightBitRotatesAndShifts) {
    cpu->ebx = 0x81;
    run({0xD0, 0xCB});                  // ROR BL, 1
    EXPECT_EQ(cpu->ebx & 0xFF, 0xC0u);
    EXPECT_TRUE(CF());
    cpu->set_flag(cpu80486::FLAG_CF, false);
    run({0xD0, 0xD3});                  // RCL BL, 1
    EXPECT_EQ(cpu->ebx & 0xFF, 0x80u);
    EXPECT_TRUE(CF());
    run({0xD0, 0xDB});                  // RCR BL, 1
    EXPECT_EQ(cpu->ebx & 0xFF, 0xC0u);
    EXPECT_FALSE(CF());
    run({0xD0, 0xFB});                  // SAR BL, 1
    EXPECT_EQ(cpu->ebx & 0xFF, 0xE0u);
    EXPECT_FALSE(OF());
}

TEST_F(Cpu80486Test, DasAdjustsAfterBcdSubtraction) {
    cpu->eax = 0x1E;                    // 23h - 05h in binary, AF set
    cpu->set_flag(cpu80486::FLAG_AF, true);
    cpu->set_flag(cpu80486::FLAG_CF, false);
    run({0x2F});                        // DAS
    EXPECT_EQ(cpu->eax & 0xFF, 0x18u);
    EXPECT_TRUE(AF());
    EXPECT_FALSE(CF());

    cpu->eax = 0xFF;                    // 00h - 01h: borrow out of both digits
    cpu->set_flag(cpu80486::FLAG_AF, true);
    cpu->set_flag(cpu80486::FLAG_CF, true);
    run({0x2F});
    EXPECT_EQ(cpu->eax & 0xFF, 0x99u);
    EXPECT_TRUE(CF());

    cpu->eax = 0x42;
    cpu->set_flag(cpu80486::FLAG_AF, false);
    cpu->set_flag(cpu80486::FLAG_CF, false);
    run({0x2F});
    EXPECT_EQ(cpu->eax & 0xFF, 0x42u) << "a valid BCD byte passes through";
    EXPECT_FALSE(AF());
}

TEST_F(Cpu80486Test, AaaAndAasAdjustAllOfAxOnA286OrLater) {
    cpu->eax = 0x000B;                  // 6 + 5 in binary
    cpu->set_flag(cpu80486::FLAG_AF, false);
    run({0x37});                        // AAA
    EXPECT_EQ(cpu->eax & 0xFFFF, 0x0101u);
    EXPECT_TRUE(CF());
    EXPECT_TRUE(AF());

    // AL + 6 carries into AH on a 286+: 00FFh becomes 0205h (8086 gives 0105h)
    cpu->eax = 0x00FF;
    run({0x37});
    EXPECT_EQ(cpu->eax & 0xFFFF, 0x0205u);

    cpu->eax = 0x0203;
    cpu->set_flag(cpu80486::FLAG_AF, false);
    run({0x37});
    EXPECT_EQ(cpu->eax & 0xFFFF, 0x0203u);
    EXPECT_FALSE(CF());

    cpu->eax = 0x02FD;                  // 05h - 08h, AF set
    cpu->set_flag(cpu80486::FLAG_AF, true);
    run({0x3F});                        // AAS
    EXPECT_EQ(cpu->eax & 0xFFFF, 0x0107u);
    EXPECT_TRUE(CF());

    // AL - 6 borrows from AH too
    cpu->eax = 0x0203;
    cpu->set_flag(cpu80486::FLAG_AF, true);
    run({0x3F});
    EXPECT_EQ(cpu->eax & 0xFFFF, 0x000Du);

    cpu->eax = 0x0305;
    cpu->set_flag(cpu80486::FLAG_AF, false);
    run({0x3F});
    EXPECT_EQ(cpu->eax & 0xFFFF, 0x0305u);
    EXPECT_FALSE(AF());
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
    // AC is above bit 15, so a 16-bit POPF cannot clear it
    cpu->set_flag(cpu80486::FLAG_AC, true);
    cpu->ss = 0;
    cpu->esp = 0x2000;
    poke16(0x2000, cpu80486::FLAG_R1);
    run({0x9D});  // POPF
    EXPECT_TRUE(cpu->flag(cpu80486::FLAG_AC));
}

TEST_F(Cpu80486Test, AcBitTogglesAndReadsBackPerAp485) {
    // AP-485 Intel386 check: flip AC and read it back; the bit survives on a 486
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
    // real mode has no CPL to gate IOPL/NT, so a 486 loads them unconditionally
    cpu->ss = 0;
    cpu->esp = 0x2000;
    poke16(0x2000, uint16_t(cpu80486::FLAG_IOPL | cpu80486::FLAG_NT | cpu80486::FLAG_R1));
    run({0x9D});  // POPF
    EXPECT_EQ(cpu->eflags & cpu80486::FLAG_IOPL, uint32_t(cpu80486::FLAG_IOPL));
    EXPECT_TRUE(cpu->flag(cpu80486::FLAG_NT));
}

// --- descriptor-table and mode-control instructions ---

TEST_F(Cpu80486Test, LgdtAndLidtLoadTheDescriptorTableRegisters) {
    // 6-byte pseudo-descriptor: 16-bit limit, 32-bit base
    poke16(0x0200, 0x0017);
    poke32(0x0202, 0x00081234u);
    run({0x0F, 0x01, 0x16, 0x00, 0x02});  // LGDT [0200h]
    EXPECT_EQ(cpu->eip, 5u) << "ModR/M and disp16 must be consumed";
    EXPECT_EQ(cpu->gdtr().limit, 0x0017);
    // 16-bit LGDT loads only 24 bits of base (Intel 80486 PRM, LGDT/LIDT)
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
    // CR0.ET is hardwired to 1 on an Intel486; PE is 0 until protected mode is entered
    EXPECT_EQ(cpu->ebx & 0xFFFF, uint32_t(cpu80486::CR0_ET));
    EXPECT_FALSE(cpu->protected_mode());
    EXPECT_EQ(unimpl_count, 0);
}

TEST_F(Cpu80486Test, LmswEntersProtectedModeAndCannotLeaveItAgain) {
    cpu->eax = 0x0001;        // PE
    run({0x0F, 0x01, 0xF0});  // LMSW AX
    EXPECT_TRUE(cpu->protected_mode()) << "LMSW is how 286-era code entered protected mode";
    // LMSW cannot clear PE (Intel 80486 PRM, LMSW); leaving needs MOV to CR0
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
    // MOV to CR0 can return to real mode, as a DOS extender does
    cpu->eax = uint32_t(cpu80486::CR0_ET);
    run({0x0F, 0x22, 0xC0});
    EXPECT_FALSE(cpu->protected_mode());
    EXPECT_EQ(unimpl_count, 0);
}

TEST_F(Cpu80486Test, EnablingPagingWithoutProtectionIsAGeneralProtectionFault) {
    // PG with PE clear is #GP(0) (Intel 80486 PRM, CR0); the IVT is zero, so only CR0.PG not taking is checked
    cpu->eax = uint32_t(cpu80486::CR0_PG);
    run({0x0F, 0x22, 0xC0});  // MOV CR0, EAX
    EXPECT_FALSE(cpu->paging_enabled());
    EXPECT_FALSE(cpu->protected_mode());
    EXPECT_EQ(unimpl_count, 0) << "a fault is not an unimplemented opcode";
}

TEST_F(Cpu80486Test, ProtectedModeOnlyOpcodesAreInvalidOpcodesInRealMode) {
    // LLDT/LTR/SLDT/STR/VERR/VERW/LAR/LSL/ARPL are #UD in real mode; vector 6 sets a flag
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
    // LGDT and MOV CR0 are legal in real mode; FreeDOS HimemX uses them (PC486_REVIEW.md §5.4)
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
    // AP-485: CPUID exists on the SL-Enhanced IntelDX2 (earlier parts fault on 0F A2); function 0 returns the vendor string and max function 1
    cpu->eax = 0;
    run({0x0F, 0xA2});
    EXPECT_EQ(unimpl_count, 0) << "CPUID must not route to the unimplemented-opcode hook";
    EXPECT_EQ(cpu->eax, 1u);
    EXPECT_EQ(cpu->ebx, 0x756E6547u);  // "Genu"
    EXPECT_EQ(cpu->edx, 0x49656E69u);  // "ineI"
    EXPECT_EQ(cpu->ecx, 0x6C65746Eu);  // "ntel"
}

TEST_F(Cpu80486Test, CpuidFunctionOneReportsAnIntelDx2SignatureWithOnlyTheFpuFeature) {
    // family 4, model 3 is the IntelDX2 (AP-485); only EDX bit 0 (FPU) is set; FreeDOS VINFO/FDAUTO.BAT branch on it (PC486_REVIEW.md §13)
    cpu->eax = 1;
    run({0x0F, 0xA2});
    EXPECT_EQ(unimpl_count, 0);
    EXPECT_EQ((cpu->eax >> 8) & 0xF, 4u) << "family";
    EXPECT_EQ((cpu->eax >> 4) & 0xF, 3u) << "model: IntelDX2";
    EXPECT_EQ(cpu->eax & 0xF, 5u) << "stepping: SL-Enhanced aB0/aC0, the DX2 with CPUID";
    EXPECT_EQ((cpu->eax >> 12) & 0x3, 0u) << "type: original OEM processor";
    EXPECT_EQ(cpu->edx, 0x00000001u);
    EXPECT_EQ(cpu->ebx, 0u);
    EXPECT_EQ(cpu->ecx, 0u);
}

TEST_F(Cpu80486Test, TheIdFlagRoundTripsSoSoftwareCanDetectCpuid) {
    // AP-485 detection: PUSHFD, flip bit 21, POPFD, PUSHFD; FreeDOS VINFO runs it before CPUID
    cpu->ss = 0;
    cpu->esp = 0x2000;
    poke32(0x2000, 0x00200002u);
    run({0x66, 0x9D});  // POPFD
    EXPECT_TRUE(cpu->flag(cpu80486::FLAG_ID));
    cpu->esp = 0x2000;
    run({0x66, 0x9C});  // PUSHFD
    EXPECT_EQ(memd(0x1FFC) & 0x00200000u, 0x00200000u);
}

// ---------------------------------------------------------------------------
// Single-step (TF), the STI / MOV SS interrupt shadow, and shutdown
// ---------------------------------------------------------------------------

class Cpu80486StepTest : public Cpu80486Test {
protected:
    void SetUp() override {
        Cpu80486Test::SetUp();
        poke16(0x0001 * 4 + 0, 0x0400);   // IVT[1] (#DB)  -> 0000:0400
        poke16(0x0001 * 4 + 2, 0x0000);
        poke16(0x0021 * 4 + 0, 0x0500);   // IVT[21h]      -> 0000:0500
        poke16(0x0021 * 4 + 2, 0x0000);
        mem[0x0400] = 0xF4;
        mem[0x0500] = 0xF4;
        cpu->ss = 0;
        cpu->esp = 0x2000;
    }
};

TEST_F(Cpu80486StepTest, TfTrapsAfterTheInstructionWithTheNextIpSaved) {
    cpu->set_flag(cpu80486::FLAG_TF, true);
    run({0x90});  // NOP
    EXPECT_EQ(cpu->eip, 0x0400u) << "vectored through IVT[1]";
    EXPECT_EQ(memw(0x1FFA), 0x0001) << "a trap saves the address of the next instruction";
    EXPECT_NE(memw(0x1FFE) & cpu80486::FLAG_TF, 0u) << "the saved FLAGS keep TF for IRET";
    EXPECT_FALSE(cpu->flag(cpu80486::FLAG_TF)) << "the handler itself runs untrapped";
    EXPECT_NE(cpu->dr(6) & 0x4000u, 0u) << "DR6.BS reports a single-step";
}

TEST_F(Cpu80486StepTest, RepStringTrapsAfterEachIterationWithIpOnThePrefix) {
    // Intel 80486 PRM, Single-Step Trap: REP traps after every iteration, saved IP points back at the first prefix
    cpu->ds = 0; cpu->es = 0;
    cpu->esi = 0x0300;
    cpu->edi = 0x0340;
    cpu->ecx = 2;
    mem[0x0300] = 0x11;
    mem[0x0301] = 0x22;
    cpu->set_flag(cpu80486::FLAG_TF, true);
    run({0x26, 0xF3, 0xA4});  // ES: REP MOVSB
    EXPECT_EQ(cpu->eip, 0x0400u);
    EXPECT_EQ(memw(0x1FFA), 0x0000) << "back on the first prefix byte";
    EXPECT_EQ(cpu->ecx & 0xFFFF, 1u);
    EXPECT_EQ(mem[0x0340], 0x11);
    EXPECT_EQ(mem[0x0341], 0x00) << "only one iteration ran";

    cpu->esp = 0x2000;
    cpu->eip = 0;
    cpu->set_flag(cpu80486::FLAG_TF, true);
    cpu->step();
    EXPECT_EQ(memw(0x1FFA), 0x0003) << "the last iteration moves on";
    EXPECT_EQ(cpu->ecx & 0xFFFF, 0u);
    EXPECT_EQ(mem[0x0341], 0x22);
}

TEST_F(Cpu80486StepTest, AnInterruptBetweenRepChunksRestartsItWithItsSetupCost) {
    // Intel 80486 PRM, REP: an interrupt between iterations resumes after IRET and pays setup again
    mem[0x0500] = 0xCF;          // IRET
    cpu->rep_yield_cycles = 9;
    cpu->ds = 0; cpu->es = 0;
    cpu->esi = 0x0300;
    cpu->edi = 0x0400;
    cpu->ecx = 10;
    load({0xF3, 0xA4});          // REP MOVSB
    cpu->eip = 0;
    cpu->step();
    ASSERT_EQ(cpu->ecx & 0xFFFF, 7u);
    cpu->set_flag(cpu80486::FLAG_IF, true);
    cpu->interrupt(0x21);
    EXPECT_EQ(cpu->eip, 0x0500u);
    EXPECT_EQ(memw(0x1FFA), 0x0000) << "the saved IP is the REP";
    cpu->step();                 // IRET
    EXPECT_EQ(cpu->eip, 0u);
    EXPECT_EQ(cpu->step(), 1 + 12 + 9) << "a restarted REP pays its prefix and setup again";
    EXPECT_EQ(cpu->ecx & 0xFFFF, 4u);
}

TEST_F(Cpu80486StepTest, PopfThatSetsTfRunsUntrappedAndTheNextInstructionTraps) {
    poke16(0x1000, 0x0102);   // FLAGS with TF set
    cpu->esp = 0x1000;
    runN({0x9D, 0x90}, 1);    // POPF
    EXPECT_EQ(cpu->eip, 1u);
    EXPECT_TRUE(cpu->flag(cpu80486::FLAG_TF));
    cpu->step();              // NOP
    EXPECT_EQ(cpu->eip, 0x0400u);
}

TEST_F(Cpu80486StepTest, PopfThatClearsTfStillTrapsAfterItself) {
    cpu->set_flag(cpu80486::FLAG_TF, true);
    poke16(0x1000, 0x0002);
    cpu->esp = 0x1000;
    run({0x9D});              // POPF
    EXPECT_EQ(cpu->eip, 0x0400u);
}

TEST_F(Cpu80486StepTest, IntNClearsTfAndEntersTheHandlerUntrapped) {
    cpu->set_flag(cpu80486::FLAG_TF, true);
    run({0xCD, 0x21});        // INT 21h
    EXPECT_EQ(cpu->eip, 0x0500u) << "the INT's own handler, not #DB";
    EXPECT_FALSE(cpu->flag(cpu80486::FLAG_TF));
}

TEST_F(Cpu80486StepTest, MovSsDefersTheTrapPastTheNextInstruction) {
    cpu->set_flag(cpu80486::FLAG_TF, true);
    runN({0x8E, 0xD0, 0x90}, 1);  // MOV SS,AX
    EXPECT_EQ(cpu->eip, 2u) << "no trap at the SS:SP boundary";
    EXPECT_TRUE(cpu->interrupt_shadow());
    cpu->esp = 0x2000;
    cpu->step();                  // NOP
    EXPECT_EQ(cpu->eip, 0x0400u);
    EXPECT_EQ(memw(0x1FFA), 0x0003);
}

TEST_F(Cpu80486StepTest, StiFromClearIfAndSsLoadsOpenAOneInstructionShadow) {
    cpu->set_flag(cpu80486::FLAG_IF, false);
    run({0xFB});              // STI
    EXPECT_TRUE(cpu->interrupt_shadow());
    run({0x90});
    EXPECT_FALSE(cpu->interrupt_shadow()) << "the shadow covers one instruction only";
    run({0xFB});              // STI with IF already set
    EXPECT_FALSE(cpu->interrupt_shadow());
    cpu->esp = 0x2000;
    run({0x17});              // POP SS
    EXPECT_TRUE(cpu->interrupt_shadow());
    run({0x8E, 0xD8});        // MOV DS,AX
    EXPECT_FALSE(cpu->interrupt_shadow()) << "only SS loads hold interrupts off";
}

TEST_F(Cpu80486StepTest, AFaultWhileDeliveringADoubleFaultIsShutdown) {
    poke16(0x0600, 0x0000);   // IDTR limit 0: every vector is out of range
    poke32(0x0602, 0x00000000u);
    run({0x0F, 0x01, 0x1E, 0x00, 0x06});  // LIDT [0600h]
    run({0xCC});              // INT3 -> #GP -> #DF -> shutdown
    EXPECT_TRUE(cpu->shutdown());
    EXPECT_TRUE(cpu->halted);
    cpu->reset();
    EXPECT_FALSE(cpu->shutdown());
}

// ---------------------------------------------------------------------------
// Debug registers: breakpoints, watchpoints, GD, ICEBP (Intel 80486 PRM,
// "Debugging"; Intel 80386 PRM, "Debug Exceptions")
// ---------------------------------------------------------------------------

class Cpu80486DebugTest : public Cpu80486StepTest {
protected:
    // MOV DRn, EAX from a scratch address, EIP unchanged
    void set_dr(int n, uint32_t v) {
        uint32_t eip = cpu->eip;
        uint32_t eax = cpu->eax;
        load({0x0F, 0x23, uint8_t(0xC0 | (n << 3))}, 0x0700);
        cpu->eax = v;
        cpu->eip = 0x0700;
        cpu->step();
        cpu->eip = eip;
        cpu->eax = eax;
    }
};

TEST_F(Cpu80486DebugTest, Dr6AndDr7ComeOutOfResetWithTheirReservedBitsAndDr4Dr5AliasThem) {
    EXPECT_EQ(cpu->dr(6), 0xFFFF0FF0u);
    EXPECT_EQ(cpu->dr(7), 0x00000400u);
    set_dr(6, 0);
    EXPECT_EQ(cpu->dr(6), 0xFFFF0FF0u) << "reserved bits read as ones";
    set_dr(5, 0x00000100u);   // DR5 is DR7; LE alone arms no breakpoint
    EXPECT_EQ(cpu->dr(7), 0x00000500u);
    run({0x0F, 0x21, 0xE0});  // MOV EAX, DR4
    EXPECT_EQ(cpu->eax, 0xFFFF0FF0u);
}

TEST_F(Cpu80486DebugTest, AnInstructionBreakpointFaultsBeforeTheInstructionAndRfLetsItRun) {
    set_dr(0, 0x0010);
    set_dr(7, 0x00000001u);   // L0, RW0=00 (execute), LEN0=00
    load({0x90}, 0x0010);
    cpu->eip = 0x0010;
    cpu->step();
    EXPECT_EQ(cpu->eip, 0x0400u) << "vectored through IVT[1]";
    EXPECT_EQ(memw(0x1FFA), 0x0010) << "a fault saves the breakpoint's own address";
    EXPECT_EQ(cpu->dr(6) & 0xFu, 0x1u) << "DR6.B0";
    cpu->esp = 0x2000;
    cpu->eip = 0x0010;
    cpu->set_flag(cpu80486::FLAG_RF, true);
    cpu->step();
    EXPECT_EQ(cpu->eip, 0x0011u) << "RF holds the breakpoint off for one instruction";
    EXPECT_FALSE(cpu->flag(cpu80486::FLAG_RF)) << "and clears once it completes";
}

TEST_F(Cpu80486DebugTest, AWriteWatchpointTrapsAfterTheWriteAndIgnoresReads) {
    set_dr(1, 0x0302);
    set_dr(7, 0x00500004u);   // L1, RW1=01 (writes), LEN1=01 (two bytes)
    run({0xA0, 0x02, 0x03});  // MOV AL, [0302h]
    EXPECT_EQ(cpu->eip, 0x0003u) << "a read does not match a write watchpoint";
    cpu->eax = 0x5A;
    run({0xA2, 0x03, 0x03});  // MOV [0303h], AL
    EXPECT_EQ(mem[0x0303], 0x5A) << "the write happens";
    EXPECT_EQ(cpu->eip, 0x0400u);
    EXPECT_EQ(memw(0x1FFA), 0x0003) << "a trap saves the next instruction";
    EXPECT_EQ(cpu->dr(6) & 0xFu, 0x2u) << "DR6.B1";
}

TEST_F(Cpu80486DebugTest, AReadWriteWatchpointMatchesAnAccessThatOverlapsIt) {
    set_dr(2, 0x0305);        // LEN 4 aligns this down to 0304h-0307h
    set_dr(7, 0x0F000010u);   // L2, RW2=11 (reads or writes), LEN2=11 (four bytes)
    run({0xA1, 0x03, 0x03});  // MOV AX, [0303h] -- touches 0303h-0304h
    EXPECT_EQ(cpu->eip, 0x0400u);
    EXPECT_EQ(cpu->dr(6) & 0xFu, 0x4u) << "DR6.B2";
}

TEST_F(Cpu80486DebugTest, AWatchpointStopsARepStringAfterTheIterationThatHitIt) {
    cpu->ds = 0; cpu->es = 0;
    cpu->esi = 0x0300;
    cpu->edi = 0x0340;
    cpu->ecx = 4;
    set_dr(1, 0x0341);
    set_dr(7, 0x00100004u);   // L1, RW1=01, LEN1=00
    run({0xF3, 0xA4});        // REP MOVSB
    EXPECT_EQ(cpu->eip, 0x0400u);
    EXPECT_EQ(cpu->ecx & 0xFFFF, 2u) << "two iterations ran";
    EXPECT_EQ(memw(0x1FFA), 0x0000) << "IP stays on the REP so it resumes";
    EXPECT_EQ(cpu->dr(6) & 0xFu, 0x2u);
}

TEST_F(Cpu80486DebugTest, GeneralDetectFaultsADebugRegisterAccessAndClearsItself) {
    set_dr(7, 0x00002000u);   // GD
    run({0x0F, 0x21, 0xC0});  // MOV EAX, DR0
    EXPECT_EQ(cpu->eip, 0x0400u);
    EXPECT_EQ(memw(0x1FFA), 0x0000) << "a fault on the MOV itself";
    EXPECT_NE(cpu->dr(6) & 0x2000u, 0u) << "DR6.BD";
    EXPECT_EQ(cpu->dr(7) & 0x2000u, 0u) << "GD clears so the handler can use the registers";
}

TEST_F(Cpu80486DebugTest, IcebpTrapsThroughVectorOneWithNoStatusBit) {
    run({0xF1});
    EXPECT_EQ(cpu->eip, 0x0400u);
    EXPECT_EQ(memw(0x1FFA), 0x0001) << "a trap, past the one-byte instruction";
    EXPECT_EQ(cpu->dr(6), 0xFFFF0FF0u);
}

TEST_F(Cpu80486Test, TheFirstFetchAfterResetIsAtTheTopOfFourGigabytes) {
    // Intel486 Data Book, RESET: CS:IP F000:FFF0 with CS base FFFF0000h until the first far jump
    cpu->reset();
    EXPECT_EQ(cpu->desc(Cpu::SEG_CS).base, 0xFFFF0000u);
    const uint8_t jmp[] = {0xEA, 0x00, 0x01, 0x00, 0xF0};   // JMP F000:0100
    for (uint32_t i = 0; i < 5; ++i) mem[0xFFFF0 + i] = jmp[i];
    mem[0xF0100] = 0x90;
    log_reads = true;
    cpu->step();
    ASSERT_FALSE(reads.empty());
    EXPECT_EQ(reads.front(), 0xFFFFFFF0u);
    EXPECT_EQ(cpu->desc(Cpu::SEG_CS).base, 0x000F0000u) << "the far jump loads an ordinary base";
    reads.clear();
    cpu->step();
    ASSERT_FALSE(reads.empty());
    EXPECT_EQ(reads.front(), 0x000F0100u);
    EXPECT_EQ(cpu->eip, 0x0101u);
}

TEST_F(Cpu80486Test, SalcSetsAlFromCarryAndLeavesTheFlagsAlone) {
    cpu->eax = 0x1234;
    cpu->set_flag(cpu80486::FLAG_CF, true);
    run({0xD6});  // SALC
    EXPECT_EQ(cpu->eax, 0x12FFu);
    EXPECT_TRUE(cpu->flag(cpu80486::FLAG_CF));
    cpu->set_flag(cpu80486::FLAG_CF, false);
    cpu->set_flag(cpu80486::FLAG_ZF, false);
    run({0xD6});
    EXPECT_EQ(cpu->eax, 0x1200u);
    EXPECT_FALSE(cpu->flag(cpu80486::FLAG_ZF)) << "SALC is not SBB AL,AL: no flag changes";
    EXPECT_EQ(unimpl_count, 0) << "the SDM names D6 as reserved but never faulting";
}

TEST_F(Cpu80486Test, ReservedEncodingsRaiseInvalidOpcodeAtTheFaultingInstruction) {
    poke16(0x0006 * 4 + 0, 0x0400);   // IVT[6] -> 0000:0400
    poke16(0x0006 * 4 + 2, 0x0000);
    mem[0x0400] = 0xF4;
    int expected = 0;
    for (std::initializer_list<uint8_t> code : {
             std::initializer_list<uint8_t>{0x0F, 0x0B},              // UD2
             std::initializer_list<uint8_t>{0x0F, 0x31},              // RDTSC, a Pentium instruction
             std::initializer_list<uint8_t>{0x0F, 0xC7, 0x0E, 0, 2},  // CMPXCHG8B, likewise
             std::initializer_list<uint8_t>{0x0F, 0xA6, 0xC3},        // the A-step 486's CMPXCHG slot
             std::initializer_list<uint8_t>{0x0F, 0xBA, 0xC0, 0x01},  // 0F BA /0
             std::initializer_list<uint8_t>{0xFF, 0xF8},              // FF /7
             std::initializer_list<uint8_t>{0xFF, 0x3E, 0, 2},        // FF /7, memory form
             std::initializer_list<uint8_t>{0xFE, 0xD0},              // FE /2
             std::initializer_list<uint8_t>{0xFF, 0xD8},              // CALL far with a register operand
             std::initializer_list<uint8_t>{0xFF, 0xE8},              // JMP far with a register operand
             std::initializer_list<uint8_t>{0x8D, 0xC3},              // LEA from a register
             std::initializer_list<uint8_t>{0xC4, 0xC3},              // LES from a register
             std::initializer_list<uint8_t>{0xC5, 0xC3},              // LDS from a register
             std::initializer_list<uint8_t>{0x0F, 0xB2, 0xC3},        // LSS from a register
             std::initializer_list<uint8_t>{0x62, 0xC3},              // BOUND against a register
         }) {
        cpu->halted = false;
        cpu->ss = 0;
        cpu->esp = 0x2000;
        uint32_t ebx = cpu->ebx;
        run(code);
        ++expected;
        EXPECT_EQ(cpu->eip, 0x0400u) << "opcode " << int(*code.begin()) << " should vector through IVT[6]";
        EXPECT_EQ(memw(0x1FFA), 0x0000) << "#UD is a fault: the saved IP is the instruction's own";
        EXPECT_EQ(cpu->ebx, ebx) << "nothing executes before the fault";
    }
    EXPECT_EQ(unimpl_count, expected) << "each one still reaches the diagnostic hook";
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

TEST_F(Cpu80486Test, ResetLoadsTheComponentIdAndCachingOffCr0) {
    // Intel486 Data Book, after RESET: EDX is the component ID, CR0 has CD and NW; 0435h is the SL-Enhanced DX2-66, 0433h (B1) has no CPUID
    cpu->reset();
    EXPECT_EQ(cpu->edx, 0x00000435u);
    cpu->cs = 0;
    run({0x0F, 0x20, 0xC0});  // MOV EAX, CR0
    EXPECT_EQ(cpu->eax, 0x60000010u);
}

TEST_F(Cpu80486Test, Cr0WithNwSetAndCdClearRaisesGp) {
    catch_faults();
    cpu->eax = 0x20000010u;
    run({0x0F, 0x22, 0xC0});  // MOV CR0, EAX
    EXPECT_EQ(first_fault, 13);
    first_fault = -1;
    cpu->eax = 0x00000010u;   // the BIOS turning the cache on
    run({0x0F, 0x22, 0xC0});
    EXPECT_EQ(first_fault, -1);
}

TEST_F(Cpu80486Test, LockIsLegalOnlyOnLockableMemoryDestinations) {
    // Intel 80486 PRM, LOCK: other uses raise #UD
    auto fault_of = [&](std::initializer_list<uint8_t> code) {
        catch_faults();
        cpu->ds = 0;
        cpu->ebx = 0x0300;
        run(code);
        return first_fault;
    };
    EXPECT_EQ(fault_of({0xF0, 0x01, 0x07}), -1) << "LOCK ADD [BX],AX";
    EXPECT_EQ(fault_of({0xF0, 0x87, 0x07}), -1) << "LOCK XCHG [BX],AX";
    EXPECT_EQ(fault_of({0xF0, 0x80, 0x37, 0x01}), -1) << "LOCK XOR byte [BX],1";
    EXPECT_EQ(fault_of({0xF0, 0xF7, 0x17}), -1) << "LOCK NOT word [BX]";
    EXPECT_EQ(fault_of({0xF0, 0xFF, 0x07}), -1) << "LOCK INC word [BX]";
    EXPECT_EQ(fault_of({0xF0, 0x0F, 0xAB, 0x07}), -1) << "LOCK BTS [BX],AX";
    EXPECT_EQ(fault_of({0xF0, 0x0F, 0xBA, 0x2F, 0x01}), -1) << "LOCK BTS word [BX],1";
    EXPECT_EQ(fault_of({0xF0, 0x0F, 0xC1, 0x07}), -1) << "LOCK XADD [BX],AX";
    EXPECT_EQ(fault_of({0xF0, 0x0F, 0xB1, 0x0F}), -1) << "LOCK CMPXCHG [BX],CX";
    EXPECT_EQ(fault_of({0xF0, 0x01, 0xD8}), 6) << "register destination";
    EXPECT_EQ(fault_of({0xF0, 0x39, 0x07}), 6) << "CMP doesn't write";
    EXPECT_EQ(fault_of({0xF0, 0x80, 0x3F, 0x01}), 6) << "80 /7 is CMP";
    EXPECT_EQ(fault_of({0xF0, 0x89, 0x07}), 6) << "MOV";
    EXPECT_EQ(fault_of({0xF0, 0x0F, 0xA3, 0x07}), 6) << "BT doesn't write";
    EXPECT_EQ(fault_of({0xF0, 0xFF, 0x17}), 6) << "FF /2 is CALL";
    EXPECT_EQ(fault_of({0xF0, 0x90}), 6) << "NOP";
    EXPECT_EQ(unimpl_count, 0) << "a LOCK #UD is not an unimplemented opcode";
}

TEST_F(Cpu80486Test, AnInstructionLongerThanFifteenBytesRaisesGp) {
    // Intel 80486 PRM, Instruction Format: 15 bytes max, #GP(0) past it
    auto fault_of = [&](std::initializer_list<uint8_t> code) {
        catch_faults();
        cpu->es = 0;
        poke32(0x0200, 0);
        run(code);
        return first_fault;
    };
    // ES x3, 66, 67, C7 05 disp32 imm32: exactly 15 bytes
    EXPECT_EQ(fault_of({0x26, 0x26, 0x26, 0x66, 0x67, 0xC7, 0x05, 0x00, 0x02, 0, 0,
                        0x78, 0x56, 0x34, 0x12}), -1);
    EXPECT_EQ(memd(0x0200), 0x12345678u);
    // one more ES makes 16; the store never happens
    EXPECT_EQ(fault_of({0x26, 0x26, 0x26, 0x26, 0x66, 0x67, 0xC7, 0x05, 0x00, 0x02, 0, 0,
                        0x78, 0x56, 0x34, 0x12}), 13);
    EXPECT_EQ(memd(0x0200), 0u);
    EXPECT_EQ(fault_of({0x26, 0x26, 0x26, 0x26, 0x26, 0x26, 0x26, 0x26, 0x26, 0x26, 0x26, 0x26,
                        0x26, 0x26, 0x90}), -1) << "14 prefixes and NOP is 15 bytes";
    EXPECT_EQ(fault_of({0x26, 0x26, 0x26, 0x26, 0x26, 0x26, 0x26, 0x26, 0x26, 0x26, 0x26, 0x26,
                        0x26, 0x26, 0x26, 0x90}), 13) << "15 prefixes and NOP is 16";
}

TEST_F(Cpu80486Test, In32RunsTwoSixteenBitCyclesAtPortAndPortPlusTwo) {
    // I/O devices are 16-bit, so BS16# splits a 32-bit cycle (Intel486 Data Book, Dynamic Bus Sizing)
    next_in16_val = 0x1234;   // the fixture hands out 1234h, then 1235h
    cpu->edx = 0x0CFC;
    run({0x66, 0xED});        // IN EAX, DX
    ASSERT_EQ(in16_log.size(), 2u);
    EXPECT_EQ(in16_log[0].first, 0x0CFCu);
    EXPECT_EQ(in16_log[1].first, 0x0CFEu);
    EXPECT_EQ(cpu->eax, 0x12351234u) << "all 32 bits, not just AX";
    in16_log.clear();
    run({0x66, 0xE5, 0x40});  // IN EAX, 40h
    ASSERT_EQ(in16_log.size(), 2u);
    EXPECT_EQ(in16_log[1].first, 0x0042u);
}

TEST_F(Cpu80486Test, Out32RunsTwoSixteenBitCyclesAtPortAndPortPlusTwo) {
    cpu->eax = 0xAABBCCDDu;
    cpu->edx = 0x0CF8;
    run({0x66, 0xEF});        // OUT DX, EAX
    ASSERT_EQ(out16_log.size(), 2u);
    EXPECT_EQ(out16_log[0], std::make_pair(uint16_t(0x0CF8), uint16_t(0xCCDD)));
    EXPECT_EQ(out16_log[1], std::make_pair(uint16_t(0x0CFA), uint16_t(0xAABB)));
}

TEST_F(Cpu80486Test, RepInsdStoresDwordsAndAdvancesEdiByFour) {
    next_in16_val = 0x1000;
    cpu->es = 0;
    cpu->edi = 0x0300;
    cpu->edx = 0x01F0;
    cpu->ecx = 2;
    run({0xF3, 0x66, 0x6D});  // REP INSD
    EXPECT_EQ(memd(0x0300), 0x10011000u);
    EXPECT_EQ(memd(0x0304), 0x10031002u);
    EXPECT_EQ(cpu->edi & 0xFFFF, 0x0308u);
    EXPECT_EQ(cpu->ecx & 0xFFFF, 0u);
}

TEST_F(Cpu80486Test, OutsdSendsADwordFromDsSi) {
    poke32(0x0300, 0x87654321u);
    cpu->ds = 0;
    cpu->esi = 0x0300;
    cpu->edx = 0x01F0;
    run({0x66, 0x6F});        // OUTSD
    ASSERT_EQ(out16_log.size(), 2u);
    EXPECT_EQ(out16_log[0].second, 0x4321u);
    EXPECT_EQ(out16_log[1].second, 0x8765u);
    EXPECT_EQ(cpu->esi & 0xFFFF, 0x0304u);
}

TEST_F(Cpu80486Test, RepInsUnderAddr32CountsInEcx) {
    // CX alone is 0, so without 0x67 nothing moves
    cpu->es = 0;
    cpu->edi = 0;
    cpu->edx = 0x01F0;
    cpu->ecx = 0x00010000u;
    run({0xF3, 0x67, 0x6C});  // REP INSB, addr32
    EXPECT_EQ(cpu->ecx, 0u);
    EXPECT_EQ(cpu->edi, 0x00010000u);
}

TEST_F(Cpu80486Test, ACodeFetchPastFfffFaultsInsteadOfWrapping) {
    // the limit covers the whole instruction: an imm8 at 0000h raises #GP
    catch_faults();
    cpu->cs = 0x1000;
    cpu->eip = 0xFFFF;
    cpu->eax = 0;
    mem[0x1FFFF] = 0xB0;
    mem[0x10000] = 0x42;
    cpu->step();
    EXPECT_EQ(first_fault, 13);
    EXPECT_EQ(cpu->eax & 0xFF, 0u);
}

TEST_F(Cpu80486Test, AOneByteInstructionAtFfffStillWrapsIpToZero) {
    // IP is 16 bits, so the next instruction starts at 0000h
    catch_faults();
    cpu->cs = 0x1000;
    cpu->eip = 0xFFFF;
    mem[0x1FFFF] = 0x90;
    cpu->step();
    EXPECT_EQ(first_fault, -1);
    EXPECT_EQ(cpu->eip, 0u);
}

TEST_F(Cpu80486Test, ALongRepYieldsAfterItsBudgetAndCarriesOn) {
    // each continuation pays only its own iterations: prefixes plus 12 + 3n
    cpu->rep_yield_cycles = 9;   // three MOVSB iterations
    cpu->ds = 0; cpu->es = 0;
    cpu->esi = 0x0300;
    cpu->edi = 0x0400;
    cpu->ecx = 10;
    for (int i = 0; i < 10; ++i) mem[0x0300 + i] = uint8_t(i + 1);
    load({0x26, 0xF3, 0xA4});    // ES: REP MOVSB
    cpu->eip = 0;
    EXPECT_EQ(cpu->step(), 2 + 12 + 9) << "two prefix clocks, the setup and three iterations";
    EXPECT_EQ(cpu->eip, 0u) << "back on the first prefix";
    EXPECT_EQ(cpu->ecx & 0xFFFF, 7u);
    EXPECT_EQ(cpu->step(), 9);
    EXPECT_EQ(cpu->step(), 9);
    EXPECT_EQ(cpu->step(), 3);
    EXPECT_EQ(cpu->eip, 3u);
    EXPECT_EQ(cpu->ecx & 0xFFFF, 0u);
    for (int i = 0; i < 10; ++i) EXPECT_EQ(mem[0x0400 + i], uint8_t(i + 1));
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
    // interrupt() is the chipset's PIC entry; real-mode frame is 6 bytes on every CPU
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


// --- protected mode, paging, gates, task switching ---
// enter_pm32() reaches PE with the real LGDT, CR0.PE, far-JMP sequence a DOS extender uses.
// Rules per the Intel 80486 PRM protection and paging chapters.

class Cpu80486PmTest : public ::testing::Test {
protected:
    std::array<uint8_t, 0x100000> mem{};
    std::unique_ptr<Cpu> cpu;
    int unimpl_count = 0;
    struct FaultRec { int vector; uint32_t error; uint16_t cs; uint32_t eip; uint32_t esp; };
    std::vector<FaultRec> faults;

    // flat descriptors have base 0, so linear equals physical unless paging is on
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

    // selectors in the default GDT
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
    // the six Bus::For operations; no device wired, reads float high
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
        // on_fault fires after restartable state is restored and before the handler runs
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

    // 8-byte descriptor layout: base and limit split across bytes for 286 compatibility (Intel 80486 PRM, Segment Descriptors)
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
        // minimal TSS with a ring-0 stack for inter-privilege gates
        for (uint32_t i = 0; i < 104; i += 4) w32(kTss + i, 0);
        w32(kTss + 4, 0x00007E00u);   // ESP0
        w32(kTss + 8, kData32);       // SS0
        w16(kTss + 102, 0x68);        // I/O map base, past the limit
    }

    // enters 32-bit protected mode via the real sequence, loads DS/ES/SS and a stack, CS:EIP at pm_code_
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

    // assembles at pm_code_, runs n instructions; clears halted first since a double fault shuts the CPU down
    // drops to real mode keeping 4GB DS/ES limits, reloads both with 0: unreal mode as HimemX sets it up
    void enter_unreal() {
        enter_pm32();
        put(kData, {0x0F, 0x20, 0xC0, 0x24, 0xFE, 0x0F, 0x22, 0xC0});
        pm_run({0x66, 0xEA, uint8_t(kData), uint8_t(kData >> 8), uint8_t(kCode16), 0x00}, 4);
        put(kData + 0x20, {0x31, 0xC0, 0x8E, 0xD8, 0x8E, 0xC0});  // xor ax,ax / mov ds,ax / mov es,ax
        cpu->eip = kData + 0x20;
        for (int i = 0; i < 3; ++i) cpu->step();
        cpu->eip = kData + 0x40;
    }

    void pm_run(std::initializer_list<uint8_t> code, int n = 1) {
        uint32_t a = pm_code_;
        for (uint8_t b : code) w8(a++, b);
        cpu->halted = false;
        cpu->eip = pm_code_;
        for (int i = 0; i < n; ++i) cpu->step();
    }
    // assemble at `at`, EIP unchanged
    void put(uint32_t at, std::initializer_list<uint8_t> code) {
        for (uint8_t b : code) w8(at++, b);
    }
    // renders the first recorded fault so a failed assertion says which one fired
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

// --- entering protected mode, and what CS.D changes ---

TEST_F(Cpu80486PmTest, TheRealEntrySequenceActuallyEntersThirtyTwoBitProtectedMode) {
    enter_pm32();
    EXPECT_TRUE(cpu->protected_mode());
    EXPECT_EQ(cpu->cs, kCode32);
    EXPECT_EQ(cpu->eip, pm_code_);
    EXPECT_EQ(cpu->cpl(), 0);
    // the descriptor cache holds the GDT's values: base 0, 4GB limit, D=1
    EXPECT_EQ(cpu->desc(Cpu::SEG_CS).base, 0u);
    EXPECT_EQ(cpu->desc(Cpu::SEG_CS).limit, 0xFFFFFFFFu);
    EXPECT_TRUE(cpu->desc(Cpu::SEG_CS).big);
    EXPECT_EQ(cpu->esp, kStackTop);
    EXPECT_TRUE(faults.empty());
    EXPECT_EQ(unimpl_count, 0);
}

TEST_F(Cpu80486PmTest, ThirtyTwoBitCodeSegmentMakesThirtyTwoBitOperandsTheDefault) {
    enter_pm32();
    // no 0x66 in a D=1 segment: B8 takes a full imm32
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
    // byte-granular 4KB segment; past it is #GP(0) with no selector in the error code
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

// the limit is checked before any bus cycle, so a straddling access faults without touching memory (PC486_REVIEW.md §14.3)
TEST_F(Cpu80486PmTest, AnAccessStraddlingTheLimitFaultsBeforeMovingAnyByte) {
    enter_pm32();
    w32(0x0FFC, 0xA5A5A5A5u);
    pm_run({0x66, 0xB8, uint8_t(kSmall), 0x00, 0x8E, 0xC0}, 2);   // mov ax,kSmall / mov es,ax
    // es: mov [0FFEh],eax; offsets 0FFEh/0FFFh are in range, 1000h/1001h are not
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
    // an execute-only code segment cannot be loaded into a data register (PRM, Data Segment Descriptor)
    pm_run({0x66, 0xB8, uint8_t(kExecOnly), 0x00, 0x8E, 0xC0}, 2);
    expect_fault(cpu80486::EXC_GP, kExecOnly & 0xFFFCu);
}

TEST_F(Cpu80486PmTest, ExpandDownSegmentInvertsTheLimitCheck) {
    enter_pm32();
    // expand-down segments are valid from limit+1 upward (PRM, Expand-Down Data Segments)
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
    // a read-only data segment is not a legal stack
    pm_run({0x66, 0xB8, uint8_t(kRoData), 0x00, 0x8E, 0xD0}, 2);
    expect_fault(cpu80486::EXC_GP, kRoData & 0xFFFCu);
    faults.clear();
    // nor is a DPL-3 segment at CPL 0: SS DPL and RPL must equal CPL
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
    // the one write a segment load performs (PRM, Accessed Bit)
    uint32_t hi_addr = kGdt + (kSmall & 0xFFF8u) + 4;
    EXPECT_EQ(r32(hi_addr) & 0x100u, 0u);
    pm_run({0x66, 0xB8, uint8_t(kSmall), 0x00, 0x8E, 0xC0}, 2);
    EXPECT_NE(r32(hi_addr) & 0x100u, 0u);
}

// --- far transfers (PRM, "Calls and Jumps to Code Segments") --------------

TEST_F(Cpu80486PmTest, FarJumpToANonConformingSegmentRequiresAnExactCplMatch) {
    enter_pm32();
    // CPL 0 to a DPL-3 non-conforming segment is #GP; privilege changes only via a gate or return
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
    // a conforming segment needs DPL at least as privileged as CPL and does not change CPL
    pm_run({0xEA, uint8_t(kData), uint8_t(kData >> 8), 0x00, 0x00, uint8_t(kConform), 0x00}, 1);
    EXPECT_TRUE(faults.empty());
    EXPECT_EQ(cpu->cpl(), 0);
    EXPECT_EQ(cpu->cs & 0xFFFCu, kConform & 0xFFFCu);
}

TEST_F(Cpu80486PmTest, CallGateRaisesPrivilegeAndSwitchesToTheTssStack) {
    enter_pm32();
    // reach ring 3 with a hand-built IRETD frame
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
    // 32-bit call gate at 78h, DPL 3, targeting the DPL-0 flat code segment
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
    // only CALL can raise privilege, since only CALL leaves a way back
    set_desc(0x78, gate_desc(kCode3, kData, 0xEC));
    pm_run({0xEA, 0x00, 0x00, 0x00, 0x00, 0x78, 0x00}, 1);   // jmp far 78h:0
    expect_fault(cpu80486::EXC_GP, kCode3 & 0xFFFCu);
}

// --- the LDT --------------------------------------------------------------

TEST_F(Cpu80486PmTest, LldtLoadsTheLdtAndLdtSelectorsResolveThroughIt) {
    enter_pm32();
    // an LDT selector has bit 2 set; LDT index 1 is selector 0Ch
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
    // SLDT reads it back
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
    // these answer "could I load this?" without faulting
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
    // the access byte is in bits 8-15 of the result
    EXPECT_EQ((cpu->ebx >> 8) & 0xFFu, 0x92u);
    pm_run({0x66, 0xB8, uint8_t(kSmall), 0x00, 0x0F, 0x03, 0xD8}, 2);  // lsl ebx,eax
    EXPECT_TRUE(cpu->flag(cpu80486::FLAG_ZF));
    EXPECT_EQ(cpu->ebx, 0x00000FFFu);
    // the 4KB-granular limit comes back in bytes
    pm_run({0x66, 0xB8, uint8_t(kData32), 0x00, 0x0F, 0x03, 0xD8}, 2);
    EXPECT_EQ(cpu->ebx, 0xFFFFFFFFu) << "a G=1 limit of FFFFFh is 4GB of bytes";
}

TEST_F(Cpu80486PmTest, ArplRaisesTheRequestedPrivilegeLevel) {
    enter_pm32();
    // ARPL stops a caller smuggling in more privilege than it has
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
    // identity-maps 0-4MB plus one extra 4MB region so translation is observable
    void build_page_tables(uint32_t pte_flags = 0x07) {
        w32(kPageDir + 0 * 4, kPageTab | 0x07u);
        for (uint32_t p = 0; p < 1024; ++p) w32(kPageTab + p * 4, (p << 12) | pte_flags);
    }
    void map_region(int dir_index, uint32_t table_phys, uint32_t frame, uint32_t flags = 0x07) {
        w32(kPageDir + uint32_t(dir_index) * 4, table_phys | 0x07u);
        for (uint32_t p = 0; p < 1024; ++p) w32(table_phys + p * 4, 0);
        w32(table_phys + 0 * 4, frame | flags);
    }
    // turns paging on from protected mode
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
    // linear 0x00400000 (PDE 1) maps to kFrame; a store landing there proves a real page walk
    map_region(1, 0xB000, kFrame);
    // a second linear window onto the same frame: aliasing only real translation can produce
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
    // linear 0x00400000 has no page-directory entry
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
    // present but read-only page, written by a supervisor with CR0.WP set
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
    // CR0.WP is new on the Intel486; clear (386 behavior), a supervisor write ignores R/W
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
    // clear them so the walk's own writes are unambiguous
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
    // repoint the page table behind the TLB's back
    w32(0xB000, 0x0000C000u | 0x07u);
    put(paged_code_ + 10, {0xB8, 0x02, 0x00, 0x00, 0x00,
                           0xA3, 0x00, 0x00, 0x40, 0x00});
    cpu->eip = paged_code_ + 10;
    for (int i = 0; i < 2; ++i) cpu->step();
    EXPECT_EQ(r32(kFrame), 2u) << "the stale TLB entry is still in use, as on real hardware";
    EXPECT_EQ(r32(0x0000C000u), 0u);
    // INVLPG drops that entry; the next access re-walks
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
    // ring-3 code and stack pages stay user-accessible, the target is supervisor-only
    map_region(1, 0xB000, kFrame, 0x03);   // present, writable, NOT user
    enable_paging();
    // IRETD outward nulls segment registers ring 3 may not use, so ring-3 code reloads DS first
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

// TLB miss costs 13, 21 or 28 bus clocks by how many entries need A/D written back (Embedded Intel486 Developer's Manual 27302101, 12.3.1, rule 10); board timing only
TEST_F(Cpu80486PagingTest, ATlbMissCostsItsWalkWithBoardTiming) {
    enter_pm32();
    build_page_tables();
    map_region(1, 0xB000, kFrame);
    enable_paging();
    pc486::Cache486 cache;
    cpu->timing = &cache;
    cpu->enable_cache();
    const uint32_t at = paged_code_;
    put(at, {0xA1, 0x00, 0x00, 0x40, 0x00,          // mov eax,[00400000h]
             0x0F, 0x01, 0x3D, 0x00, 0x00, 0x40, 0x00,   // invlpg [00400000h]
             0xA3, 0x00, 0x00, 0x40, 0x00});        // mov [00400000h],eax
    auto step_at = [&](uint32_t eip) { cpu->eip = eip; return cpu->step(); };
    step_at(at);                                    // walk, and warm every line
    step_at(at + 12);
    w32(0xB000, kFrame | 0x27u);                    // A set, D clear again
    step_at(at + 5);
    int walked = step_at(at);                       // A bits already set
    int hit = step_at(at);
    EXPECT_EQ(walked - hit, 2 * 13);
    step_at(at + 5);
    int dirtied = step_at(at + 12);                 // a write: D needs setting
    int write_hit = step_at(at + 12);
    EXPECT_EQ(dirtied - write_hit, 2 * 21);
    EXPECT_TRUE(faults.empty()) << fault_desc();
}

// PCD keeps a page out of the L1; the 471 has no PCD input, so the L2 is unaffected
TEST_F(Cpu80486PagingTest, APcdPageIsNeverFilledIntoTheL1) {
    enter_pm32();
    build_page_tables();
    map_region(1, 0xB000, kFrame, 0x17);            // present, writable, user, PCD
    map_region(3, 0xC000, kFrame + 0x1000, 0x07);
    enable_paging();
    pc486::Cache486 cache;
    cpu->timing = &cache;
    cpu->enable_cache();
    paged_run({0xA1, 0x00, 0x00, 0x40, 0x00,        // mov eax,[00400000h]
               0xA1, 0x00, 0x00, 0xC0, 0x00}, 2);   // mov eax,[00C00000h]
    EXPECT_FALSE(cache.l1_has(kFrame));
    EXPECT_TRUE(cache.l1_has(kFrame + 0x1000));
    EXPECT_TRUE(faults.empty()) << fault_desc();
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
    // frame: EIP, CS, EFLAGS for a 32-bit gate
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
    // a DPL-0 gate: ring 3 cannot reach it with INT n
    set_gate(0x40, gate_desc(kCode32, kData, 0x8E));
    // LTR first: the hardware-interrupt half crosses ring 3 to ring 0 and takes its stack from the TSS
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
    // INT n needs gate DPL >= CPL; error code is the IDT offset plus 2
    expect_fault(cpu80486::EXC_GP, 0x40u * 8 + 2);
    faults.clear();
    cpu->halted = false;   // the #GP above had no gate, so it shut the CPU down
    // hardware interrupts are exempt
    cpu->interrupt(0x40);
    EXPECT_TRUE(faults.empty());
    EXPECT_EQ(cpu->cpl(), 0);
    EXPECT_EQ(cpu->eip, kData);
}

TEST_F(Cpu80486PmTest, InterPrivilegeInterruptSwitchesStackFromTheTssAndIretdReturns) {
    enter_pm32();
    // LTR so the CPU has a TSS for SS0/ESP0
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
    // with a privilege change the frame adds ESP and SS
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

TEST_F(Cpu80486PmTest, OutwardIretdLoadsIoplFromTheStackImage) {
    // Intel 80486 PRM, IRET: IOPL changes only when the executing CPL is 0; a ring-0 monitor can hand ring 3 an elevated IOPL
    enter_pm32();
    put(pm_code_ + 7, {0x68, uint8_t(kStack3 | 3), 0x00, 0x00, 0x00,
                       0x68, 0x00, 0x7C, 0x00, 0x00,
                       0x68, 0x02, 0x32, 0x00, 0x00,   // EFLAGS: IOPL 3, IF set
                       0x68, uint8_t(kCode3), 0x00, 0x00, 0x00,
                       0x68, 0x00, 0x64, 0x00, 0x00,
                       0xCF});
    cpu->eip = pm_code_ + 7;
    for (int i = 0; i < 5; ++i) cpu->step();
    ASSERT_TRUE(faults.empty());
    ASSERT_EQ(cpu->cpl(), 0) << "still ring 0 -- only the pushes have run";
    cpu->step();   // iretd, ring 0 -> ring 3
    EXPECT_TRUE(faults.empty());
    EXPECT_EQ(cpu->cpl(), 3) << "IRETD returned outward";
    EXPECT_EQ((cpu->eflags & uint32_t(cpu80486::FLAG_IOPL)) >> 12, 3u)
        << "a ring-0 IRETD can hand a lower-privilege task an elevated IOPL";
}

TEST_F(Cpu80486PmTest, AnExceptionPushesItsErrorCode) {
    enter_pm32();
    put(kData, {0xF4});
    set_gate(cpu80486::EXC_GP, gate_desc(kCode32, kData, 0x8E));
    // a #GP from a bad SS carries the selector
    pm_run({0x66, 0xB8, uint8_t(kRoData), 0x00, 0x8E, 0xD0}, 2);
    EXPECT_EQ(cpu->eip, kData);
    // frame: EIP, CS, EFLAGS, error code on top
    EXPECT_EQ(r32(cpu->esp), kRoData & 0xFFFCu);
    EXPECT_EQ(cpu->esp, kStackTop - 16);
}

TEST_F(Cpu80486PmTest, AVectorPastTheIdtLimitIsAGeneralProtectionFault) {
    enter_pm32();
    // shrink the IDT to 16 vectors, ask for 40h
    w16(kIdtPtr, 0x7F);
    pm_run({0x0F, 0x01, 0x1E, 0x00, 0x00}, 0);   // (assembled but not run -- see below)
    // LIDT needs a 32-bit operand form to reach kIdtPtr as disp32
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
    // empty IDT: #GP has no gate (#DF), #DF has none (shutdown until RESET)
    for (uint32_t v = 0; v < 32; ++v) set_gate(int(v), 0);
    pm_run({0x66, 0xB8, uint8_t(kRoData), 0x00, 0x8E, 0xD0}, 2);
    ASSERT_GE(faults.size(), 2u);
    EXPECT_EQ(faults[0].vector, cpu80486::EXC_GP);
    EXPECT_EQ(faults[1].vector, cpu80486::EXC_DF);
    EXPECT_TRUE(cpu->halted);
}

TEST_F(Cpu80486PmTest, AFaultRestoresTheStackPointerSoTheInstructionCanRestart) {
    enter_pm32();
    // a #SS on PUSH must leave ESP as before the PUSH so the restart does not decrement twice
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
    // type 9 (available) becomes B (busy) in the descriptor
    EXPECT_EQ((r32(kGdt + (kTssSel & 0xFFF8u) + 4) >> 8) & 0x0Fu, 0x0Bu);
    // STR reads the selector back
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
    // outgoing registers land in the outgoing TSS; the incoming TSS is loaded wholesale
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
    // JMP hands over: old busy bit clears, new one sets, NT stays clear
    EXPECT_EQ((r32(kGdt + (kTssSel & 0xFFF8u) + 4) >> 8) & 0x0Fu, 0x09u);
    EXPECT_EQ((r32(kGdt + (kTss2Sel & 0xFFF8u) + 4) >> 8) & 0x0Fu, 0x0Bu);
    EXPECT_FALSE(cpu->flag(cpu80486::FLAG_NT));
    // CR0.TS is set so the first FPU instruction traps
    EXPECT_NE(cpu->cr(0) & uint32_t(cpu80486::CR0_TS), 0u);
}

TEST_F(Cpu80486PmTest, ATssWithItsTBitSetTrapsAfterTheSwitch) {
    // Intel 80386 PRM, Debug Exceptions: TSS T bit (offset 64h) raises #DB after the switch with DR6.BT
    enter_pm32();
    for (uint32_t i = 0; i < 104; i += 4) w32(kTss2 + i, 0);
    put(kData, {0xF4});
    put(kData + 0x20, {0xF4});
    set_gate(1, gate_desc(kCode32, kData + 0x20, 0x8E));
    w32(kTss2 + 28, kPageDir);
    w32(kTss2 + 32, kData);
    w32(kTss2 + 36, 0x00000002u);
    w32(kTss2 + 56, 0x00007A00u);
    w32(kTss2 + 72, kData32);
    w32(kTss2 + 76, kCode32);
    w32(kTss2 + 80, kData32);
    w32(kTss2 + 84, kData32);
    w32(kTss2 + 100, 1u);            // T
    pm_run({0x66, 0xB8, uint8_t(kTssSel), 0x00, 0x0F, 0x00, 0xD8}, 2);
    put(pm_code_ + 7, {0xEA, 0x00, 0x00, 0x00, 0x00, uint8_t(kTss2Sel), 0x00});
    cpu->eip = pm_code_ + 7;
    cpu->step();
    ASSERT_TRUE(faults.empty()) << fault_desc();
    EXPECT_EQ(cpu->tr_selector(), kTss2Sel);
    EXPECT_EQ(cpu->eip, kData + 0x20) << "in the #DB handler before the task's first instruction";
    EXPECT_EQ(r32(cpu->esp), kData) << "the trap frame points at the new task's entry";
    EXPECT_NE(cpu->dr(6) & 0x8000u, 0u) << "DR6.BT";
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
    // both tasks are busy while nested
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
    // a task gate names a TSS selector, giving #DF its own stack
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
    // CPL 0 with IOPL 0 is fine
    pm_run({0xFA}, 1);
    EXPECT_TRUE(faults.empty());
    // CPL 3 with IOPL 0 is not
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
    // I/O permission bitmap: a set bit denies. Port 60h allowed, 70h denied.
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

TEST_F(Cpu80486PmTest, InsAndOutsTakeThePortPermissionCheck) {
    // INS and OUTS are I/O accesses (Intel 80486 PRM, INS)
    enter_pm32();
    put(kData, {0xF4});
    set_gate(cpu80486::EXC_GP, gate_desc(kCode32, kData, 0x8E));
    set_desc(kTssSel, seg_desc(kTss, 0x67 + 0x20, 0x89, false, false));
    w16(kTss + 102, 0x68);
    for (uint32_t i = 0; i < 0x20; ++i) w8(kTss + 0x68 + i, 0xFF);
    w8(kTss + 0x68 + (0x60 / 8), 0x00);         // ports 60h-67h permitted
    pm_run({0x66, 0xB8, uint8_t(kTssSel), 0x00, 0x0F, 0x00, 0xD8}, 2);
    ASSERT_TRUE(faults.empty());
    put(kData + 0x400, {0x16, 0x07,                     // push ss / pop es
                        0xBF, 0x00, 0x70, 0x00, 0x00,   // mov edi,7000h
                        0xBA, 0x60, 0x00, 0x00, 0x00,   // mov edx,60h
                        0x6C,                           // insb -- permitted
                        0xBA, 0x70, 0x00, 0x00, 0x00,   // mov edx,70h
                        0x6C});                         // insb -- denied
    put(pm_code_ + 7, {0x68, uint8_t(kStack3 | 3), 0x00, 0x00, 0x00,
                       0x68, 0x00, 0x7C, 0x00, 0x00,
                       0x68, 0x02, 0x02, 0x00, 0x00,
                       0x68, uint8_t(kCode3), 0x00, 0x00, 0x00,
                       0x68, 0x00, 0x64, 0x00, 0x00,
                       0xCF});
    cpu->eip = pm_code_ + 7;
    for (int i = 0; i < 6; ++i) cpu->step();
    ASSERT_EQ(cpu->cpl(), 3);
    for (int i = 0; i < 5; ++i) cpu->step();
    EXPECT_TRUE(faults.empty()) << fault_desc();
    cpu->step();
    cpu->step();
    expect_fault(cpu80486::EXC_GP, 0, "port 70h's bit is set, so INSB is denied");
}

TEST_F(Cpu80486PmTest, PopfdCannotChangeIoplOutsideRingZeroOrIfAboveIopl) {
    enter_pm32();
    // at CPL 0 IOPL is writable
    pm_run({0x68, 0x02, 0x30, 0x00, 0x00, 0x9D}, 2);   // push 3002h / popfd
    EXPECT_EQ((cpu->eflags & uint32_t(cpu80486::FLAG_IOPL)) >> 12, 3u);
    // at CPL 3 it is silently ignored (Intel 80486 PRM, POPF)
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
    // drop to a 16-bit code segment, then clear PE; base 0 keeps CS of 0 in place
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
    // PC486_REVIEW.md §5.4: a memory manager loads a 4GB descriptor in a brief PM excursion, drops PE, and the cached limit survives
    EXPECT_EQ(cpu->desc(Cpu::SEG_ES).limit, 0xFFFFFFFFu);
    put(kData, {0x0F, 0x20, 0xC0, 0x24, 0xFE, 0x0F, 0x22, 0xC0});
    pm_run({0x66, 0xEA, uint8_t(kData), uint8_t(kData >> 8), uint8_t(kCode16), 0x00}, 4);
    ASSERT_FALSE(cpu->protected_mode());
    // a real-mode segment load sets the base and keeps the cached limit
    put(kData + 0x20, {0x31, 0xC0, 0x8E, 0xC0});   // xor ax,ax / mov es,ax
    cpu->eip = kData + 0x20;
    cpu->step();
    cpu->step();
    EXPECT_EQ(cpu->desc(Cpu::SEG_ES).base, 0u);
    EXPECT_EQ(cpu->desc(Cpu::SEG_ES).limit, 0xFFFFFFFFu)
        << "the limit is not reset by a real-mode load -- that is unreal mode";
    EXPECT_TRUE(faults.empty());
}

TEST_F(Cpu80486PmTest, UnrealModeAddr32ReachesPastSixtyFourK) {
    // with a 4GB limit cached, a 32-bit offset above 0FFFFh passes (FreeDOS HimemX, PC486_REVIEW.md §5.4)
    enter_unreal();
    ASSERT_FALSE(cpu->protected_mode());
    w16(0x0200, 0x7777);
    w16(0x10200, 0x1234);
    put(kData + 0x40, {0x67, 0x8B, 0x05, 0x00, 0x02, 0x01, 0x00});  // MOV AX, [00010200h]
    cpu->step();
    EXPECT_TRUE(faults.empty()) << fault_desc();
    EXPECT_EQ(cpu->eax & 0xFFFF, 0x1234u) << "offset must NOT wrap mod 64K";
}

TEST_F(Cpu80486PmTest, UnrealModeAddr32StringOpUsesEsiEdiEcxAboveSixtyFourK) {
    // HimemX copies XMS with F3 67 66 A5 (REP MOVSD, addr32); a truncating core fails
    enter_unreal();
    w32(0x00020000, 0xDEADBEEFu);
    w32(0x00020004, 0xCAFEBABEu);
    cpu->esi = 0x00020000u;
    cpu->edi = 0x00030000u;
    cpu->ecx = 2;
    put(kData + 0x40, {0xF3, 0x67, 0x66, 0xA5});
    cpu->step();
    EXPECT_TRUE(faults.empty()) << fault_desc();
    EXPECT_EQ(r32(0x00030000), 0xDEADBEEFu);
    EXPECT_EQ(r32(0x00030004), 0xCAFEBABEu);
    EXPECT_EQ(cpu->esi, 0x00020008u) << "ESI, not SI, advances";
    EXPECT_EQ(cpu->edi, 0x00030008u) << "EDI, not DI, advances";
    EXPECT_EQ(cpu->ecx, 0u) << "ECX is the counter under a 0x67 prefix";
}


// --- Virtual-8086 mode ---
// Built on the paging fixture: enter_v86() runs LTR, nine pushes and an IRETD with EFLAGS.VM set, as JEMMEX does (PC486_REVIEW.md §5.9).
// References: Intel 80386 PRM chapter 15 (15.3 entering and leaving, 15.4 sensitive instructions, 15.5 virtual I/O); the 486 behaves the same.

class Cpu80486V86Test : public Cpu80486PagingTest {
protected:
    // V86 segment 0F00h: offsets land at linear 0F000h + offset
    static constexpr uint16_t kV86Seg  = 0x0F00;
    static constexpr uint32_t kV86Base = uint32_t(kV86Seg) << 4;
    // EIP inside the monitor's prefetch window (monitor on linear page 3000h, EIP 3100h); see EnteringV86ClearsTheStalePrefetchWindow
    static constexpr uint32_t kV86Off  = 0x3100;
    static constexpr uint32_t kV86Sp   = 0x0F00;
    static constexpr uint32_t kIopl3   = 3u << 12;
    static constexpr uint32_t kRing0Sp = 0x00007E00;   // TSS ESP0, from build_default_gdt()

    // where an 8086 offset lands
    static uint32_t v86_lin(uint32_t off) { return kV86Base + off; }
    uint16_t r16(uint32_t a) const { return uint16_t(mem[a] | (uint16_t(mem[a + 1]) << 8)); }

    // I/O permission bitmap with only port 60h granted; must precede LTR, which caches the TSS limit
    void give_tss_io_bitmap() {
        set_desc(kTssSel, seg_desc(kTss, 0x67 + 0x20, 0x89, false, false));
        w16(kTss + 102, 0x68);
        for (uint32_t i = 0; i < 0x20; ++i) w8(kTss + 0x68 + i, 0xFF);
        w8(kTss + 0x68 + (0x60 / 8), 0x00);
    }

    // runs the monitor's entry sequence at `at`: LTR, the nine-dword frame (GS FS DS ES SS ESP EFLAGS CS EIP pushed in order), IRETD; flags_image is ORed into EFLAGS
    void enter_v86(uint32_t flags_image = 0, uint32_t at = 0) {
        std::vector<uint8_t> c;
        auto b = [&](std::initializer_list<int> v) { for (int x : v) c.push_back(uint8_t(x)); };
        auto pushd = [&](uint32_t v) {
            c.push_back(0x68);
            for (int i = 0; i < 4; ++i) c.push_back(uint8_t(v >> (8 * i)));
        };
        b({0x66, 0xB8, kTssSel, 0x00, 0x0F, 0x00, 0xD8});   // mov ax,kTssSel / ltr ax
        pushd(0);                                            // GS
        pushd(0);                                            // FS
        pushd(kV86Seg);                                      // DS
        pushd(kV86Seg);                                      // ES
        pushd(kV86Seg);                                      // SS
        pushd(kV86Sp);                                       // ESP
        pushd(uint32_t(cpu80486::FLAG_VM) | uint32_t(cpu80486::FLAG_R1) | flags_image);
        pushd(kV86Seg);                                      // CS
        pushd(kV86Off);                                      // EIP
        b({0xCF});                                           // iretd
        uint32_t a = (at != 0) ? at : pm_code_;
        for (uint8_t x : c) w8(a++, x);
        cpu->eip = a - uint32_t(c.size());
        for (int i = 0; i < 12; ++i) cpu->step();
    }

    // assemble 8086 code at the V86 entry point
    void v86_code(std::initializer_list<uint8_t> code) { put(v86_lin(kV86Off), code); }

    // ring-0 frame a V86 trap left, by offset from the handler's ESP: 0 EIP, 8 EFLAGS, 32 GS
    uint32_t frame(uint32_t off) const { return r32(cpu->esp + off); }

public:
    // binds the optional Bus::page / map_epoch path so the page cache and prefetch window are live (PC486_REVIEW.md §15); above 1MB is open bus
    uint8_t *page_host(uint32_t page_base, bool) {
        return page_base < mem.size() ? &mem[page_base] : nullptr;
    }
    const uint32_t *map_epoch() { return &map_epoch_; }

protected:
    // rebuilt so Bus::For() sees this class and binds page_host()/map_epoch()
    void SetUp() override {
        Cpu80486PagingTest::SetUp();
        cpu = std::make_unique<Cpu>(Bus::For(this));
        cpu->reset();
        cpu->on_unimplemented = [this](uint16_t, uint32_t, uint16_t) { ++unimpl_count; };
        cpu->on_fault = [this](int v, uint32_t e, uint16_t c, uint32_t ip) {
            if (faults.size() < 8) faults.push_back({v, e, c, ip, cpu->esp});
        };
    }

private:
    uint32_t map_epoch_ = 1;
};

// --- entering the mode ---

TEST_F(Cpu80486V86Test, IretdFromRingZeroWithVmSetEntersVirtualEightyEightySixMode) {
    enter_pm32();
    enter_v86();
    ASSERT_TRUE(faults.empty()) << fault_desc();
    EXPECT_TRUE(cpu->flag(cpu80486::FLAG_VM));
    EXPECT_TRUE(cpu->v86_mode());
    EXPECT_TRUE(cpu->protected_mode()) << "V86 is a submode of protected mode, not a third mode";
    EXPECT_EQ(cpu->cpl(), 3) << "CPL is always three in V86 mode";
    EXPECT_EQ(cpu->cs, kV86Seg);
    EXPECT_EQ(cpu->ss, kV86Seg);
    EXPECT_EQ(cpu->ds, kV86Seg);
    EXPECT_EQ(cpu->es, kV86Seg);
    EXPECT_EQ(cpu->fs, 0);
    EXPECT_EQ(cpu->gs, 0);
    EXPECT_EQ(cpu->eip, kV86Off);
    EXPECT_EQ(cpu->esp, kV86Sp);
    // descriptor caches become 8086 segments (base selector*16, 64KB, 16-bit); keeping the flat ones would be unreal mode (PC486_REVIEW.md §5.4)
    for (int si : {int(Cpu::SEG_CS), int(Cpu::SEG_SS), int(Cpu::SEG_DS), int(Cpu::SEG_ES)}) {
        EXPECT_EQ(cpu->desc(si).base, kV86Base) << "segment index " << si;
        EXPECT_EQ(cpu->desc(si).limit, 0xFFFFu) << "segment index " << si;
        EXPECT_FALSE(cpu->desc(si).big) << "segment index " << si;
    }
}

TEST_F(Cpu80486V86Test, AV86TaskAddressesMemoryTheWayAnEightyEightySixDoes) {
    enter_pm32();
    enter_v86();
    ASSERT_TRUE(faults.empty()) << fault_desc();
    // three bytes, not five: V86 code is 16-bit
    v86_code({0xB8, 0x34, 0x12,          // mov ax,1234h
              0xA3, 0x00, 0x50});        // mov [5000h],ax
    cpu->step();
    cpu->step();
    EXPECT_TRUE(faults.empty()) << fault_desc();
    EXPECT_EQ(cpu->eax & 0xFFFFu, 0x1234u);
    EXPECT_EQ(cpu->eip, kV86Off + 6);
    EXPECT_EQ(r16(v86_lin(0x5000)), 0x1234u) << "DS = 0F00h, so the store is at 0F000h + 5000h";
    EXPECT_EQ(r16(0x5000), 0u) << "and not at the monitor's flat 5000h";
}

TEST_F(Cpu80486V86Test, AV86WordAtOffsetFfffRaisesGpInsteadOfWrapping) {
    enter_pm32();
    enter_v86();
    // V86 limit is FFFFh: a word read there is #GP(0); an 8086 wrapped to 0
    w8(v86_lin(0xFFFF), 0xCD);
    w8(v86_lin(0x0000), 0xAB);
    v86_code({0xA1, 0xFF, 0xFF});   // mov ax,[0FFFFh]
    cpu->step();
    ASSERT_FALSE(faults.empty());
    EXPECT_EQ(faults[0].vector, 13);
    EXPECT_EQ(faults[0].error, 0u);
}

TEST_F(Cpu80486V86Test, AV86TaskRunsItsEightyEightySixAddressesThroughThePageTables) {
    enter_pm32();
    build_page_tables();
    // remap the page the V86 store hits (linear 0F000h + 5000h) onto kFrame; paging depends on CR0.PG alone, so V86 sits on the monitor's page tables
    w32(kPageTab + (v86_lin(0x5000) >> 12) * 4, kFrame | 0x07u);
    enable_paging();
    ASSERT_TRUE(cpu->paging_enabled());
    ASSERT_TRUE(faults.empty()) << fault_desc();
    enter_v86(0, paged_code_);
    ASSERT_TRUE(faults.empty()) << fault_desc();
    ASSERT_EQ(cpu->cpl(), 3);
    v86_code({0xB8, 0xEF, 0xBE,          // mov ax,0BEEFh
              0xA3, 0x00, 0x50});        // mov [5000h],ax
    cpu->step();
    cpu->step();
    EXPECT_TRUE(faults.empty()) << fault_desc();
    EXPECT_EQ(r16(kFrame), 0xBEEFu) << "the 8086 address was translated, not used directly";
    EXPECT_EQ(r16(v86_lin(0x5000)), 0u);
}

TEST_F(Cpu80486V86Test, EnteringV86ClearsTheStalePrefetchWindow) {
    enter_pm32();
    // the monitor's prefetch window covers page 3000h-3FFFh; a surviving window would run linear 3100h instead of 0F000h + 3100h
    put(0x3100, {0xB0, 0xA5});                 // mov al,0A5h -- must NOT run
    put(v86_lin(kV86Off), {0xB0, 0x5A});       // mov al,5Ah  -- the real V86 instruction
    enter_v86();
    ASSERT_TRUE(faults.empty()) << fault_desc();
    ASSERT_EQ(cpu->eip, kV86Off);
    cpu->step();
    EXPECT_EQ(cpu->eax & 0xFFu, 0x5Au) << "the stale window would have executed 0A5h";
}

// --- leaving the mode: extended interrupt frame (PRM 15.3) ---

TEST_F(Cpu80486V86Test, AnInterruptOutOfV86PushesTheEightyEightySixSegmentsAndClearsVm) {
    enter_pm32();
    // DPL-3 32-bit interrupt gate to nonconforming ring-0 code, as the PRM requires
    set_gate(0x40, gate_desc(kCode32, kData, 0xEE));
    put(kData, {0xF4});                        // the handler just halts
    enter_v86(kIopl3);                         // IOPL 3: INT n is not trapped
    ASSERT_TRUE(faults.empty()) << fault_desc();
    v86_code({0xCD, 0x40});
    cpu->step();
    ASSERT_TRUE(faults.empty()) << fault_desc();
    EXPECT_FALSE(cpu->flag(cpu80486::FLAG_VM)) << "the handler runs in ordinary protected mode";
    EXPECT_EQ(cpu->cpl(), 0) << "a V86 gate always lands at ring 0";
    EXPECT_EQ(cpu->cs, kCode32);
    EXPECT_EQ(cpu->eip, kData);
    EXPECT_EQ(cpu->ss, kData32) << "SS:ESP came from TSS.SS0/ESP0";
    // all four are zeroed
    EXPECT_EQ(cpu->es, 0);
    EXPECT_EQ(cpu->ds, 0);
    EXPECT_EQ(cpu->fs, 0);
    EXPECT_EQ(cpu->gs, 0);
    EXPECT_TRUE(cpu->desc(Cpu::SEG_DS).null);
    // nine dwords: EIP CS EFLAGS ESP SS ES DS FS GS, GS highest
    EXPECT_EQ(cpu->esp, kRing0Sp - 36);
    EXPECT_EQ(frame(0), kV86Off + 2) << "EIP, past the INT";
    EXPECT_EQ(frame(4), kV86Seg) << "CS";
    EXPECT_NE(frame(8) & uint32_t(cpu80486::FLAG_VM), 0u)
        << "the pushed EFLAGS still says the interrupted code was an 8086 program";
    EXPECT_EQ(frame(12), kV86Sp) << "the 8086 task's own ESP";
    EXPECT_EQ(frame(16), kV86Seg) << "SS";
    EXPECT_EQ(frame(20), kV86Seg) << "ES";
    EXPECT_EQ(frame(24), kV86Seg) << "DS";
    EXPECT_EQ(frame(28), 0u) << "FS";
    EXPECT_EQ(frame(32), 0u) << "GS";
}

TEST_F(Cpu80486V86Test, AHardwareInterruptOutOfV86PushesTheSameExtendedFrame) {
    enter_pm32();
    // delivered from outside: no gate-DPL check, no IOPL, same V86 frame
    set_gate(0x40, gate_desc(kCode32, kData, 0x8E));
    put(kData, {0xF4});
    enter_v86();
    ASSERT_TRUE(faults.empty()) << fault_desc();
    ASSERT_EQ(cpu->eip, kV86Off);
    cpu->interrupt(0x40);
    ASSERT_TRUE(faults.empty()) << fault_desc();
    EXPECT_FALSE(cpu->flag(cpu80486::FLAG_VM));
    EXPECT_EQ(cpu->cpl(), 0);
    EXPECT_EQ(cpu->esp, kRing0Sp - 36);
    EXPECT_EQ(frame(0), kV86Off) << "an interrupted, not completed, instruction";
    EXPECT_NE(frame(8) & uint32_t(cpu80486::FLAG_VM), 0u);
    EXPECT_EQ(frame(32), 0u) << "GS sits at the top of the frame";
    EXPECT_EQ(frame(20), kV86Seg) << "and ES below SS";
}

TEST_F(Cpu80486V86Test, AV86InterruptThroughAConformingOrRingThreeTargetIsRefused) {
    enter_pm32();
    // PRM requires a nonconforming ring-0 target; a conforming one would leave the handler at CPL 3 with no ring-0 stack
    set_gate(0x40, gate_desc(kConform, kData, 0xEE));
    set_gate(cpu80486::EXC_GP, gate_desc(kCode32, kData + 0x100, 0x8E));
    put(kData + 0x100, {0xF4});
    enter_v86(kIopl3);
    ASSERT_TRUE(faults.empty()) << fault_desc();
    v86_code({0xCD, 0x40});
    cpu->step();
    expect_fault(cpu80486::EXC_GP, kConform & 0xFFFCu);
}

// --- IOPL-sensitive instructions (PRM 15.4) ---

TEST_F(Cpu80486V86Test, CliAndStiTrapToTheMonitorBelowIoplThree) {
    enter_pm32();
    set_gate(cpu80486::EXC_GP, gate_desc(kCode32, kData, 0x8E));
    put(kData, {0xF4});
    enter_v86();   // IOPL 0
    ASSERT_TRUE(faults.empty()) << fault_desc();
    v86_code({0xFA});   // cli
    cpu->step();
    expect_fault(cpu80486::EXC_GP, 0, "CLI at IOPL 0 in V86");
}

// HLT at CPL 3 is #GP(0); FreeDOS EMMQXXX0 is a HLT in the UMB that JEMMEX catches. The monitor skips it and IRETs.
TEST_F(Cpu80486V86Test, HltTrapsToTheMonitorWhichCanSkipIt) {
    enter_pm32();
    set_gate(cpu80486::EXC_GP, gate_desc(kCode32, kData, 0x8E));
    // #GP error code is on the stack; bump the restart EIP past the 1-byte HLT, IRETD
    put(kData, {0x83, 0x44, 0x24, 0x04, 0x01,  // add dword [esp+4],1
                0x83, 0xC4, 0x04,              // add esp,4
                0xCF});                        // iretd
    enter_v86();
    ASSERT_TRUE(faults.empty()) << fault_desc();
    v86_code({0xF4,            // hlt -- must #GP, never actually halt
              0xB0, 0x5A,      // mov al,5Ah
              0xEB, 0xFE});    // jmp $
    cpu->step();               // HLT -> #GP -> monitor
    expect_fault(cpu80486::EXC_GP, 0, "HLT at CPL 3 in V86");
    EXPECT_FALSE(cpu->v86_mode()) << "handler runs in ordinary ring 0";
    cpu->step();               // add [esp+4],1
    cpu->step();               // add esp,4
    cpu->step();               // iretd
    EXPECT_TRUE(cpu->v86_mode());
    EXPECT_EQ(cpu->eip, kV86Off + 1u) << "restarted past the HLT";
    cpu->step();               // mov al,5Ah
    EXPECT_EQ(cpu->eax & 0xFFu, 0x5Au);
    EXPECT_FALSE(cpu->halted) << "ring-3 HLT must not set halted";
}

TEST_F(Cpu80486V86Test, CliIsPermittedAtIoplThree) {
    enter_pm32();
    set_gate(cpu80486::EXC_GP, gate_desc(kCode32, kData, 0x8E));
    put(kData, {0xF4});
    enter_v86(kIopl3 | uint32_t(cpu80486::FLAG_IF));
    ASSERT_TRUE(faults.empty()) << fault_desc();
    v86_code({0xFA});
    cpu->step();
    EXPECT_TRUE(faults.empty()) << fault_desc();
    EXPECT_FALSE(cpu->flag(cpu80486::FLAG_IF)) << "the 8086 program really masked interrupts";
}

TEST_F(Cpu80486V86Test, PushfIsIoplSensitiveInV86) {
    enter_pm32();
    set_gate(cpu80486::EXC_GP, gate_desc(kCode32, kData, 0x8E));
    put(kData, {0xF4});
    enter_v86();   // IOPL 0
    ASSERT_TRUE(faults.empty()) << fault_desc();
    v86_code({0x9C});   // pushf
    cpu->step();
    expect_fault(cpu80486::EXC_GP, 0, "PUSHF at IOPL 0 in V86");
}

TEST_F(Cpu80486V86Test, PopfIsIoplSensitiveInV86) {
    // same rule as PUSHF, a real fault rather than silent IOPL masking
    enter_pm32();
    set_gate(cpu80486::EXC_GP, gate_desc(kCode32, kData, 0x8E));
    put(kData, {0xF4});
    enter_v86();   // IOPL 0
    ASSERT_TRUE(faults.empty()) << fault_desc();
    v86_code({0x9D});   // popf
    cpu->step();
    expect_fault(cpu80486::EXC_GP, 0, "POPF at IOPL 0 in V86");
}

TEST_F(Cpu80486V86Test, PushfAndPopfAtIoplThreeRunAndCannotChangeIoplOrVm) {
    enter_pm32();
    set_gate(cpu80486::EXC_GP, gate_desc(kCode32, kData, 0x8E));
    put(kData, {0xF4});
    enter_v86(kIopl3);
    ASSERT_TRUE(faults.empty()) << fault_desc();
    v86_code({0x9C,          // pushf
              0x9D});        // popf -- pops back what pushf wrote
    cpu->step();
    EXPECT_TRUE(faults.empty()) << fault_desc();
    EXPECT_EQ(cpu->esp, kV86Sp - 2) << "a 16-bit push onto the 8086 stack";
    EXPECT_EQ(r16(v86_lin(kV86Sp - 2)), uint16_t(cpu->eflags & 0xFFFFu));
    // poke IOPL 0 into the popped image; neither IOPL nor VM (bit 17) may change
    w16(v86_lin(kV86Sp - 2), 0x0002);
    cpu->step();
    EXPECT_TRUE(faults.empty()) << fault_desc();
    EXPECT_EQ((cpu->eflags & uint32_t(cpu80486::FLAG_IOPL)) >> 12, 3u);
    EXPECT_TRUE(cpu->flag(cpu80486::FLAG_VM));
    EXPECT_EQ(cpu->esp, kV86Sp);
}

TEST_F(Cpu80486V86Test, PopfdAtIoplThreeCannotDropOutOfV86) {
    // 32-bit sibling with PUSHFD/POPFD (66 9C / 66 9D). Intel 80486 PRM, POPF/POPFD: VM and RF unaffected.
    // The 32-bit path builds its result from mask/keep, so VM must be in `keep`.
    enter_pm32();
    set_gate(cpu80486::EXC_GP, gate_desc(kCode32, kData, 0x8E));
    put(kData, {0xF4});
    enter_v86(kIopl3);
    ASSERT_TRUE(faults.empty()) << fault_desc();
    v86_code({0x66, 0x9C,          // pushfd
              0x66, 0x9D});        // popfd -- pops back what pushfd wrote
    cpu->step();
    EXPECT_TRUE(faults.empty()) << fault_desc();
    EXPECT_EQ(cpu->esp, kV86Sp - 4) << "a 32-bit push onto the 8086 stack";
    EXPECT_EQ(r32(v86_lin(kV86Sp - 4)), cpu->eflags);
    // poke IOPL 0 and VM 0 into the image; neither may change
    w32(v86_lin(kV86Sp - 4), 0x00000002u);
    cpu->step();
    EXPECT_TRUE(faults.empty()) << fault_desc();
    EXPECT_EQ((cpu->eflags & uint32_t(cpu80486::FLAG_IOPL)) >> 12, 3u);
    EXPECT_TRUE(cpu->flag(cpu80486::FLAG_VM))
        << "POPFD must not be able to drop the task out of virtual-8086 mode";
    EXPECT_TRUE(cpu->v86_mode());
    EXPECT_EQ(cpu->esp, kV86Sp);
}

TEST_F(Cpu80486V86Test, OutsideV86ARingThreePushfStillDoesNotFault) {
    // at CPL 3 with VM clear, POPF masks IOPL silently (Intel 80486 PRM, POPF), a different rule
    enter_pm32();
    put(kData, {0xF4});
    set_gate(cpu80486::EXC_GP, gate_desc(kCode32, kData, 0x8E));
    put(kData + 0x400, {0x9C,                                // pushfd
                        0x68, 0x02, 0x00, 0x00, 0x00,        // push 0002h -- IOPL 0
                        0x9D});                              // popfd
    pm_run({0x68, uint8_t(kStack3 | 3), 0x00, 0x00, 0x00,
            0x68, 0x00, 0x7C, 0x00, 0x00,
            0x68, 0x02, 0x32, 0x00, 0x00,                    // EFLAGS: IOPL 3, IF set
            0x68, uint8_t(kCode3), 0x00, 0x00, 0x00,
            0x68, 0x00, 0x64, 0x00, 0x00,
            0xCF}, 6);
    ASSERT_EQ(cpu->cpl(), 3);
    ASSERT_FALSE(cpu->v86_mode());
    for (int i = 0; i < 3; ++i) cpu->step();
    EXPECT_TRUE(faults.empty()) << fault_desc();
    EXPECT_EQ((cpu->eflags & uint32_t(cpu80486::FLAG_IOPL)) >> 12, 3u)
        << "the CPL-3 POPFD left IOPL alone instead of faulting";
}

TEST_F(Cpu80486V86Test, IntNIsIoplSensitiveInV86) {
    enter_pm32();
    set_gate(0x21, gate_desc(kCode32, kData, 0xEE));
    set_gate(cpu80486::EXC_GP, gate_desc(kCode32, kData + 0x100, 0x8E));
    put(kData, {0xF4});
    put(kData + 0x100, {0xF4});
    enter_v86();   // IOPL 0
    ASSERT_TRUE(faults.empty()) << fault_desc();
    v86_code({0xCD, 0x21});
    cpu->step();
    // the monitor gets #GP instead of the vector, to intercept 8086 OS calls
    expect_fault(cpu80486::EXC_GP, 0);
    EXPECT_EQ(cpu->eip, kData + 0x100) << "the #GP handler ran, not vector 21h's";
}

TEST_F(Cpu80486V86Test, IretInV86IsIoplSensitive) {
    enter_pm32();
    set_gate(cpu80486::EXC_GP, gate_desc(kCode32, kData, 0x8E));
    put(kData, {0xF4});
    enter_v86();   // IOPL 0
    ASSERT_TRUE(faults.empty()) << fault_desc();
    v86_code({0xCF});
    cpu->step();
    expect_fault(cpu80486::EXC_GP, 0, "IRET at IOPL 0 in V86");
}

TEST_F(Cpu80486V86Test, IretAtIoplThreeInV86IsThePlainEightyEightySixIret) {
    // pops IP, CS and FLAGS from the 8086 stack; VM and IOPL stay
    enter_pm32();
    set_gate(cpu80486::EXC_GP, gate_desc(kCode32, kData, 0x8E));
    put(kData, {0xF4});
    enter_v86(kIopl3);
    ASSERT_TRUE(faults.empty()) << fault_desc();
    w16(v86_lin(kV86Sp + 0), 0x0200);       // IP
    w16(v86_lin(kV86Sp + 2), kV86Seg);      // CS
    w16(v86_lin(kV86Sp + 4), 0x0003);       // FLAGS: CF set, IOPL 0 in the image
    v86_code({0xCF});
    cpu->step();
    EXPECT_TRUE(faults.empty()) << fault_desc();
    EXPECT_TRUE(cpu->flag(cpu80486::FLAG_VM)) << "an 8086 IRET stays in V86";
    EXPECT_EQ((cpu->eflags & uint32_t(cpu80486::FLAG_IOPL)) >> 12, 3u)
        << "and cannot lower IOPL out of its own flags image";
    EXPECT_TRUE(cpu->flag(cpu80486::FLAG_CF));
    EXPECT_EQ(cpu->cs, kV86Seg);
    EXPECT_EQ(cpu->eip, 0x0200u);
    EXPECT_EQ(cpu->esp, kV86Sp + 6);
    EXPECT_EQ(cpu->cpl(), 3);
}

TEST_F(Cpu80486V86Test, PortIoInV86AlwaysConsultsTheBitmapEvenAtIoplThree) {
    enter_pm32();
    set_gate(cpu80486::EXC_GP, gate_desc(kCode32, kData, 0x8E));
    put(kData, {0xF4});
    give_tss_io_bitmap();
    // in V86 IN/OUT ignore IOPL; only the map decides (PRM 15.5)
    enter_v86(kIopl3);
    ASSERT_TRUE(faults.empty()) << fault_desc();
    v86_code({0xE4, 0x60,    // in al,60h -- bit clear, granted
              0xE4, 0x70});  // in al,70h -- bit set, denied
    cpu->step();
    EXPECT_TRUE(faults.empty()) << fault_desc();
    cpu->step();
    expect_fault(cpu80486::EXC_GP, 0, "port 70h is denied by the map whatever IOPL says");
}

TEST_F(Cpu80486V86Test, PrivilegedInstructionsInV86TrapToTheMonitor) {
    enter_pm32();
    set_gate(cpu80486::EXC_GP, gate_desc(kCode32, kData, 0x8E));
    put(kData, {0xF4});
    enter_v86(kIopl3);
    ASSERT_TRUE(faults.empty()) << fault_desc();
    // CPL 3 makes ring-0-only instructions fault; LGDT is what a memory manager in V86 might try
    v86_code({0x0F, 0x01, 0x16, 0x00, 0x05});   // lgdt [0500h]
    cpu->step();
    expect_fault(cpu80486::EXC_GP, 0);
}

// --- IRETD and the VM bit outside ring 0 ---

TEST_F(Cpu80486V86Test, AnIretdOutsideRingZeroCannotSetVm) {
    enter_pm32();
    put(kData, {0xF4});
    set_gate(cpu80486::EXC_GP, gate_desc(kCode32, kData, 0x8E));
    // ring-3 IRETD with VM in its image: CPL must be 0 or VM is unchanged, not faulted (PRM 15.3)
    put(kData + 0x400, {0x68, 0x02, 0x00, 0x02, 0x00,        // push EFLAGS with VM (bit 17)
                        0x68, uint8_t(kCode3), 0x00, 0x00, 0x00,
                        0x68, 0x10, 0x64, 0x00, 0x00,        // push kData+0x410
                        0xCF});
    put(kData + 0x410, {0xB8, 0x77, 0x00, 0x00, 0x00});      // mov eax,77h
    pm_run({0x68, uint8_t(kStack3 | 3), 0x00, 0x00, 0x00,
            0x68, 0x00, 0x7C, 0x00, 0x00,
            0x68, 0x02, 0x02, 0x00, 0x00,
            0x68, uint8_t(kCode3), 0x00, 0x00, 0x00,
            0x68, 0x00, 0x64, 0x00, 0x00,
            0xCF}, 6);
    ASSERT_EQ(cpu->cpl(), 3);
    for (int i = 0; i < 4; ++i) cpu->step();   // three pushes and the IRETD
    EXPECT_TRUE(faults.empty()) << fault_desc();
    EXPECT_FALSE(cpu->flag(cpu80486::FLAG_VM)) << "VM was ignored, not loaded";
    EXPECT_EQ(cpu->cpl(), 3);
    cpu->step();
    EXPECT_EQ(cpu->eax, 0x77u) << "and execution continued in ordinary protected mode";
}

// --- no task-gate entry into V86 (cpu80486.h header) ---

TEST_F(Cpu80486V86Test, ATaskSwitchIntoAV86TaskIsRefusedRatherThanCorruptingState) {
    enter_pm32();
    for (uint32_t i = 0; i < 104; i += 4) w32(kTss2 + i, 0);
    w32(kTss2 + 32, kData);                                   // EIP
    w32(kTss2 + 36, 0x00000202u | uint32_t(cpu80486::FLAG_VM));  // EFLAGS with VM set
    w32(kTss2 + 56, 0x00007A00u);
    w32(kTss2 + 72, kV86Seg);   // 8086 segment values, not selectors
    w32(kTss2 + 76, kV86Seg);
    w32(kTss2 + 80, kV86Seg);
    w32(kTss2 + 84, kV86Seg);
    pm_run({0x66, 0xB8, uint8_t(kTssSel), 0x00, 0x0F, 0x00, 0xD8}, 2);   // ltr
    ASSERT_TRUE(faults.empty()) << fault_desc();
    put(pm_code_ + 7, {0xEA, 0x00, 0x00, 0x00, 0x00, uint8_t(kTss2Sel), 0x00});
    cpu->eip = pm_code_ + 7;
    cpu->step();
    // reported as an invalid TSS with nothing committed
    expect_fault(cpu80486::EXC_TS, kTss2Sel & 0xFFFCu);
    EXPECT_EQ(cpu->tr_selector(), kTssSel) << "the switch did not happen";
    EXPECT_FALSE(cpu->flag(cpu80486::FLAG_VM));
    EXPECT_EQ(cpu->cpl(), 0);
    EXPECT_EQ(cpu->cs, kCode32);
}

// --- the whole cycle, composed -------------------------------------------

// An 8086 program at IOPL 0 runs CLI, a denied IN, and an INT; one #GP handler skips each and IRETDs back.
// Each faulting instruction is two bytes (CLI gets a CS: prefix) so the handler needs no length decoding.
TEST_F(Cpu80486V86Test, AMonitorEmulatesThreeTrappedInstructionsAndTheTaskRunsOn) {
    enter_pm32();
    give_tss_io_bitmap();
    set_gate(cpu80486::EXC_GP, gate_desc(kCode32, kData, 0x8E));
    // handler at ring 0 with data segments nulled: touches only SS and EBX (counts traps); #GP error code puts EIP at [esp+4]
    put(kData, {0x43,                          // inc ebx
                0x83, 0x44, 0x24, 0x04, 0x02,  // add dword [esp+4],2
                0x83, 0xC4, 0x04,              // add esp,4
                0xCF});                        // iretd -- back into V86
    enter_v86();
    ASSERT_TRUE(faults.empty()) << fault_desc();
    cpu->ebx = 0;
    v86_code({0x2E, 0xFA,     // cs: cli   -- IOPL-sensitive
              0xE4, 0x70,     // in al,70h -- denied by the I/O permission map
              0xCD, 0x21,     // int 21h   -- IOPL-sensitive
              0xB0, 0x5A,     // mov al,5Ah
              0xEB, 0xFE});   // jmp $
    for (int i = 0; i < 20; ++i) cpu->step();
    EXPECT_EQ(cpu->ebx, 3u) << "three traps, three emulated instructions";
    ASSERT_EQ(faults.size(), 3u);
    for (const FaultRec &f : faults) {
        EXPECT_EQ(f.vector, cpu80486::EXC_GP);
        EXPECT_EQ(f.error, 0u);
        EXPECT_EQ(f.cs, kV86Seg) << "every trap came out of the 8086 task";
    }
    EXPECT_TRUE(cpu->v86_mode()) << "and each IRETD put the task back in V86";
    EXPECT_EQ(cpu->cpl(), 3);
    EXPECT_EQ(cpu->eax & 0xFFu, 0x5Au) << "the 8086 program ran past all three";
    EXPECT_EQ(cpu->eip, kV86Off + 8) << "parked on its own jmp $";
    EXPECT_EQ(cpu->esp, kV86Sp) << "the 8086 stack was never touched -- the frames went to ring 0";
    EXPECT_EQ(cpu->ds, kV86Seg) << "and the IRETD restored the 8086 segment registers";
    EXPECT_EQ(cpu->es, kV86Seg);
}

// --- alignment check (Intel 80486 PRM) ---
// V86 runs at CPL 3, where a DOS program meets #AC.

class Cpu80486AlignTest : public Cpu80486V86Test {
protected:
    void enter_v86_aligned(bool am, bool ac) {
        enter_pm32();
        if (am)
            pm_run({0x0F, 0x20, 0xC0,                     // mov eax,cr0
                    0x0D, 0x00, 0x00, 0x04, 0x00,         // or eax,40000h (AM)
                    0x0F, 0x22, 0xC0}, 3);                // mov cr0,eax
        enter_v86(ac ? uint32_t(cpu80486::FLAG_AC) : 0u);
        ASSERT_TRUE(faults.empty()) << fault_desc();
    }
};

TEST_F(Cpu80486AlignTest, AMisalignedWordAtCplThreeRaisesAlignmentCheck) {
    enter_v86_aligned(true, true);
    v86_code({0xA1, 0x00, 0x50,     // mov ax,[5000h] -- aligned
              0xA1, 0x01, 0x50});   // mov ax,[5001h]
    cpu->step();
    EXPECT_TRUE(faults.empty()) << fault_desc();
    cpu->step();
    expect_fault(17, 0, "a word at an odd address");
}

TEST_F(Cpu80486AlignTest, EitherEnableBitClearLeavesMisalignedAccessesAlone) {
    enter_v86_aligned(false, true);
    v86_code({0xA1, 0x01, 0x50});
    cpu->step();
    EXPECT_TRUE(faults.empty()) << "CR0.AM clear: " << fault_desc();
}

TEST_F(Cpu80486AlignTest, EflagsAcClearLeavesMisalignedAccessesAlone) {
    enter_v86_aligned(true, false);
    v86_code({0xA1, 0x01, 0x50});
    cpu->step();
    EXPECT_TRUE(faults.empty()) << "EFLAGS.AC clear: " << fault_desc();
}

TEST_F(Cpu80486AlignTest, RingZeroIsNeverAlignmentChecked) {
    enter_pm32();
    pm_run({0x0F, 0x20, 0xC0, 0x0D, 0x00, 0x00, 0x04, 0x00, 0x0F, 0x22, 0xC0}, 3);
    cpu->set_flag(cpu80486::FLAG_AC, true);
    pm_run({0x8B, 0x05, 0x01, 0x60, 0x00, 0x00});   // mov eax,[6001h]
    EXPECT_TRUE(faults.empty()) << fault_desc();
}

TEST_F(Cpu80486AlignTest, FsaveChecksItsWholeAreaNotEachRegisterSlot) {
    // a 16-bit FSAVE area needs only word alignment
    enter_v86_aligned(true, true);
    v86_code({0xDD, 0x36, 0x02, 0x50,    // fnsave [5002h]
              0xDD, 0x36, 0x01, 0x51});  // fnsave [5101h]
    cpu->step();
    EXPECT_TRUE(faults.empty()) << fault_desc();
    cpu->step();
    expect_fault(17, 0, "an FSAVE area at an odd address");
}

TEST_F(Cpu80486AlignTest, SgdtNeedsADwordAlignedImage) {
    enter_v86_aligned(true, true);
    v86_code({0x0F, 0x01, 0x06, 0x04, 0x50,    // sgdt [5004h]
              0x0F, 0x01, 0x06, 0x02, 0x51});  // sgdt [5102h]
    cpu->step();
    EXPECT_TRUE(faults.empty()) << fault_desc();
    cpu->step();
    expect_fault(17, 0, "a GDTR image on a word boundary");
}


// --- on-die x87 FPU ---
// Tested through the real-mode fixture; DOS code uses the FPU freely. Results are exactly representable, and the 80-bit round trips use a payload NaN and a denormal that only survive in the architectural format.
// Reference: Intel 80486 PRM floating-point chapters.

namespace {
// ModR/M for ESC mod=00 rm=110 disp16, an absolute address
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

    // FLD m64 / FSTP m64
    static std::initializer_list<uint8_t> fld_m64(uint16_t at);
    bool c0() const { return (cpu->fpu_status() & (1u << 8)) != 0; }
    bool c1() const { return (cpu->fpu_status() & (1u << 9)) != 0; }
    bool c2() const { return (cpu->fpu_status() & (1u << 10)) != 0; }
    bool c3() const { return (cpu->fpu_status() & (1u << 14)) != 0; }
    // tag of ST(i): 3 means empty
    int tag_of(int i) const {
        int phys = (cpu->fpu_top() + i) & 7;
        return (cpu->fpu_tag() >> (phys * 2)) & 3;
    }
};

TEST_F(Cpu80486FpuTest, FninitLeavesTheDocumentedResetState) {
    run({0xDB, 0xE3});   // FNINIT
    // control word 037Fh: all masks set, extended precision, round to nearest (Intel 80486 PRM, FPU Initialization)
    EXPECT_EQ(cpu->fpu_control(), 0x037Fu);
    EXPECT_EQ(cpu->fpu_status(), 0x0000u);
    EXPECT_EQ(cpu->fpu_tag(), 0xFFFFu) << "every register tagged empty";
    EXPECT_EQ(cpu->fpu_top(), 0);
    EXPECT_EQ(unimpl_count, 0);
}

TEST_F(Cpu80486FpuTest, FxchWithAnEmptyRegisterIsAMaskedStackUnderflow) {
    // Intel 80486 PRM, FXCH: an empty operand is stack underflow; masked, it reads as indefinite and the exchange happens
    runN({0xDB, 0xE3,      // FNINIT
          0xD9, 0xE8,      // FLD1
          0xD9, 0xC9}, 3); // FXCH ST(1)
    EXPECT_EQ(cpu->st_value(1), 1.0L);
    EXPECT_EQ(cpu->st(0).sign_exp, 0xFFFFu);
    EXPECT_EQ(cpu->st(0).significand, 0xC000000000000000ull);
    EXPECT_EQ(tag_of(0), 2) << "the indefinite is a NaN: special";
    EXPECT_EQ(tag_of(1), 0);
    EXPECT_EQ(cpu->fpu_status() & 0x41u, 0x41u) << "IE and SF";
    EXPECT_FALSE(c1()) << "C1 clear: underflow, not overflow";
}

TEST_F(Cpu80486FpuTest, FxchWithAnEmptyRegisterUnmaskedLeavesBothAlone) {
    poke16(kA, 0x037E);    // invalid operation unmasked
    runN({0xDB, 0xE3,                  // FNINIT
          0xD9, 0x2E, 0x00, 0x02,      // FLDCW [0200h]
          0xD9, 0xE8,                  // FLD1
          0xD9, 0xC9}, 4);             // FXCH ST(1)
    EXPECT_EQ(cpu->st_value(0), 1.0L);
    EXPECT_EQ(tag_of(1), 3) << "still empty";
    EXPECT_NE(cpu->fpu_status() & 0x80u, 0u) << "ES: the exception is pending";
}

TEST_F(Cpu80486FpuTest, TheStoredTagWordClassifiesEachRegister) {
    // Intel 80486 PRM, Tag Word: 00 valid, 01 zero, 10 special, 11 empty
    poke_double(kA, 1.0);
    poke_double(kB, 0.0);
    runN({0xDB, 0xE3,                          // FNINIT
          0xD9, 0xE8,                          // FLD1        -> ST2 at the end
          0xD9, 0xEE,                          // FLDZ        -> ST1
          0xDD, esc_mem(0), 0x00, 0x02,        // FLD 1.0
          0xDC, esc_mem(6), 0x00, 0x03,        // FDIV 0.0    -> ST0 = +inf
          0xD9, esc_mem(6), 0x00, 0x04}, 6);   // FNSTENV [0400h]
    // TOP is 5: ST0 is physical 5; 0-4 are empty
    EXPECT_EQ(memw(kC + 4), 0x1BFFu) << "7 valid, 6 zero, 5 special, the rest empty";
    EXPECT_EQ(cpu->fpu_tag(), 0x1BFFu);
}

TEST_F(Cpu80486FpuTest, FldenvKeepsOnlyEmptinessAndRecomputesTheRest) {
    runN({0xDB, 0xE3,                          // FNINIT
          0xD9, 0xE8,                          // FLD1: physical 7 valid
          0xD9, esc_mem(6), 0x00, 0x02}, 3);   // FNSTENV [0200h]
    ASSERT_EQ(memw(kA + 4), 0x3FFFu);
    poke16(kA + 4, 0x7FFF);                    // claim physical 7 is a zero
    runN({0xD9, esc_mem(4), 0x00, 0x02,        // FLDENV [0200h]
          0xD9, esc_mem(6), 0x00, 0x03}, 2);   // FNSTENV [0300h]
    EXPECT_EQ(memw(kB + 4), 0x3FFFu) << "recomputed from the 1.0 it holds";
    poke16(kA + 4, 0xFFFF);                    // now claim it's empty
    runN({0xD9, esc_mem(4), 0x00, 0x02}, 1);
    EXPECT_EQ(tag_of(0), 3) << "emptiness is taken as loaded";
}

TEST_F(Cpu80486FpuTest, FxchSwapsTwoValidRegisters) {
    runN({0xDB, 0xE3, 0xD9, 0xE8, 0xD9, 0xEE, 0xD9, 0xC9}, 4);  // FNINIT, FLD1, FLDZ, FXCH ST(1)
    EXPECT_EQ(cpu->st_value(0), 1.0L);
    EXPECT_EQ(cpu->st_value(1), 0.0L);
    EXPECT_EQ(cpu->fpu_status() & 0x41u, 0u);
}

TEST_F(Cpu80486FpuTest, TheEightyBitRegisterFileHoldsTheArchitecturalFormat) {
    // a payload NaN a host double could not carry
    poke80(kA, 0xC123456789ABCDEFull, 0x7FFF);
    runN({0xDB, 0xE3,                          // FNINIT
          0xDB, esc_mem(5), 0x00, 0x02,        // FLD  tbyte [0200h]
          0xDB, esc_mem(7), 0x00, 0x03}, 3);   // FSTP tbyte [0300h]
    EXPECT_EQ(mem64(kB), 0xC123456789ABCDEFull);
    EXPECT_EQ(memw(kB + 8), 0x7FFFu);
    EXPECT_EQ(unimpl_count, 0);
}

TEST_F(Cpu80486FpuTest, AnEightyBitDenormalRoundTripsExactly) {
    // exponent 0 with non-zero significand is a denormal with integer bit clear
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

    // FSUB and FDIV including reversed forms, exact values
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
    // x87 encoding quirk: with ST(i) as destination, DC E0+i is FSUBR and DC E8+i is FSUB, opposite the D8 forms
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
    // a C (int) cast sets RC to truncate, does FISTP, restores RC
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
    // 2.5 separates nearest-even from round-half-up
    poke_double(kA, 2.5);
    EXPECT_EQ(store_with_rc(0x0000), 2) << "2.5 rounds to 2, not 3: ties go to even";
    EXPECT_EQ(store_with_rc(0x0800), 3);
}

TEST_F(Cpu80486FpuTest, PrecisionControlNarrowsTheResult) {
    // PC=single rounds results to 24 bits
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
    // double compare is DC /2 (D8 /2 is single). ST(0) greater: C3 C2 C0 clear.
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
    // FCOMP pops, FCOM does not
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

TEST_F(Cpu80486FpuTest, FucomAndFucompTakeARegisterOperand) {
    poke80(kA, 0xC000000000000000ull, 0x7FFF);   // a quiet NaN
    poke_double(kB, 1.0);
    runN({0xDB, 0xE3,
          0xDD, esc_mem(0), 0x00, 0x03,        // ST0 = 1.0
          0xDB, esc_mem(5), 0x00, 0x02,        // ST0 = NaN, ST1 = 1.0
          0xDD, 0xE1}, 4);                     // FUCOM ST(1)
    EXPECT_TRUE(c3()); EXPECT_TRUE(c2()); EXPECT_TRUE(c0());
    EXPECT_EQ(cpu->fpu_status() & 0x0001u, 0u) << "unordered, and FUCOM does not signal";
    EXPECT_EQ(cpu->fpu_top(), 6);
    runN({0xDB, 0xE3,
          0xD9, 0xE8,                          // FLD1
          0xD9, 0xEE,                          // FLDZ: ST0 = 0, ST1 = 1
          0xDD, 0xE9}, 4);                     // FUCOMP ST(1)
    EXPECT_TRUE(c0()) << "0 < 1";
    EXPECT_EQ(cpu->fpu_top(), 7) << "FUCOMP pops once";
}

TEST_F(Cpu80486FpuTest, UndocumentedRegisterAliasesBehaveLikeTheirDocumentedForms) {
    // each sequence starts FNINIT / FLDZ / FLD1: ST0 = 1, ST1 = 0
    runN({0xDB, 0xE3, 0xD9, 0xEE, 0xD9, 0xE8, 0xDC, 0xD1}, 4);   // DC D1: FCOM ST(1)
    EXPECT_FALSE(c0()); EXPECT_FALSE(c3()) << "1 > 0, and nothing was divided";
    EXPECT_EQ(cpu->fpu_top(), 6);
    runN({0xDB, 0xE3, 0xD9, 0xEE, 0xD9, 0xE8, 0xDC, 0xD9}, 4);   // DC D9: FCOMP ST(1)
    EXPECT_EQ(cpu->fpu_top(), 7);
    runN({0xDB, 0xE3, 0xD9, 0xEE, 0xD9, 0xE8, 0xDE, 0xD1}, 4);   // DE D1: FCOMP ST(1)
    EXPECT_EQ(cpu->fpu_top(), 7);
    EXPECT_FALSE(c0());

    for (uint8_t xch : {uint8_t(0xDD), uint8_t(0xDF)}) {         // DD C9 / DF C9: FXCH ST(1)
        runN({0xDB, 0xE3, 0xD9, 0xEE, 0xD9, 0xE8, xch, 0xC9,
              0xDD, esc_mem(3), 0x00, 0x03}, 5);                  // FSTP qword [0300h]
        EXPECT_EQ(mem_double(kB), 0.0) << "ST0 and ST1 swapped";
    }
    for (uint8_t st : {uint8_t(0xD1), uint8_t(0xD9)}) {          // DF D1 / DF D9: FSTP ST(1)
        runN({0xDB, 0xE3, 0xD9, 0xEE, 0xD9, 0xE8, 0xDF, st,
              0xDD, esc_mem(3), 0x00, 0x03}, 5);
        EXPECT_EQ(mem_double(kB), 1.0) << "ST0 stored over ST1, then popped";
        EXPECT_EQ(cpu->fpu_top(), 0);
    }

    runN({0xDB, 0xE3, 0xD9, 0xEE, 0xD9, 0xE8, 0xDF, 0xC1}, 4);   // DF C1: FFREEP ST(1)
    EXPECT_EQ(cpu->fpu_top(), 7);
    EXPECT_EQ(tag_of(0), 3) << "the old ST(1) was freed, and is ST(0) after the pop";
    EXPECT_EQ(cpu->fpu_status() & 0x0041u, 0u) << "no stack fault";

    runN({0xDB, 0xE3, 0xD9, 0xD9}, 2);                           // D9 D9 on an empty stack
    EXPECT_EQ(cpu->fpu_status() & 0x0041u, 0u)
        << "the D9 D8+i FSTP alias skips the stack-underflow check";
    EXPECT_EQ(cpu->fpu_top(), 1);
    EXPECT_EQ(unimpl_count, 0);
}

TEST_F(Cpu80486FpuTest, ReservedEscEncodingsRaiseInvalidOpcode) {
    poke16(0x0006 * 4 + 0, 0x0400);
    poke16(0x0006 * 4 + 2, 0x0000);
    mem[0x0400] = 0xF4;
    int expected = 0;
    for (std::initializer_list<uint8_t> code : {
             std::initializer_list<uint8_t>{0xDA, 0xC1},              // FCMOVB, a P6 instruction
             std::initializer_list<uint8_t>{0xDB, 0xC1},              // FCMOVNB
             std::initializer_list<uint8_t>{0xDB, 0xE9},              // FUCOMI
             std::initializer_list<uint8_t>{0xDF, 0xE9},              // FUCOMIP
             std::initializer_list<uint8_t>{0xDB, esc_mem(1), 0, 2},  // FISTTP m32, SSE3
             std::initializer_list<uint8_t>{0xDF, esc_mem(1), 0, 2},  // FISTTP m16
             std::initializer_list<uint8_t>{0xDD, esc_mem(1), 0, 2},  // FISTTP m64
             std::initializer_list<uint8_t>{0xD9, esc_mem(1), 0, 2},
             std::initializer_list<uint8_t>{0xDD, esc_mem(5), 0, 2},
             std::initializer_list<uint8_t>{0xDB, esc_mem(4), 0, 2},
             std::initializer_list<uint8_t>{0xD9, 0xD1},
             std::initializer_list<uint8_t>{0xD9, 0xE2},
             std::initializer_list<uint8_t>{0xDB, 0xE5},
             std::initializer_list<uint8_t>{0xDD, 0xF0},
             std::initializer_list<uint8_t>{0xDE, 0xD8},
             std::initializer_list<uint8_t>{0xDF, 0xE1},
         }) {
        cpu->halted = false;
        cpu->ss = 0;
        cpu->esp = 0x2000;
        uint16_t cw = 0x1234;
        poke16(kA, cw);
        run(code);
        ++expected;
        EXPECT_EQ(cpu->eip, 0x0400u) << "ESC " << int(*code.begin()) << " should vector through IVT[6]";
        EXPECT_EQ(memw(kA), cw) << "a reserved memory form must not store anything";
    }
    EXPECT_EQ(unimpl_count, expected);
    EXPECT_EQ(unimpl_opcode, 0xDFE1);
}

TEST_F(Cpu80486FpuTest, FtstComparesAgainstZeroAndFxamClassifies) {
    poke_double(kA, -3.0);
    runN({0xDB, 0xE3, 0xDD, esc_mem(0), 0x00, 0x02, 0xD9, 0xE4}, 3);   // FTST
    EXPECT_TRUE(c0()) << "-3.0 is less than zero";
    // FXAM reports class in C3/C2/C0 and sign in C1
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
    // masked (reset default): infinity, not a trap
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
    // sign-bit operations, so they work on NaNs
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
    // ninth push sets IE and SF with C1=1 for overflow (Intel 80486 PRM, Stack Fault)
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
    // FFREE empties a register without moving TOP
    int top_before = cpu->fpu_top();
    put_and_run({0xDD, 0xC1});   // FFREE ST(1)
    EXPECT_EQ(cpu->fpu_top(), top_before);
    EXPECT_EQ(tag_of(1), 3);
    EXPECT_EQ(tag_of(0), 0) << "ST(0) is untouched";
    // FINCSTP / FDECSTP move TOP, tags untouched
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
    // status word TOP is bits 11-13
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
    EXPECT_EQ(tag_of(0), 2) << "but the register stack is left alone (infinity tags special) -- that is FNINIT's job";
}

TEST_F(Cpu80486FpuTest, FsaveAndFrstorRoundTripTheWholeStackAndEnvironment) {
    poke_double(kA, 7.25);
    runN({0xDB, 0xE3,
          0xDD, esc_mem(0), 0x00, 0x02,        // ST0 = 7.25
          0xD9, 0xE8,                          // ST0 = 1.0, ST1 = 7.25
          0x66, 0xDD, esc_mem(6), 0x00, 0x05}, 4);   // FNSAVE [0500h], 32-bit environment
    // FSAVE leaves the FPU reset, so the next task starts clean
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
    // FBLD/FBSTP: 18 packed decimal digits plus a sign byte
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
    // FYL2X: ST(1) * log2(ST(0)), popping. 3.0 * log2(8.0) = 9.0
    poke_double(kA, 3.0);
    poke_double(kB, 8.0);
    runN({0xDB, 0xE3,
          0xDD, esc_mem(0), 0x00, 0x02,        // ST0 = 3.0
          0xDD, esc_mem(0), 0x00, 0x03,        // ST0 = 8.0, ST1 = 3.0
          0xD9, 0xF1,                          // FYL2X
          0xDD, esc_mem(3), 0x00, 0x04}, 5);
    EXPECT_DOUBLE_EQ(mem_double(kC), 9.0);
    // FPATAN(1,1) is pi/4
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
    // FPREM1 is the IEEE remainder: 10 rem 3 gives 1; for 5 and 3 it gives -1 where FPREM gives 2
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
    // CR0.EM sends every ESC to #NM; CR0.TS does so for the first FPU instruction after a task switch
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
    // CLTS clears TS
    cpu->halted = false;
    runN({0x0F, 0x06, 0xD9, 0xE8}, 2);   // CLTS / FLD1
    EXPECT_EQ(double(cpu->st_value(0)), 1.0);
    EXPECT_EQ(unimpl_count, 0);
}

TEST_F(Cpu80486FpuTest, WaitFaultsOnlyWhenMonitorCoprocessorAndTaskSwitchedAreBothSet) {
    poke16(0x0007 * 4 + 0, 0x0500);
    poke16(0x0007 * 4 + 2, 0x0000);
    mem[0x0500] = 0xF4;
    // TS alone leaves WAIT alone (MP)
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
    // the 486 defers the report to the next waiting FPU instruction, so a handler uses no-wait forms (FNSTSW, FNCLEX)
    poke16(0x0010 * 4 + 0, 0x0500);   // IVT[16] -> #MF
    poke16(0x0010 * 4 + 2, 0x0000);
    mem[0x0500] = 0xF4;
    // NE=1 selects the native #MF report over FERR#/IRQ13
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
    // a no-wait form does not check
    put_and_run({0xDF, 0xE0});        // FNSTSW AX
    EXPECT_NE(cpu->eip, 0x0500u) << "FNSTSW is a no-wait form";
    // the next waiting FPU instruction reports
    put_and_run({0xD9, 0xE8});        // FLD1
    EXPECT_EQ(cpu->eip, 0x0500u) << "and now #MF is delivered";
}

}  // namespace
