// GoogleTest suite for cpu80286.cpp's per-opcode cycle-cost model --
// separate from cpu80286_test.cpp (which covers instruction *semantics*)
// because this file exists specifically to pin down a real regression: an
// earlier version of this core charged a handful of generic bucket costs
// (2 register-operand / 7 memory-operand / 7 taken-branch / 3 not-taken)
// for every opcode regardless of which one it actually was, badly
// undercosting MUL/DIV/IDIV and REP-prefixed string/shift instructions in
// particular. That surfaced concretely running a real piece of period
// software (Landmark System Speed Test v2.00) under this emulator: it
// measured an apparent ~11.5-14 MHz CPU clock against this machine's real,
// wall-clock-paced 8 MHz (app.js's frame() -- see CLAUDE.md "Never speed
// these up"), because a DIV/MUL-heavy timing loop was completing far more
// iterations per real second than real 80286 timings allow.
//
// Reference cycle counts below are the 80286 column of the published
// "Art of Assembly" (Randall Hyde) Appendix D instruction-timing table,
// itself reproducing Intel's own iAPX 286 Programmer's Reference Manual /
// 80286 data sheet timing appendix -- the same source cited inline in
// cpu80286.cpp at each fixed constant. See IBM_PCAT_REVIEW.md's CPU-timing
// section for the full investigation.

#include <gtest/gtest.h>

#include "cpu80286.h"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>

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
    // Assemble `code` at CS:0 and execute exactly one instruction, returning
    // the cycle count step() reports for it -- the thing under test here.
    int runCycles(std::initializer_list<uint8_t> code) {
        load(code);
        cpu->ip = 0;
        return cpu->step();
    }
};

// --- MUL/IMUL/DIV/IDIV: each op costs its own real total below, not a  ---
// --- flat 7 for every op -- the dominant real-world cause of the       ---
// --- Landmark Speed Test overshoot.                                    ---

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
    // TEST reg,imm8 and NOT/NEG were never the bug, but grp3_unary now
    // computes its own cost table rather than a blanket CYC_MEM -- pin
    // down that the reg-operand case (which a flat 7 for every case in
    // this group would overcost) reads back correctly too.
    EXPECT_EQ(runCycles({0xF6, 0xD0}), 2);         // F6 /2: NOT AL
    EXPECT_EQ(runCycles({0xF6, 0xD8}), 2);         // F6 /3: NEG AL
    EXPECT_EQ(runCycles({0xF6, 0xC0, 0x00}), 3);   // F6 /0, imm8: TEST AL,0
}

// --- grp1_immed (ADD/OR/ADC/SBB/AND/SUB/XOR/CMP r/m,imm): the single ----
// --- hottest opcode in Landmark's actual hot path (0x83, per the       ---
// --- opcode-histogram diagnostic) -- was flat-costed at CYC_MEM=7 even ---
// --- for a register operand, whose real cost is 3.                    ---

TEST_F(Cpu80286TimingTest, Grp1ImmedSignExtended8BitRegisterCosts3Cycles) {
    EXPECT_EQ(runCycles({0x83, 0xF8, 0x00}), 3);  // 83 /7, mod=11,rm=000: CMP AX,0
}
TEST_F(Cpu80286TimingTest, Grp1ImmedSignExtended8BitMemoryCosts7Cycles) {
    EXPECT_EQ(runCycles({0x83, 0x3F, 0x00}), 7);  // 83 /7, mod=00,rm=111: CMP [BX],0
}
TEST_F(Cpu80286TimingTest, Grp1Immed8BitRegisterCosts3Cycles) {
    EXPECT_EQ(runCycles({0x80, 0xC0, 0x01}), 3);  // 80 /0, mod=11,rm=000: ADD AL,1
}
TEST_F(Cpu80286TimingTest, Grp1Immed8BitMemoryCosts7Cycles) {
    EXPECT_EQ(runCycles({0x80, 0x07, 0x01}), 7);  // 80 /0, mod=00,rm=111: ADD byte [BX],1
}

