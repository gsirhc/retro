// Per-opcode cycle costs for cpu80286.cpp. Reference counts are the 80286 column
// of the Art of Assembly (Hyde) Appendix D table, which reproduces Intel's iAPX 286 PRM.

#include <gtest/gtest.h>

#include "cpu80286.h"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <vector>

namespace {

using cpu80286::Cpu;
using cpu80286::Bus;
using cpu80286::FLAG_OF;
using cpu80286::FLAG_ZF;

class Cpu80286TimingTest : public ::testing::Test {
protected:
    std::array<uint8_t, 0x100000> mem{};
    std::unique_ptr<Cpu> cpu;

    void SetUp() override {
        Bus bus;
        bus.read   = [this](uint32_t a) { return mem[a & 0xFFFFF]; };
        bus.write  = [this](uint32_t a, uint8_t v) { mem[a & 0xFFFFF] = v; };
        bus.in     = [](uint16_t) -> uint8_t { return 0xFF; };
        bus.out    = [](uint16_t, uint8_t) {};
        bus.in16   = [](uint16_t) -> uint16_t { return 0xFFFF; };
        bus.out16  = [](uint16_t, uint16_t) {};
        cpu = std::make_unique<Cpu>(bus);
        cpu->reset();
        cpu->cs = 0;
        cpu->ip = 0;
    }