TEST_F(Cpu80286TimingTest, PushReg16Costs3Cycles) {
    // Was flat-costed at CYC_MEM=7 like every other opcode in the original
    // 4-bucket model; real 80286 cost for PUSH reg16 is 3.
    EXPECT_EQ(runCycles({0x50}), 3);  // PUSH AX
}

// --- REP-prefixed string ops: cost scales with count, not a flat 7 -----

TEST_F(Cpu80286TimingTest, MovsbNonRepCosts5Cycles) {
    EXPECT_EQ(runCycles({0xA4}), 5);  // MOVSB
}
// Every REP-prefixed case below adds 2 to the string_op() formula itself --
// step()'s prefix-fetch loop (pre-existing, untouched by this fix) charges
// +2 per prefix byte consumed before the opcode dispatch even runs, and
// REP/REPE/REPNE (0xF2/0xF3) is itself one such prefix byte.
TEST_F(Cpu80286TimingTest, RepMovsbScalesWithCount) {
    cpu->cx = 10;
    // F3 A4: REP MOVSB -- 2 (prefix) + 5 + 4*10 = 47, not the old flat 7
    EXPECT_EQ(runCycles({0xF3, 0xA4}), 47);
    EXPECT_EQ(cpu->cx, 0);  // sanity: the whole rep actually ran
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
    // CMPSB compares [SI] vs [ES:DI]; make byte 3 differ so REPE stops
    // after 3 iterations even though CX asked for 10 -- the cost must
    // follow the actual iteration count (3), not the original CX (10).
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
    // D2 /4: SHL AL,CL -- 5 + 5 = 10, not the old flat 7
    EXPECT_EQ(runCycles({0xD2, 0xE0}), 10);
}
TEST_F(Cpu80286TimingTest, ShiftByImm8MemoryScalesWithCount) {
    cpu->bx = 0x50;
    // C0 /4, imm8=3: SHL byte [BX],3 -- 8 + 3 = 11
    EXPECT_EQ(runCycles({0xC0, 0x27, 0x03}), 11);
}

// --- LOOP/LOOPE/LOOPNE/JCXZ: a classic counting-loop construct, and the
// --- one this fix's first pass genuinely missed (found by disassembling
// --- Landmark itself, since the MUL/DIV/REP-string fixes above turned out
// --- not to move its own CPU-speed reading at all). Taken cost is floor(8)
// --- + kQueueRefillTax(2) = 10, landing inside Intel's documented 8-11
// --- range for the taken case -- see cpu80286.h's queue-refill-tax comment.

TEST_F(Cpu80286TimingTest, LoopTakenCosts10Cycles) {
    cpu->cx = 2;
    EXPECT_EQ(runCycles({0xE2, 0xFE}), 10);  // LOOP $ (rel=-2, back to self) -- taken since cx becomes 1
}
TEST_F(Cpu80286TimingTest, LoopNotTakenCosts4Cycles) {
    cpu->cx = 1;
    EXPECT_EQ(runCycles({0xE2, 0xFE}), 4);  // cx becomes 0 -- not taken
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

// --- INT/INT3/INTO/IRET: a flat 45 would *over*count these by ~2x (the
// --- opposite direction from MUL/DIV) -- real 80286 floor is 23 (INT3/INT
// --- nn), 24 (INTO taken), 17 (IRET), each + kQueueRefillTax since entering
// --- or returning from an interrupt is itself a control transfer that
// --- flushes the prefetch queue (see cpu80286.h).

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

// --- Jcc/JMP/CALL/RET: every queue-flushing control transfer gets its
// --- cited floor-of-range cost + kQueueRefillTax (cpu80286.h), landing
// --- inside Intel's own documented range for each (verified per opcode
// --- against the same Appendix D table cited throughout this file). A
// --- flat 11 for CALL near (0xE8) would sit *above* Intel's own 7-10
// --- range for that opcode, a real, separate overcost bug caught while
// --- auditing every control-transfer opcode for this fix.

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
    EXPECT_EQ(runCycles({0xE8, 0x00, 0x00}), 9);  // floor 7 (7-10) + tax -- was a flat 11, above Intel's own range
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

}  // namespace