    void load(std::initializer_list<uint8_t> code, uint16_t at = 0) {
        uint16_t addr = at;
        for (uint8_t b : code) mem[addr++] = b;
    }
    // Assembles `code` at CS:0, executes one instruction, returns step()'s cycles.
    int runCycles(std::initializer_list<uint8_t> code) {
        load(code);
        cpu->ip = 0;
        return cpu->step();
    }
};

// --- MUL/IMUL/DIV/IDIV ---

TEST_F(Cpu80286TimingTest, MulRegister8BitCosts13Cycles) {
    // F6 /4: MUL AL (mod=11, reg=100, rm=000 -> F6 E0)
    EXPECT_EQ(runCycles({0xF6, 0xE0}), 13);
}
TEST_F(Cpu80286TimingTest, MulRegister16BitCosts21Cycles) {
    // F7 /4: MUL AX
    EXPECT_EQ(runCycles({0xF7, 0xE0}), 21);
}
TEST_F(Cpu80286TimingTest, MulMemory8BitCosts16Cycles) {
    // F6 /4, [BX]: MUL byte [BX] (mod=00, reg=100, rm=111 -> F6 27)
    cpu->bx = 0x50;
    EXPECT_EQ(runCycles({0xF6, 0x27}), 16);
}
TEST_F(Cpu80286TimingTest, MulMemory16BitCosts24Cycles) {
    cpu->bx = 0x50;
    EXPECT_EQ(runCycles({0xF7, 0x27}), 24);
}
TEST_F(Cpu80286TimingTest, ImulSingleOperandRegisterCosts21Cycles) {
    // F7 /5: IMUL AX -- same cost class as MUL on real 80286
    EXPECT_EQ(runCycles({0xF7, 0xE8}), 21);
}
TEST_F(Cpu80286TimingTest, DivRegister8BitCosts14Cycles) {
    cpu->ax = 0x0004;
    cpu->bx = 0x0002;  // BL = divisor, must be non-zero to avoid the #DE fault path
    EXPECT_EQ(runCycles({0xF6, 0xF3}), 14);  // F6 /6, mod=11,rm=011: DIV BL
}
TEST_F(Cpu80286TimingTest, DivRegister16BitCosts22Cycles) {
    cpu->ax = 4; cpu->dx = 0;
    cpu->bx = 0x0002;  // divisor in BX, non-zero
    EXPECT_EQ(runCycles({0xF7, 0xF3}), 22);  // DIV BX
}
TEST_F(Cpu80286TimingTest, IdivRegister16BitCosts25Cycles) {
    cpu->ax = 4; cpu->dx = 0;
    cpu->bx = 2;
    EXPECT_EQ(runCycles({0xF7, 0xFB}), 25);  // IDIV BX
}
TEST_F(Cpu80286TimingTest, TestAndNotAndNegKeepTheGenericRegMemSplit) {
    EXPECT_EQ(runCycles({0xF6, 0xD0}), 2);         // F6 /2: NOT AL
    EXPECT_EQ(runCycles({0xF6, 0xD8}), 2);         // F6 /3: NEG AL
    EXPECT_EQ(runCycles({0xF6, 0xC0, 0x00}), 3);   // F6 /0, imm8: TEST AL,0
}

// --- grp1_immed: reg 3, mem 7 ---

TEST_F(Cpu80286TimingTest, Grp1ImmedSignExtended8BitRegisterCosts3Cycles) {
    EXPECT_EQ(runCycles({0x83, 0xF8, 0x00}), 3);  // 83 /7, mod=11,rm=000: CMP AX,0
}
TEST_F(Cpu80286TimingTest, Grp1ImmedSignExtended8BitMemoryCosts7Cycles) {
    EXPECT_EQ(runCycles({0x83, 0x07, 0x00}), 7);  // 83 /0, mod=00,rm=111: ADD [BX],0
}
TEST_F(Cpu80286TimingTest, CmpMemoryImmediateCosts6Cycles) {
    EXPECT_EQ(runCycles({0x83, 0x3F, 0x00}), 6);  // CMP [BX],0
    EXPECT_EQ(runCycles({0x80, 0x3F, 0x00}), 6);  // CMP byte [BX],0
}
TEST_F(Cpu80286TimingTest, Grp1Immed8BitRegisterCosts3Cycles) {
    EXPECT_EQ(runCycles({0x80, 0xC0, 0x01}), 3);  // 80 /0, mod=11,rm=000: ADD AL,1
}
TEST_F(Cpu80286TimingTest, Grp1Immed8BitMemoryCosts7Cycles) {
    EXPECT_EQ(runCycles({0x80, 0x07, 0x01}), 7);  // 80 /0, mod=00,rm=111: ADD byte [BX],1
}

TEST_F(Cpu80286TimingTest, PushReg16Costs3Cycles) {
    EXPECT_EQ(runCycles({0x50}), 3);  // PUSH AX
}

// --- The rest of the 286 column (HelpPC 2.10) ---

TEST_F(Cpu80286TimingTest, NopAndXchgAccumulatorCost3Cycles) {
    EXPECT_EQ(runCycles({0x90}), 3);
    EXPECT_EQ(runCycles({0x93}), 3);  // XCHG AX,BX
}
TEST_F(Cpu80286TimingTest, XchgCosts3RegisterAnd5Memory) {
    EXPECT_EQ(runCycles({0x87, 0xD8}), 3);  // XCHG BX,AX
    EXPECT_EQ(runCycles({0x87, 0x07}), 5);  // XCHG [BX],AX
    EXPECT_EQ(runCycles({0x86, 0x07}), 5);  // XCHG [BX],AL
}
TEST_F(Cpu80286TimingTest, LeaCosts3Cycles) {
    EXPECT_EQ(runCycles({0x8D, 0x07}), 3);  // LEA AX,[BX]
}
TEST_F(Cpu80286TimingTest, AccumulatorImmediateCosts3Cycles) {
    EXPECT_EQ(runCycles({0x04, 0x01}), 3);        // ADD AL,1
    EXPECT_EQ(runCycles({0x3D, 0x01, 0x00}), 3);  // CMP AX,1
    EXPECT_EQ(runCycles({0xA8, 0x01}), 3);        // TEST AL,1
    EXPECT_EQ(runCycles({0xA9, 0x01, 0x00}), 3);  // TEST AX,1
}
TEST_F(Cpu80286TimingTest, TestRegisterMemoryCosts6Cycles) {
    EXPECT_EQ(runCycles({0x85, 0xC3}), 2);  // TEST BX,AX
    EXPECT_EQ(runCycles({0x85, 0x07}), 6);  // TEST [BX],AX
    EXPECT_EQ(runCycles({0x84, 0x07}), 6);  // TEST [BX],AL
}
TEST_F(Cpu80286TimingTest, CmpRegisterMemoryCosts6AndMemoryRegister7) {
    EXPECT_EQ(runCycles({0x3B, 0x07}), 6);  // CMP AX,[BX]
    EXPECT_EQ(runCycles({0x3A, 0x07}), 6);  // CMP AL,[BX]
    EXPECT_EQ(runCycles({0x39, 0x07}), 7);  // CMP [BX],AX
    EXPECT_EQ(runCycles({0x03, 0x07}), 7);  // ADD AX,[BX]
}
TEST_F(Cpu80286TimingTest, PushImmediateAndSegmentCost3Cycles) {
    cpu->sp = 0x200;
    EXPECT_EQ(runCycles({0x68, 0x34, 0x12}), 3);
    EXPECT_EQ(runCycles({0x6A, 0x01}), 3);
    EXPECT_EQ(runCycles({0x06}), 3);  // PUSH ES
    EXPECT_EQ(runCycles({0x1E}), 3);  // PUSH DS
}
TEST_F(Cpu80286TimingTest, PopCosts5Cycles) {
    cpu->sp = 0x100;
    EXPECT_EQ(runCycles({0x58}), 5);        // POP AX
    EXPECT_EQ(runCycles({0x07}), 5);        // POP ES
    EXPECT_EQ(runCycles({0x8F, 0x07}), 5);  // POP [BX]
}
TEST_F(Cpu80286TimingTest, PushMemoryCosts5Cycles) {
    cpu->sp = 0x200;
    EXPECT_EQ(runCycles({0xFF, 0x37}), 5);  // PUSH [BX]
}
TEST_F(Cpu80286TimingTest, BcdAdjustsCost3Cycles) {
    EXPECT_EQ(runCycles({0x27}), 3);  // DAA
    EXPECT_EQ(runCycles({0x2F}), 3);  // DAS
    EXPECT_EQ(runCycles({0x37}), 3);  // AAA
    EXPECT_EQ(runCycles({0x3F}), 3);  // AAS
}
TEST_F(Cpu80286TimingTest, XlatCosts5Cycles) {
    EXPECT_EQ(runCycles({0xD7}), 5);
}
TEST_F(Cpu80286TimingTest, BoundInRangeCosts13Cycles) {
    cpu->bx = 0x50;
    mem[0x50] = 0x00; mem[0x51] = 0x00; mem[0x52] = 0x10; mem[0x53] = 0x00;
    cpu->ax = 5;
    EXPECT_EQ(runCycles({0x62, 0x07}), 13);  // BOUND AX,[BX]
}
TEST_F(Cpu80286TimingTest, ImulImmediateCosts21RegisterAnd24Memory) {
    EXPECT_EQ(runCycles({0x6B, 0xC3, 0x03}), 21);        // IMUL AX,BX,3
    EXPECT_EQ(runCycles({0x69, 0xC3, 0x03, 0x00}), 21);  // IMUL AX,BX,3 (imm16)
    EXPECT_EQ(runCycles({0x6B, 0x07, 0x03}), 24);        // IMUL AX,[BX],3
}
TEST_F(Cpu80286TimingTest, IncDecThroughGroupCost2RegisterAnd7Memory) {
    EXPECT_EQ(runCycles({0xFE, 0xC0}), 2);  // INC AL
    EXPECT_EQ(runCycles({0xFF, 0xCB}), 2);  // DEC BX
    cpu->bx = 0x50;
    EXPECT_EQ(runCycles({0xFE, 0x07}), 7);  // INC byte [BX]
    EXPECT_EQ(runCycles({0xFF, 0x0F}), 7);  // DEC word [BX]
}
TEST_F(Cpu80286TimingTest, IndirectTransfersPayTheirFloorPlusTax) {
    cpu->sp = 0x200;
    EXPECT_EQ(runCycles({0xFF, 0xE3}), 7 + 2);   // JMP BX
    EXPECT_EQ(runCycles({0xFF, 0x27}), 11 + 2);  // JMP [BX]
    EXPECT_EQ(runCycles({0xFF, 0x2F}), 15 + 2);  // JMP FAR [BX]
    EXPECT_EQ(runCycles({0xFF, 0xD3}), 7 + 2);   // CALL BX
    EXPECT_EQ(runCycles({0xFF, 0x17}), 11 + 2);  // CALL [BX]
    EXPECT_EQ(runCycles({0xFF, 0x1F}), 16 + 2);  // CALL FAR [BX]
}

// --- REP string ops ---

TEST_F(Cpu80286TimingTest, MovsbNonRepCosts5Cycles) {
    EXPECT_EQ(runCycles({0xA4}), 5);  // MOVSB
}
// REP cases add 2 for the prefix byte step() consumes first.
TEST_F(Cpu80286TimingTest, RepMovsbScalesWithCount) {
    cpu->cx = 10;
    // F3 A4: REP MOVSB: 2 (prefix) + 5 + 4*10 = 47
    EXPECT_EQ(runCycles({0xF3, 0xA4}), 47);
    EXPECT_EQ(cpu->cx, 0);  // the whole rep ran
}
TEST_F(Cpu80286TimingTest, RepMovsbWithZeroCountStillChargesTheBaseCost) {
    cpu->cx = 0;
    EXPECT_EQ(runCycles({0xF3, 0xA4}), 7);  // 2 (prefix) + 5 + 4*0
}
TEST_F(Cpu80286TimingTest, RepStosbScalesWithCount) {
    cpu->cx = 20;
    // F3 AA: REP STOSB -- 2 (prefix) + 4 + 3*20 = 66
    EXPECT_EQ(runCycles({0xF3, 0xAA}), 66);
}
TEST_F(Cpu80286TimingTest, RepeCmpsbStopsEarlyAndCostsOnlyTheIterationsThatRan) {
    // Byte 3 differs, so REPE stops after 3 iterations. Cost follows that, not CX.
    cpu->si = 0x100; cpu->di = 0x200; cpu->cx = 10;
    for (int i = 0; i < 10; ++i) { mem[0x100 + i] = 5; mem[0x200 + i] = 5; }
    mem[0x102] = 9;  // third byte differs
    // F3 A6: REPE CMPSB -- 2 (prefix) + 5 + 9*3 = 34
    EXPECT_EQ(runCycles({0xF3, 0xA6}), 34);
    EXPECT_EQ(cpu->cx, 7);  // 10 - 3 actually consumed
}
TEST_F(Cpu80286TimingTest, RepScasbScalesWithCount) {
    cpu->di = 0x300; cpu->cx = 4;
    cpu->ax = 0;  // AL=0
    for (int i = 0; i < 4; ++i) mem[0x300 + i] = 0xFF;  // never matches -> runs all 4
    // F2 AE: REPNE SCASB (stop on match; never matches here) -- 2 (prefix) + 5 + 8*4 = 39
    EXPECT_EQ(runCycles({0xF2, 0xAE}), 39);
}
TEST_F(Cpu80286TimingTest, RepLodsbScalesWithCount) {
    cpu->cx = 12;
    // F3 AC: REP LODSB -- 2 (prefix) + 5 + 4*12 = 55 (80C286 data sheet)
    EXPECT_EQ(runCycles({0xF3, 0xAC}), 55);
}

// --- Effective address: summing base, index and displacement costs a clock ---

TEST_F(Cpu80286TimingTest, BaseIndexDisplacementAddsOneClock) {
    EXPECT_EQ(runCycles({0x8B, 0x00}), 5);        // MOV AX,[BX+SI]
    EXPECT_EQ(runCycles({0x8B, 0x47, 0x02}), 5);  // MOV AX,[BX+2]
    EXPECT_EQ(runCycles({0x8B, 0x40, 0x02}), 6);  // MOV AX,[BX+SI+2]
    EXPECT_EQ(runCycles({0x89, 0x83, 0x00, 0x01}), 4);  // MOV [BP+DI+100h],AX
}

// --- Shift/rotate group: by-1 is its own fixed cost, CL/imm8 scale ------

TEST_F(Cpu80286TimingTest, ShiftByOneRegisterCosts2Cycles) {
    EXPECT_EQ(runCycles({0xD0, 0xE0}), 2);  // D0 /4: SHL AL,1
}
TEST_F(Cpu80286TimingTest, ShiftByOneMemoryCosts7Cycles) {
    cpu->bx = 0x50;
    EXPECT_EQ(runCycles({0xD0, 0x27}), 7);  // SHL byte [BX],1
}
TEST_F(Cpu80286TimingTest, ShiftByClRegisterScalesWithCount) {
    cpu->cx = (cpu->cx & 0xFF00) | 5;  // CL = 5
    // D2 /4: SHL AL,CL: 5 + 5 = 10
    EXPECT_EQ(runCycles({0xD2, 0xE0}), 10);
}
TEST_F(Cpu80286TimingTest, ShiftCostUsesTheMaskedCount) {
    cpu->cx = (cpu->cx & 0xFF00) | 200;  // 200 & 31 = 8
    EXPECT_EQ(runCycles({0xD2, 0xE0}), 5 + 8);
    EXPECT_EQ(runCycles({0xC1, 0xE0, 0x20}), 5);  // SHL AX,32 runs as a count of 0
}
TEST_F(Cpu80286TimingTest, ShiftByImm8MemoryScalesWithCount) {
    cpu->bx = 0x50;
    // C0 /4, imm8=3: SHL byte [BX],3 -- 8 + 3 = 11
    EXPECT_EQ(runCycles({0xC0, 0x27, 0x03}), 11);
}

// --- LOOP/LOOPE/LOOPNE/JCXZ: taken = floor 8 + kQueueRefillTax 2 = 10 (Intel 8-11) ---

TEST_F(Cpu80286TimingTest, LoopTakenCosts10Cycles) {
    cpu->cx = 2;
    EXPECT_EQ(runCycles({0xE2, 0xFE}), 10);  // LOOP $ (rel=-2, back to self) -- taken since cx becomes 1
}
TEST_F(Cpu80286TimingTest, LoopNotTakenCosts4Cycles) {
    cpu->cx = 1;
    EXPECT_EQ(runCycles({0xE2, 0xFE}), 4);  // cx becomes 0 -- not taken
}
TEST_F(Cpu80286TimingTest, LoopneAndLoopeNotTakenCost4Cycles) {
    cpu->cx = 1;
    EXPECT_EQ(runCycles({0xE0, 0xFE}), 4);  // HelpPC 2.10: no jump 4 on the 286
    cpu->cx = 1;
    EXPECT_EQ(runCycles({0xE1, 0xFE}), 4);
}
TEST_F(Cpu80286TimingTest, JcxzTakenCosts10Cycles) {
    cpu->cx = 0;
    EXPECT_EQ(runCycles({0xE3, 0xFE}), 10);
}
TEST_F(Cpu80286TimingTest, JcxzNotTakenCosts4Cycles) {
    cpu->cx = 1;
    EXPECT_EQ(runCycles({0xE3, 0xFE}), 4);
}

// --- PUSHA/POPA/PUSHF/POPF/ENTER/LEAVE ----------------------------------

TEST_F(Cpu80286TimingTest, PushaCosts17Cycles) {
    EXPECT_EQ(runCycles({0x60}), 17);
}
TEST_F(Cpu80286TimingTest, PopaCosts19Cycles) {
    cpu->sp = 0x100;
    EXPECT_EQ(runCycles({0x61}), 19);
}
TEST_F(Cpu80286TimingTest, PushfCosts3Cycles) {
    EXPECT_EQ(runCycles({0x9C}), 3);
}
TEST_F(Cpu80286TimingTest, PopfCosts5Cycles) {
    cpu->sp = 0x100;
    EXPECT_EQ(runCycles({0x9D}), 5);
}
TEST_F(Cpu80286TimingTest, EnterLevel0Costs11Cycles) {
    cpu->sp = 0x200;
    EXPECT_EQ(runCycles({0xC8, 0x04, 0x00, 0x00}), 11);  // ENTER 4,0
}
TEST_F(Cpu80286TimingTest, EnterLevel1Costs15Cycles) {
    cpu->sp = 0x200;
    EXPECT_EQ(runCycles({0xC8, 0x04, 0x00, 0x01}), 15);  // ENTER 4,1
}
TEST_F(Cpu80286TimingTest, EnterLevel3CostsPerTheLexFormula) {
    cpu->sp = 0x200;
    // ENTER 4,3 -- 12 + 4*(3-1) = 20
    EXPECT_EQ(runCycles({0xC8, 0x04, 0x00, 0x03}), 20);
}
TEST_F(Cpu80286TimingTest, LeaveCosts5Cycles) {
    cpu->bp = 0x150;
    cpu->sp = 0x100;
    mem[0x150] = 0x34; mem[0x151] = 0x12;  // value POP BP will read
    EXPECT_EQ(runCycles({0xC9}), 5);
}

// --- INT/INT3/INTO/IRET: floor 23 / 24 / 17, each plus kQueueRefillTax ---

TEST_F(Cpu80286TimingTest, Int3Costs25Cycles) {
    cpu->sp = 0x200;
    EXPECT_EQ(runCycles({0xCC}), 25);  // floor 23 (23-26) + queue-refill tax
}
TEST_F(Cpu80286TimingTest, IntNnCosts25Cycles) {
    cpu->sp = 0x200;
    EXPECT_EQ(runCycles({0xCD, 0x21}), 25);  // INT 21h -- same floor+tax as INT3
}
TEST_F(Cpu80286TimingTest, IntoNotTakenCosts3Cycles) {
    EXPECT_EQ(runCycles({0xCE}), 3);  // OF clear by default -- not taken, no queue flush
}
TEST_F(Cpu80286TimingTest, IntoTakenCosts26Cycles) {
    cpu->sp = 0x200;
    cpu->flags |= FLAG_OF;
    EXPECT_EQ(runCycles({0xCE}), 26);  // floor 24 (24-27) + queue-refill tax
}
TEST_F(Cpu80286TimingTest, IretCosts19Cycles) {
    cpu->sp = 0x100;
    EXPECT_EQ(runCycles({0xCF}), 19);  // floor 17 (17-20) + queue-refill tax
}

// --- IN/OUT: real 80286 is IN=5, OUT=3 (asymmetric, like MOV) ----------

TEST_F(Cpu80286TimingTest, InImmediatePortCosts5Cycles) {
    EXPECT_EQ(runCycles({0xE4, 0x60}), 5);  // IN AL,60h
}
TEST_F(Cpu80286TimingTest, OutImmediatePortCosts3Cycles) {
    EXPECT_EQ(runCycles({0xE6, 0x60}), 3);  // OUT 60h,AL
}
TEST_F(Cpu80286TimingTest, InDxCosts5Cycles) {
    EXPECT_EQ(runCycles({0xEC}), 5);  // IN AL,DX
}
TEST_F(Cpu80286TimingTest, OutDxCosts3Cycles) {
    EXPECT_EQ(runCycles({0xEE}), 3);  // OUT DX,AL
}

// --- MOV's directional memory cost (write=3, read=5, not a flat 7) -----

TEST_F(Cpu80286TimingTest, MovMemoryFromRegisterCosts3Cycles) {
    cpu->bx = 0x50;
    EXPECT_EQ(runCycles({0x88, 0x27}), 3);  // MOV [BX],AH  (mod=00,reg=100,rm=111)
}
TEST_F(Cpu80286TimingTest, MovRegisterFromMemoryCosts5Cycles) {
    cpu->bx = 0x50;
    EXPECT_EQ(runCycles({0x8A, 0x27}), 5);  // MOV AH,[BX]
}
TEST_F(Cpu80286TimingTest, MovAlFromDisplacementCosts5Cycles) {
    EXPECT_EQ(runCycles({0xA0, 0x00, 0x00}), 5);  // MOV AL,[0000]
}
TEST_F(Cpu80286TimingTest, MovDisplacementFromAlCosts3Cycles) {
    EXPECT_EQ(runCycles({0xA2, 0x00, 0x00}), 3);  // MOV [0000],AL
}

// --- Jcc/JMP/CALL/RET: cited floor plus kQueueRefillTax ---

TEST_F(Cpu80286TimingTest, JccShortTakenCosts9Cycles) {
    cpu->flags |= FLAG_ZF;
    EXPECT_EQ(runCycles({0x74, 0x02}), 9);  // JZ rel8, taken -- floor 7 (7-10) + tax
}
TEST_F(Cpu80286TimingTest, JccShortNotTakenCosts3Cycles) {
    EXPECT_EQ(runCycles({0x74, 0x02}), 3);  // JZ rel8, not taken -- no flush, unaffected
}
TEST_F(Cpu80286TimingTest, JmpShortCosts9Cycles) {
    EXPECT_EQ(runCycles({0xEB, 0x00}), 9);  // floor 7 (7-10) + tax
}
TEST_F(Cpu80286TimingTest, JmpNearCosts9Cycles) {
    EXPECT_EQ(runCycles({0xE9, 0x00, 0x00}), 9);  // floor 7 (7-10) + tax
}
TEST_F(Cpu80286TimingTest, JmpFarCosts13Cycles) {
    EXPECT_EQ(runCycles({0xEA, 0x00, 0x00, 0x00, 0x00}), 13);  // floor 11 (11-14) + tax
}
TEST_F(Cpu80286TimingTest, CallNearCosts9Cycles) {
    cpu->sp = 0x200;
    EXPECT_EQ(runCycles({0xE8, 0x00, 0x00}), 9);  // floor 7 (7-10) + tax
}
TEST_F(Cpu80286TimingTest, CallFarCosts15Cycles) {
    cpu->sp = 0x200;
    EXPECT_EQ(runCycles({0x9A, 0x00, 0x00, 0x00, 0x00}), 15);  // floor 13 (13-16) + tax
}
TEST_F(Cpu80286TimingTest, RetCosts13Cycles) {
    cpu->sp = 0x100;
    mem[0x100] = 0x00; mem[0x101] = 0x00;  // return IP popped off the stack
    EXPECT_EQ(runCycles({0xC3}), 13);  // floor 11 (11-14) + tax
}
TEST_F(Cpu80286TimingTest, RetImm16Costs13Cycles) {
    cpu->sp = 0x100;
    mem[0x100] = 0x00; mem[0x101] = 0x00;
    EXPECT_EQ(runCycles({0xC2, 0x04, 0x00}), 13);  // RET 4 -- floor 11 (11-14) + tax
}
TEST_F(Cpu80286TimingTest, RetfCosts17Cycles) {
    cpu->sp = 0x100;
    mem[0x100] = 0x00; mem[0x101] = 0x00;  // IP
    mem[0x102] = 0x00; mem[0x103] = 0x00;  // CS
    EXPECT_EQ(runCycles({0xCB}), 17);  // floor 15 (15-18) + tax
}
TEST_F(Cpu80286TimingTest, RetfImm16Costs17Cycles) {
    cpu->sp = 0x100;
    mem[0x100] = 0x00; mem[0x101] = 0x00;
    mem[0x102] = 0x00; mem[0x103] = 0x00;
    EXPECT_EQ(runCycles({0xCA, 0x04, 0x00}), 17);  // RETF 4 -- floor 15 (15-18) + tax
}

// --- AT bus timing: wait states and the prefetch queue ---

// RAM, ROM and port 1F0h take 3 clocks a cycle; A0000-BFFFF and other ports 6.
class Cpu80286BusTest : public ::testing::Test {
protected:
    std::array<uint8_t, 0x100000> mem{};
    std::array<uint8_t, 4096> clocks{};
    std::unique_ptr<Cpu> cpu;
    Bus bus;

    void SetUp() override {
        clocks.fill(3);
        for (uint32_t page = 0xA0; page < 0xC0; ++page) clocks[page] = 6;
        bus.read   = [this](uint32_t a) { return mem[a & 0xFFFFF]; };
        bus.write  = [this](uint32_t a, uint8_t v) { mem[a & 0xFFFFF] = v; };
        bus.in     = [](uint16_t) -> uint8_t { return 0xFF; };
        bus.out    = [](uint16_t, uint8_t) {};
        bus.in16   = [](uint16_t) -> uint16_t { return 0xFFFF; };
        bus.out16  = [](uint16_t, uint16_t) {};
        bus.mem_clocks = clocks.data();
        bus.io_clocks = [](uint16_t p) -> uint8_t { return p == 0x1F0 ? 3 : 6; };
        cpu = std::make_unique<Cpu>(bus);
    }
    // Fresh CPU (empty queue) running `code` at 0000:0000.
    int first(std::initializer_list<uint8_t> code) {
        uint16_t addr = 0;
        for (uint8_t b : code) mem[addr++] = b;
        cpu->reset();
        cpu->cs = 0; cpu->ip = 0;
        cpu->ss = 0; cpu->sp = 0x8000;
        return cpu->step();
    }
};

TEST_F(Cpu80286BusTest, WordToAnEightBitDeviceTakesTwelveClocks) {
    int ram = first({0xA1, 0x00, 0x02});  // MOV AX, [0200h]
    cpu->reset();
    cpu->cs = 0; cpu->ip = 0; cpu->ds = 0xA000;
    EXPECT_EQ(cpu->step() - ram, 12 - 3);
}

TEST_F(Cpu80286BusTest, ByteToAnEightBitDeviceTakesSixClocks) {
    int ram = first({0xA0, 0x00, 0x02});  // MOV AL, [0200h]
    cpu->reset();
    cpu->cs = 0; cpu->ip = 0; cpu->ds = 0xA000;
    EXPECT_EQ(cpu->step() - ram, 6 - 3);
}

TEST_F(Cpu80286BusTest, SlottedPagesWaitForTheDevicePerByte) {
    int ram = first({0xA1, 0x00, 0x02});  // MOV AX, [0200h]
    std::vector<uint32_t> asked;
    clocks[0xA0] = 6 | Bus::kMemSlotted;
    bus.mem_wait = [&](uint32_t a, uint64_t) { asked.push_back(a); return 20; };
    cpu = std::make_unique<Cpu>(bus);
    cpu->reset();
    cpu->cs = 0; cpu->ip = 0; cpu->ds = 0xA000;
    EXPECT_EQ(cpu->step() - ram, 2 * 20 - 3);
    EXPECT_EQ(asked, (std::vector<uint32_t>{0xA0200, 0xA0201}));
}

TEST_F(Cpu80286BusTest, OddWordTakesTwoCycles) {
    int even = first({0xA1, 0x00, 0x02});  // MOV AX, [0200h]
    int odd = first({0xA1, 0x01, 0x02});   // MOV AX, [0201h]
    EXPECT_EQ(odd - even, 3);
}

TEST_F(Cpu80286BusTest, PortCyclesFollowTheDevice) {
    mem[0] = 0xED;  // IN AX, DX
    auto in_from = [&](uint16_t port) {
        cpu->reset();
        cpu->cs = 0; cpu->ip = 0; cpu->dx = port;
        return cpu->step();
    };
    EXPECT_EQ(in_from(0x60) - in_from(0x1F0), 12 - 3) << "a word from an 8-bit port is two 6-clock cycles";
}

TEST_F(Cpu80286BusTest, BusBoundCodeRunsAtTheFetchRate) {
    for (int i = 0; i < 100; ++i) { mem[3 * i] = 0xB8; mem[3 * i + 1] = 0x34; mem[3 * i + 2] = 0x12; }
    cpu->reset();
    cpu->cs = 0; cpu->ip = 0;
    for (int i = 0; i < 10; ++i) cpu->step();
    int total = 0;
    for (int i = 0; i < 80; ++i) total += cpu->step();
    EXPECT_EQ(total, 80 * 3 * 3 / 2) << "MOV AX,imm16 (3 bytes, 2 clocks) waits on 1.5 word fetches of 3 clocks";
}

TEST_F(Cpu80286BusTest, SlowInstructionsHideTheirFetches) {
    for (int i = 0; i < 40; ++i) { mem[2 * i] = 0xF6; mem[2 * i + 1] = 0xE0; }  // MUL AL
    cpu->reset();
    cpu->cs = 0; cpu->ip = 0;
    for (int i = 0; i < 5; ++i) cpu->step();
    for (int i = 0; i < 20; ++i) EXPECT_EQ(cpu->step(), 13);
}

TEST_F(Cpu80286BusTest, TakenJumpRefetchesItsTargetAtWaitStates) {
    mem[0] = 0xEB; mem[1] = 0x00;                    // JMP $+2
    mem[2] = 0xB8; mem[3] = 0x34; mem[4] = 0x12;     // MOV AX, 1234h
    cpu->reset();
    cpu->cs = 0; cpu->ip = 0;
    cpu->step();
    EXPECT_EQ(cpu->step(), 2 + 2 * (3 - 2)) << "two words at one extra clock each";
}

TEST_F(Cpu80286BusTest, HardwareInterruptCostsIntPlusTwoInta) {
    mem[0x20] = 0x00; mem[0x21] = 0x10; mem[0x22] = 0x00; mem[0x23] = 0x00;
    cpu->reset();
    cpu->cs = 0; cpu->ip = 0x0500;
    cpu->ss = 0; cpu->sp = 0x8000;
    // 23 + queue tax, two 6-clock INTA cycles, and 5 RAM words at one wait state each.
    EXPECT_EQ(cpu->hardware_interrupt(8), 23 + 2 + 2 * 6 + 5);
    EXPECT_EQ(cpu->ip, 0x1000);
}

TEST_F(Cpu80286TimingTest, DivideErrorPaysForTheInterrupt) {
    cpu->sp = 0x200;
    cpu->ax = 4; cpu->bx = 0;
    EXPECT_EQ(runCycles({0xF6, 0xF3}), 14 + 23 + 2);  // DIV BL by zero
    EXPECT_EQ(runCycles({0xD4, 0x00}), 16 + 23 + 2);  // AAM 0
}
TEST_F(Cpu80286TimingTest, BoundOutOfRangePaysForTheInterrupt) {
    cpu->sp = 0x200;
    cpu->bx = 0x50;
    mem[0x50] = 0x00; mem[0x51] = 0x00; mem[0x52] = 0x10; mem[0x53] = 0x00;
    cpu->ax = 0x20;
    EXPECT_EQ(runCycles({0x62, 0x07}), 13 + 23 + 2);
}

TEST_F(Cpu80286TimingTest, HardwareInterruptOnAZeroWaitBus) {
    cpu->ss = 0; cpu->sp = 0x8000;
    EXPECT_EQ(cpu->hardware_interrupt(8), 23 + 2 + 2 * 2);
}

}  // namespace
