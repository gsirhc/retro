// GoogleTest suite for cpu80486.cpp's per-opcode cycle-cost model --
// separate from cpu80486_test.cpp (which covers instruction *semantics*)
// for the same reason ibmpc-at splits its two 80286 suites: the cost model
// is a distinct thing that can regress on its own, silently, and only
// shows up later as period software measuring the wrong CPU speed. That
// machine's own investigation is the cautionary tale -- a handful of
// generic bucket costs made Landmark System Speed Test read ~11.5-14 MHz
// against a real, wall-clock-paced 8 MHz 286 (IBM_PCAT_REVIEW.md), because
// MUL/DIV and the REP-prefixed string ops were undercosted by multiples.
//
// Reference values are the 486 column of the Quantasm "80x86 Integer
// Instruction Set (8088 - Pentium)" table, which reproduces Intel's own
// i486 Programmer's Reference Manual timing appendix -- the same source
// cited inline at every constant in cpu80486.cpp. Three things this suite
// exists specifically to pin down, because they are where a 486 differs
// structurally from the 286 core next door:
//
//   - The 486's on-chip cache makes a load or store a one-cycle operation,
//     so MOV is 1 clock in every direction and the 286's directional 3/5
//     asymmetry is gone -- but the ALU group is NOT uniform: CMP/TEST read
//     memory (2) while ADD and friends read-modify-write it (3).
//   - The barrel shifter makes SHL/SHR/SAR/ROL/ROR count-INDEPENDENT
//     (the 286 charged 5+n), while RCL/RCR stay iterative.
//   - There is no "+m" prefetch-refill term in the 486 column at all, so
//     unlike cpu80286.h's kQueueRefillTax the published control-transfer
//     numbers are charged directly.
//
// Every prefix byte costs 1 clock (cpu80486.cpp's step(): the published
// LOCK figure, applied uniformly), so any 0x66/0x67/0xF3-prefixed
// expectation below is the instruction's own cost plus one per prefix.

#include <gtest/gtest.h>

#include "cpu80486.h"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <vector>

namespace {

using cpu80486::Bus;
using cpu80486::Cpu;
using cpu80486::FLAG_OF;
using cpu80486::FLAG_ZF;

class Cpu80486TimingTest : public ::testing::Test {
protected:
    std::array<uint8_t, 0x100000> mem{};
    std::unique_ptr<Cpu> cpu;

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
        cpu->cs = 0;
        cpu->eip = 0;
    }

    void load(std::initializer_list<uint8_t> code, uint16_t at = 0) {
        uint16_t addr = at;
        for (uint8_t b : code) mem[addr++] = b;
    }
    // Assemble `code` at CS:0 and execute exactly one instruction,
    // returning the cycle count step() reports -- the thing under test.
    int runCycles(std::initializer_list<uint8_t> code) {
        load(code);
        cpu->eip = 0;
        return cpu->step();
    }
};

// --- MOV and the ALU group ------------------------------------------------
// The 486's cache makes every MOV 1 clock; the ALU group splits by whether
// the operation writes memory back.

TEST_F(Cpu80486TimingTest, MovIsOneCycleInEveryDirection) {
    cpu->ebx = 0x50;
    EXPECT_EQ(runCycles({0x89, 0xD8}), 1);        // MOV AX,BX
    EXPECT_EQ(runCycles({0x88, 0x07}), 1);        // MOV [BX],AL
    EXPECT_EQ(runCycles({0x8A, 0x07}), 1);        // MOV AL,[BX]
    EXPECT_EQ(runCycles({0xB8, 0x00, 0x00}), 1);  // MOV AX,imm16
    EXPECT_EQ(runCycles({0xA0, 0x00, 0x00}), 1);  // MOV AL,[0000]
    EXPECT_EQ(runCycles({0xA2, 0x00, 0x00}), 1);  // MOV [0000],AL
}

TEST_F(Cpu80486TimingTest, AluGroupSplitsByWhetherItWritesMemoryBack) {
    cpu->ebx = 0x50;
    EXPECT_EQ(runCycles({0x01, 0xD8}), 1);  // ADD AX,BX      -- reg,reg
    EXPECT_EQ(runCycles({0x03, 0x07}), 2);  // ADD AX,[BX]    -- reg,mem
    EXPECT_EQ(runCycles({0x01, 0x07}), 3);  // ADD [BX],AX    -- mem,reg (read-modify-write)
    EXPECT_EQ(runCycles({0x05, 0x00, 0x00}), 1);  // ADD AX,imm16
}

TEST_F(Cpu80486TimingTest, CmpAgainstMemoryIsCheaperThanAddBecauseItNeverWritesBack) {
    // The single most common instruction in a compare-and-branch loop.
    // Charging the whole ALU group one flat number would overcost this by
    // 50% -- published: CMP mem,reg 2, ADD mem,reg 3.
    cpu->ebx = 0x50;
    EXPECT_EQ(runCycles({0x39, 0x07}), 2);        // CMP [BX],AX
    EXPECT_EQ(runCycles({0x83, 0x3F, 0x00}), 2);  // CMP word [BX],0
    EXPECT_EQ(runCycles({0x80, 0x07, 0x01}), 3);  // ADD byte [BX],1
    EXPECT_EQ(runCycles({0x83, 0xF8, 0x00}), 1);  // CMP AX,0 -- register operand
}

TEST_F(Cpu80486TimingTest, TestIsReadOnlyAndSoCostsTwoAgainstMemory) {
    cpu->ebx = 0x50;
    EXPECT_EQ(runCycles({0x85, 0x07}), 2);        // TEST [BX],AX
    EXPECT_EQ(runCycles({0x84, 0xC0}), 1);        // TEST AL,AL
    EXPECT_EQ(runCycles({0xF6, 0xC0, 0x00}), 1);  // F6 /0: TEST AL,imm8
    EXPECT_EQ(runCycles({0xF6, 0x07, 0x00}), 2);  // F6 /0: TEST byte [BX],imm8
}

TEST_F(Cpu80486TimingTest, IncDecAndNotNegSplitRegisterFromMemory) {
    cpu->ebx = 0x50;
    EXPECT_EQ(runCycles({0x40}), 1);        // INC AX
    EXPECT_EQ(runCycles({0xFF, 0x07}), 3);  // INC word [BX]
    EXPECT_EQ(runCycles({0xF6, 0xD0}), 1);  // NOT AL
    EXPECT_EQ(runCycles({0xF6, 0x1F}), 3);  // NEG byte [BX]
}

TEST_F(Cpu80486TimingTest, BaseIndexDisplacementAddressingCostsOneExtraClock) {
    // The table's legend publishes exactly one effective-address penalty
    // for the 286-486: "base+index+disp = +1, all others, no penalty".
    cpu->ebx = 0x50;
    cpu->esi = 0x02;
    EXPECT_EQ(runCycles({0x8B, 0x00}), 1);        // MOV AX,[BX+SI]    -- base+index, no disp
    EXPECT_EQ(runCycles({0x8B, 0x40, 0x04}), 2);  // MOV AX,[BX+SI+4]  -- +1
    EXPECT_EQ(runCycles({0x8B, 0x47, 0x04}), 1);  // MOV AX,[BX+4]     -- base+disp, no penalty
}

TEST_F(Cpu80486TimingTest, LeaLandsOnItsPublishedOneToTwoRange) {
    cpu->ebx = 0x50;
    cpu->esi = 0x02;
    EXPECT_EQ(runCycles({0x8D, 0x07}), 1);        // LEA AX,[BX]
    EXPECT_EQ(runCycles({0x8D, 0x40, 0x04}), 2);  // LEA AX,[BX+SI+4]
}

// --- MUL/IMUL: the early-out model, anchored to the published endpoints ---

TEST_F(Cpu80486TimingTest, MultiplyCostsTheFloorWhenTheMultiplierIsTiny) {
    cpu->ebx = 1;
    EXPECT_EQ(runCycles({0xF6, 0xE3}), 13);  // MUL BL, BL=1
    EXPECT_EQ(runCycles({0xF7, 0xE3}), 13);  // MUL BX, BX=1
}

TEST_F(Cpu80486TimingTest, MultiplyCostsTheCeilingWhenTheTopBitIsSet) {
    // Published endpoints: r/m8 13-18, r/m16 13-26, r/m32 13-42.
    cpu->ebx = 0xFFFFFFFFu;
    EXPECT_EQ(runCycles({0xF6, 0xE3}), 18);         // MUL BL, BL=FFh
    EXPECT_EQ(runCycles({0xF7, 0xE3}), 26);         // MUL BX, BX=FFFFh
    EXPECT_EQ(runCycles({0x66, 0xF7, 0xE3}), 43);   // MUL EBX -- 42 + 1 prefix
}

TEST_F(Cpu80486TimingTest, MultiplyCostRisesWithTheMultipliersSignificantBits) {
    cpu->ebx = 0x0100;  // MSB at bit 9
    EXPECT_EQ(runCycles({0xF7, 0xE3}), 13 + (9 - 3));  // MUL BX
}

TEST_F(Cpu80486TimingTest, MultiplyCostsTheSameForMemoryAndRegisterOperands) {
    // A genuine 486 peculiarity worth pinning: unlike almost every other
    // group, the published MUL figures are identical for both.
    cpu->ebx = 0x50;
    mem[0x50] = 0xFF;
    EXPECT_EQ(runCycles({0xF6, 0x27}), 18);  // MUL byte [BX], value FFh
}

TEST_F(Cpu80486TimingTest, TwoAndThreeOperandImulUseTheSameEarlyOutModel) {
    cpu->ebx = 0xFFFF;
    EXPECT_EQ(runCycles({0x0F, 0xAF, 0xC3}), 26);              // IMUL AX,BX
    EXPECT_EQ(runCycles({0x69, 0xC3, 0x07, 0x00}), 13);        // IMUL AX,BX,7
    EXPECT_EQ(runCycles({0x66, 0x69, 0xC3, 0x07, 0x00, 0x00, 0x00}), 14);  // IMUL EAX,EBX,7 -- 13 + 1 prefix
}

// --- DIV/IDIV: single published values, an order of magnitude apart ------

TEST_F(Cpu80486TimingTest, DivideCostsScaleSharplyWithOperandWidth) {
    cpu->eax = 4; cpu->edx = 0; cpu->ebx = 2;
    EXPECT_EQ(runCycles({0xF6, 0xF3}), 16);        // DIV BL
    EXPECT_EQ(runCycles({0xF7, 0xF3}), 24);        // DIV BX
    EXPECT_EQ(runCycles({0x66, 0xF7, 0xF3}), 41);  // DIV EBX -- 40 + 1 prefix
}

TEST_F(Cpu80486TimingTest, SignedDivideCostsMoreThanUnsignedAndAddsOneForMemory) {
    cpu->eax = 4; cpu->edx = 0; cpu->ebx = 2;
    EXPECT_EQ(runCycles({0xF6, 0xFB}), 19);        // IDIV BL
    EXPECT_EQ(runCycles({0xF7, 0xFB}), 27);        // IDIV BX
    EXPECT_EQ(runCycles({0x66, 0xF7, 0xFB}), 44);  // IDIV EBX -- 43 + 1 prefix
    cpu->ebx = 0x50;
    mem[0x50] = 2; mem[0x51] = 0;
    cpu->eax = 4; cpu->edx = 0;
    EXPECT_EQ(runCycles({0xF7, 0x3F}), 28);        // IDIV word [BX]
}

// --- Shifts: the barrel shifter makes the plain forms count-independent --

TEST_F(Cpu80486TimingTest, PlainShiftsAndRotatesDoNotScaleWithTheCount) {
    // The 286 charged 5+n / 8+n here; the 486 does not, which is exactly
    // the kind of generational difference a copied cost table would miss.
    cpu->ecx = 0x1F;  // CL = 31
    cpu->ebx = 0x50;
    EXPECT_EQ(runCycles({0xD0, 0xE0}), 3);        // SHL AL,1
    EXPECT_EQ(runCycles({0xD0, 0x27}), 4);        // SHL byte [BX],1
    EXPECT_EQ(runCycles({0xD2, 0xE0}), 3);        // SHL AL,CL -- CL=31, still 3
    EXPECT_EQ(runCycles({0xC0, 0xE0, 0x03}), 2);  // SHL AL,3  -- imm8 form is the cheap one
    EXPECT_EQ(runCycles({0xC0, 0x27, 0x03}), 4);  // SHL byte [BX],3
}

TEST_F(Cpu80486TimingTest, RotateThroughCarryStaysIterativeAndSaturates) {
    // Published RCL/RCR by a count: 8-30 (register), 9-31 (memory).
    cpu->ecx = 5;
    EXPECT_EQ(runCycles({0xD0, 0xD0}), 3);   // RCL AL,1 -- the separate by-1 case
    EXPECT_EQ(runCycles({0xD2, 0xD0}), 12);  // RCL AL,CL with CL=5 -> 7+5
    cpu->ecx = 0x1F;                          // CL = 31
    EXPECT_EQ(runCycles({0xD2, 0xD0}), 30);  // saturates at the published ceiling
    cpu->ebx = 0x50;
    cpu->ecx = 5;
    EXPECT_EQ(runCycles({0xD2, 0x17}), 13);  // RCL byte [BX],CL -> 8+5
}

TEST_F(Cpu80486TimingTest, DoublePrecisionShiftsChargeTheirOwnPublishedCosts) {
    cpu->ebx = 0x50;
    EXPECT_EQ(runCycles({0x0F, 0xA4, 0xD8, 0x04}), 2);  // SHLD AX,BX,4
    EXPECT_EQ(runCycles({0x0F, 0xA4, 0x1F, 0x04}), 3);  // SHLD [BX],BX,4
    EXPECT_EQ(runCycles({0x0F, 0xA5, 0xD8}), 3);        // SHLD AX,BX,CL
    EXPECT_EQ(runCycles({0x0F, 0xA5, 0x1F}), 4);        // SHLD [BX],BX,CL
}

// --- REP-prefixed string ops: cost follows the iterations that ran -------

TEST_F(Cpu80486TimingTest, NonRepStringOpsChargeTheirPublishedSingleIterationCost) {
    EXPECT_EQ(runCycles({0xA4}), 7);  // MOVSB
    EXPECT_EQ(runCycles({0xA6}), 8);  // CMPSB
    EXPECT_EQ(runCycles({0xAA}), 5);  // STOSB
    EXPECT_EQ(runCycles({0xAC}), 5);  // LODSB
    EXPECT_EQ(runCycles({0xAE}), 6);  // SCASB
}

TEST_F(Cpu80486TimingTest, RepMovsScalesWithCount) {
    cpu->ecx = 10;
    // F3 A4: 1 (prefix) + 12 + 3*10
    EXPECT_EQ(runCycles({0xF3, 0xA4}), 43);
    EXPECT_EQ(cpu->ecx & 0xFFFF, 0u) << "sanity: the whole rep actually ran";
}

TEST_F(Cpu80486TimingTest, RepMovsHonorsThePublishedZeroAndOneCountSpecialCases) {
    // The table's own footnote on REP MOVS/REP STOS: "5 if n=0, 13 if n=1"
    // -- neither of which is what the 12+3n formula would give.
    cpu->ecx = 0;
    EXPECT_EQ(runCycles({0xF3, 0xA4}), 1 + 5);
    cpu->ecx = 1;
    EXPECT_EQ(runCycles({0xF3, 0xA4}), 1 + 13);
}

TEST_F(Cpu80486TimingTest, RepStosScalesWithCount) {
    cpu->ecx = 20;
    // F3 AA: 1 (prefix) + 7 + 4*20
    EXPECT_EQ(runCycles({0xF3, 0xAA}), 88);
}

TEST_F(Cpu80486TimingTest, RepeCmpsStopsEarlyAndBillsOnlyTheIterationsThatRan) {
    // Byte 3 differs, so REPE stops after 3 iterations even though CX asked
    // for 10 -- the cost must follow the real iteration count.
    cpu->esi = 0x100; cpu->edi = 0x200; cpu->ecx = 10;
    for (int i = 0; i < 10; ++i) { mem[0x100 + i] = 5; mem[0x200 + i] = 5; }
    mem[0x102] = 9;
    // F3 A6: 1 (prefix) + 7 + 7*3
    EXPECT_EQ(runCycles({0xF3, 0xA6}), 29);
    EXPECT_EQ(cpu->ecx & 0xFFFF, 7u);
}

TEST_F(Cpu80486TimingTest, RepScasScalesWithCount) {
    cpu->edi = 0x300; cpu->ecx = 4; cpu->eax = 0;
    for (int i = 0; i < 4; ++i) mem[0x300 + i] = 0xFF;  // never matches -> all 4 run
    // F2 AE: 1 (prefix) + 7 + 5*4
    EXPECT_EQ(runCycles({0xF2, 0xAE}), 28);
}

TEST_F(Cpu80486TimingTest, RepMovsdCostsTheSameFormulaAtDwordWidth) {
    cpu->ecx = 4;
    // F3 66 A5: 2 prefixes + 12 + 3*4
    EXPECT_EQ(runCycles({0xF3, 0x66, 0xA5}), 2 + 24);
}

// --- LOOP/JCXZ: published per-op, per-outcome ----------------------------

TEST_F(Cpu80486TimingTest, LoopAndJcxzChargePublishedTakenAndNotTakenCosts) {
    cpu->ecx = 2;
    EXPECT_EQ(runCycles({0xE2, 0xFE}), 7);  // LOOP taken
    cpu->ecx = 1;
    EXPECT_EQ(runCycles({0xE2, 0xFE}), 6);  // LOOP not taken
    cpu->ecx = 2;
    cpu->set_flag(FLAG_ZF, true);
    EXPECT_EQ(runCycles({0xE1, 0xFE}), 9);  // LOOPE taken -- more expensive than LOOP
    cpu->ecx = 1;
    EXPECT_EQ(runCycles({0xE1, 0xFE}), 6);  // LOOPE not taken
    cpu->ecx = 0;
    EXPECT_EQ(runCycles({0xE3, 0xFE}), 8);  // JCXZ taken
    cpu->ecx = 1;
    EXPECT_EQ(runCycles({0xE3, 0xFE}), 5);  // JCXZ not taken
}

// --- Control transfers: flat published numbers, no prefetch-refill term --

TEST_F(Cpu80486TimingTest, ConditionalBranchesCostOneNotTakenAndThreeTaken) {
    EXPECT_EQ(runCycles({0x74, 0x02}), 1);  // JZ rel8, not taken
    cpu->set_flag(FLAG_ZF, true);
    EXPECT_EQ(runCycles({0x74, 0x02}), 3);  // JZ rel8, taken
    EXPECT_EQ(runCycles({0x0F, 0x84, 0x02, 0x00}), 3);  // JZ rel16, taken -- same cost
}

TEST_F(Cpu80486TimingTest, JumpsAndCallsChargeTheirPublishedCosts) {
    cpu->esp = 0x200;
    EXPECT_EQ(runCycles({0xEB, 0x00}), 3);                          // JMP short
    EXPECT_EQ(runCycles({0xE9, 0x00, 0x00}), 3);                    // JMP near
    EXPECT_EQ(runCycles({0xEA, 0x00, 0x00, 0x00, 0x00}), 17);       // JMP far
    EXPECT_EQ(runCycles({0xE8, 0x00, 0x00}), 3);                    // CALL near
    EXPECT_EQ(runCycles({0x9A, 0x00, 0x00, 0x00, 0x00}), 18);       // CALL far
    EXPECT_EQ(runCycles({0xFF, 0xD0}), 5);                          // CALL near indirect (reg)
    EXPECT_EQ(runCycles({0xFF, 0xE0}), 5);                          // JMP near indirect (reg)
    cpu->ebx = 0x50;
    EXPECT_EQ(runCycles({0xFF, 0x1F}), 17);                         // CALL far indirect
    EXPECT_EQ(runCycles({0xFF, 0x2F}), 13);                         // JMP far indirect
}

TEST_F(Cpu80486TimingTest, ReturnsChargeTheirPublishedCosts) {
    cpu->esp = 0x100;
    EXPECT_EQ(runCycles({0xC3}), 5);              // RET near
    cpu->esp = 0x100;
    EXPECT_EQ(runCycles({0xC2, 0x04, 0x00}), 5);  // RET near imm16
    cpu->esp = 0x100;
    EXPECT_EQ(runCycles({0xCB}), 13);             // RETF
    cpu->esp = 0x100;
    EXPECT_EQ(runCycles({0xCA, 0x04, 0x00}), 14); // RETF imm16 -- one more than plain RETF
}

TEST_F(Cpu80486TimingTest, InterruptEntryAndReturnChargeTheirPublishedCosts) {
    cpu->esp = 0x200;
    EXPECT_EQ(runCycles({0xCC}), 26);        // INT 3
    cpu->esp = 0x200;
    EXPECT_EQ(runCycles({0xCD, 0x21}), 30);  // INT 21h -- the immediate costs more
    EXPECT_EQ(runCycles({0xCE}), 3);         // INTO, OF clear -- not taken
    cpu->esp = 0x200;
    cpu->set_flag(FLAG_OF, true);
    EXPECT_EQ(runCycles({0xCE}), 28);        // INTO taken
    cpu->esp = 0x100;
    EXPECT_EQ(runCycles({0xCF}), 15);        // IRET
}

TEST_F(Cpu80486TimingTest, HardwareInterruptDeliveryIsBilledExactlyOnce) {
    // interrupt() is the chipset's entry point for a PIC-delivered IRQ. It
    // charges INT3's published 26 (the closest anchor: same IVT work, no
    // immediate to fetch) -- and step()'s own INT paths must not add to
    // that, which is why do_interrupt() does the work without touching the
    // cycle counter.
    cpu->esp = 0x200;
    uint64_t before = cpu->cycles;
    EXPECT_EQ(cpu->interrupt(0x08), 26);
    EXPECT_EQ(cpu->cycles - before, 26u) << "no double billing";
}

// --- Stack, frame and flag instructions ----------------------------------

TEST_F(Cpu80486TimingTest, StackInstructionsChargeTheirPublishedCosts) {
    cpu->esp = 0x200;
    cpu->ebx = 0x50;
    EXPECT_EQ(runCycles({0x50}), 1);              // PUSH AX
    cpu->esp = 0x200;
    EXPECT_EQ(runCycles({0x58}), 1);              // POP AX
    cpu->esp = 0x200;
    EXPECT_EQ(runCycles({0x68, 0x00, 0x00}), 1);  // PUSH imm16
    cpu->esp = 0x200;
    EXPECT_EQ(runCycles({0xFF, 0x37}), 4);        // PUSH word [BX]
    cpu->esp = 0x200;
    EXPECT_EQ(runCycles({0x8F, 0x07}), 6);        // POP word [BX] -- the expensive one
    cpu->esp = 0x200;
    EXPECT_EQ(runCycles({0x06}), 3);              // PUSH ES
    cpu->esp = 0x200;
    EXPECT_EQ(runCycles({0x07}), 3);              // POP ES
}

TEST_F(Cpu80486TimingTest, PushaPopaPushfPopf) {
    cpu->esp = 0x200;
    EXPECT_EQ(runCycles({0x60}), 11);  // PUSHA
    cpu->esp = 0x200;
    EXPECT_EQ(runCycles({0x61}), 9);   // POPA
    cpu->esp = 0x200;
    EXPECT_EQ(runCycles({0x9C}), 4);   // PUSHF
    cpu->esp = 0x200;
    EXPECT_EQ(runCycles({0x9D}), 9);   // POPF -- more than twice PUSHF
    cpu->esp = 0x200;
    EXPECT_EQ(runCycles({0x66, 0x9C}), 5);  // PUSHFD -- 4 + 1 prefix
}

TEST_F(Cpu80486TimingTest, EnterFollowsItsPublishedNestingLevelFormula) {
    cpu->esp = 0x200;
    EXPECT_EQ(runCycles({0xC8, 0x04, 0x00, 0x00}), 14);  // ENTER 4,0
    cpu->esp = 0x200;
    EXPECT_EQ(runCycles({0xC8, 0x04, 0x00, 0x01}), 17);  // ENTER 4,1
    cpu->esp = 0x200;
    EXPECT_EQ(runCycles({0xC8, 0x04, 0x00, 0x03}), 26);  // ENTER 4,3 -> 17 + 3*3
    cpu->ebp = 0x150; cpu->esp = 0x100;
    EXPECT_EQ(runCycles({0xC9}), 5);                     // LEAVE
}

TEST_F(Cpu80486TimingTest, FlagInstructionsAndTheInterruptFlagPairAreNotTheSamePrice) {
    // CLI/STI are 5 on a 486 against 2 for every other flag instruction --
    // and against the 286's own 3, so this is a real generational change.
    EXPECT_EQ(runCycles({0xF8}), 2);  // CLC
    EXPECT_EQ(runCycles({0xF9}), 2);  // STC
    EXPECT_EQ(runCycles({0xF5}), 2);  // CMC
    EXPECT_EQ(runCycles({0xFC}), 2);  // CLD
    EXPECT_EQ(runCycles({0xFD}), 2);  // STD
    EXPECT_EQ(runCycles({0xFA}), 5);  // CLI
    EXPECT_EQ(runCycles({0xFB}), 5);  // STI
    EXPECT_EQ(runCycles({0x9E}), 2);  // SAHF
    EXPECT_EQ(runCycles({0x9F}), 3);  // LAHF
}

// --- Port I/O: far slower than the 286, and asymmetric the other way ----

TEST_F(Cpu80486TimingTest, PortIoIsExpensiveAndOutCostsMoreThanIn) {
    // Published 486: IN 14, OUT 16 -- against the 286's 5 and 3. Getting
    // this backwards (or reusing the 286's numbers) would badly misprice
    // every BIOS polling loop on this machine.
    EXPECT_EQ(runCycles({0xE4, 0x60}), 14);  // IN AL,imm8
    EXPECT_EQ(runCycles({0xE6, 0x60}), 16);  // OUT imm8,AL
    EXPECT_EQ(runCycles({0xEC}), 14);        // IN AL,DX
    EXPECT_EQ(runCycles({0xEE}), 16);        // OUT DX,AL
    EXPECT_EQ(runCycles({0xED}), 14);        // IN AX,DX
    EXPECT_EQ(runCycles({0xEF}), 16);        // OUT DX,AX
}

TEST_F(Cpu80486TimingTest, StringPortIoChargesSeventeenPerIteration) {
    EXPECT_EQ(runCycles({0x6C}), 17);  // INSB
    EXPECT_EQ(runCycles({0x6E}), 17);  // OUTSB
    cpu->ecx = 3;
    EXPECT_EQ(runCycles({0xF3, 0x6C}), 1 + 17 * 3);  // REP INSB
    cpu->ecx = 0;
    EXPECT_EQ(runCycles({0xF3, 0x6C}), 1 + 5);       // empty rep
}

// --- 486-native and 386-inherited opcodes --------------------------------

TEST_F(Cpu80486TimingTest, BswapIsASingleCycle) {
    EXPECT_EQ(runCycles({0x0F, 0xC8}), 1);  // BSWAP EAX
}

TEST_F(Cpu80486TimingTest, XaddAndCmpxchgChargeTheirPublishedCosts) {
    cpu->ebx = 0x50;
    EXPECT_EQ(runCycles({0x0F, 0xC1, 0xD8}), 3);  // XADD AX,BX
    EXPECT_EQ(runCycles({0x0F, 0xC1, 0x0F}), 4);  // XADD [BX],CX
    EXPECT_EQ(runCycles({0x0F, 0xB1, 0xD9}), 6);  // CMPXCHG CX,BX -- register form
}

TEST_F(Cpu80486TimingTest, CmpxchgAgainstMemorySplitsThePublishedSevenToTenRange) {
    // Published "7-10" for the memory form; this core reads that as
    // compare-only vs compare-plus-store, which is the only split the
    // instruction actually has (labelled in cpu80486.cpp -- Intel prints
    // the range without saying which end is which).
    cpu->ebx = 0x50;
    cpu->eax = 0x1111;
    mem[0x50] = 0x11; mem[0x51] = 0x11;   // matches AX -> store happens
    EXPECT_EQ(runCycles({0x0F, 0xB1, 0x0F}), 10);
    mem[0x50] = 0x22; mem[0x51] = 0x22;   // mismatch -> no store
    cpu->eax = 0x1111;
    EXPECT_EQ(runCycles({0x0F, 0xB1, 0x0F}), 7);
}

TEST_F(Cpu80486TimingTest, MovzxMovsxAndSetccChargeTheirPublishedCosts) {
    cpu->ebx = 0x50;
    EXPECT_EQ(runCycles({0x0F, 0xB6, 0xC3}), 3);  // MOVZX AX,BL
    EXPECT_EQ(runCycles({0x0F, 0xBE, 0xC3}), 3);  // MOVSX AX,BL
    // Published SETcc, "cycles are for: true/false": r8 4/3, mem8 3/4.
    cpu->set_flag(FLAG_ZF, true);
    EXPECT_EQ(runCycles({0x0F, 0x94, 0xC3}), 4);  // SETZ BL, condition true
    cpu->set_flag(FLAG_ZF, false);
    EXPECT_EQ(runCycles({0x0F, 0x94, 0xC3}), 3);  // SETZ BL, condition false
    EXPECT_EQ(runCycles({0x0F, 0x94, 0x07}), 4);  // SETZ [BX], condition false
}

TEST_F(Cpu80486TimingTest, BitScanCostTracksHowManyBitsWereExamined) {
    // Published BSF 6-42 / BSR 6-103 for a register source; this core
    // charges the floor plus one clock per bit examined (cpu80486.cpp).
    cpu->ebx = 0x0100;
    EXPECT_EQ(runCycles({0x0F, 0xBC, 0xC3}), 6 + 9);   // BSF AX,BX -- bits 0..8 examined
    EXPECT_EQ(runCycles({0x0F, 0xBD, 0xC3}), 6 + 8);   // BSR AX,BX -- bits 15..8 examined
    cpu->ebx = 0;
    EXPECT_EQ(runCycles({0x0F, 0xBC, 0xC3}), 6 + 16);  // zero source -- the whole width scanned
}

TEST_F(Cpu80486TimingTest, BitTestGroupSplitsReadOnlyFromReadModifyWrite) {
    cpu->ebx = 0x50;
    EXPECT_EQ(runCycles({0x0F, 0xBA, 0xE3, 0x05}), 3);   // BT BX,5
    EXPECT_EQ(runCycles({0x0F, 0xBA, 0xEB, 0x05}), 6);   // BTS BX,5
    EXPECT_EQ(runCycles({0x0F, 0xBA, 0x27, 0x05}), 3);   // BT [BX],5
    EXPECT_EQ(runCycles({0x0F, 0xBA, 0x2F, 0x05}), 8);   // BTS [BX],5
    EXPECT_EQ(runCycles({0x0F, 0xA3, 0xCB}), 3);         // BT BX,CX
    EXPECT_EQ(runCycles({0x0F, 0xA3, 0x0F}), 8);         // BT [BX],CX
    EXPECT_EQ(runCycles({0x0F, 0xAB, 0x0F}), 13);        // BTS [BX],CX
}

TEST_F(Cpu80486TimingTest, SegmentRegisterMovesAndFarPointerLoads) {
    cpu->ebx = 0x50;
    EXPECT_EQ(runCycles({0x8E, 0xD8}), 3);        // MOV DS,AX
    EXPECT_EQ(runCycles({0x8C, 0xD8}), 3);        // MOV AX,DS
    EXPECT_EQ(runCycles({0xC4, 0x07}), 6);        // LES AX,[BX]
    EXPECT_EQ(runCycles({0x0F, 0xB4, 0x07}), 6);  // LFS AX,[BX]
    EXPECT_EQ(runCycles({0x0F, 0xA0}), 3);        // PUSH FS
}

// --- Misc single instructions --------------------------------------------

TEST_F(Cpu80486TimingTest, MiscInstructionsChargeTheirPublishedCosts) {
    cpu->ebx = 0x50;
    EXPECT_EQ(runCycles({0x90}), 1);        // NOP
    EXPECT_EQ(runCycles({0xF4}), 4);        // HLT
    cpu->halted = false;                    // HLT really does halt -- clear it before going on
    EXPECT_EQ(runCycles({0x93}), 3);        // XCHG AX,BX
    EXPECT_EQ(runCycles({0x87, 0x07}), 5);  // XCHG AX,[BX]
    EXPECT_EQ(runCycles({0x98}), 3);        // CBW
    EXPECT_EQ(runCycles({0x99}), 3);        // CWD
    EXPECT_EQ(runCycles({0xD7}), 4);        // XLAT
    EXPECT_EQ(runCycles({0x27}), 2);        // DAA
    EXPECT_EQ(runCycles({0x37}), 3);        // AAA
    EXPECT_EQ(runCycles({0xD5, 0x0A}), 14); // AAD
    EXPECT_EQ(runCycles({0x62, 0x07}), 7);  // BOUND AX,[BX]
    EXPECT_EQ(runCycles({0x9B}), 1);        // WAIT
}

TEST_F(Cpu80486TimingTest, AamChargesFifteenAndIsNotDerivedFromTheDivideCost) {
    cpu->eax = 0x0005;
    EXPECT_EQ(runCycles({0xD4, 0x0A}), 15);  // AAM
}

TEST_F(Cpu80486TimingTest, HaltedCpuIdlesAtThePublishedHltCost) {
    cpu->halted = true;
    EXPECT_EQ(cpu->step(), 4);
}

TEST_F(Cpu80486TimingTest, EachPrefixByteCostsOneClock) {
    // The 486 column publishes a cost only for LOCK (1); cpu80486.cpp
    // applies that same figure uniformly to the segment-override,
    // operand-size and address-size prefixes, which the 486 decodes one
    // per clock.
    EXPECT_EQ(runCycles({0x89, 0xD8}), 1);                    // MOV AX,BX
    EXPECT_EQ(runCycles({0x66, 0x89, 0xD8}), 2);              // MOV EAX,EBX
    EXPECT_EQ(runCycles({0x26, 0x8A, 0x07}), 2);              // ES: MOV AL,[BX]
    EXPECT_EQ(runCycles({0x64, 0x8A, 0x07}), 2);              // FS: MOV AL,[BX]
    EXPECT_EQ(runCycles({0xF0, 0x01, 0xD8}), 2);              // LOCK ADD AX,BX
    EXPECT_EQ(runCycles({0x66, 0x67, 0x8B, 0x05, 0, 0, 0, 0}), 3);  // two prefixes + MOV
}

// --- Mode and descriptor-table management, real mode ---------------------
// These are the members of the group that are legal in real mode, and their
// published costs are unchanged now that they do real work (Milestone 2)
// rather than being documented no-ops (Milestone 1).

TEST_F(Cpu80486TimingTest, ModeAndDescriptorTableInstructionsChargeTheirPublishedCosts) {
    cpu->eax = 0;   // so LMSW/MOV CR0 do not actually leave real mode here
    EXPECT_EQ(runCycles({0x0F, 0x01, 0x16, 0x00, 0x02}), 11);  // LGDT [0200h]
    EXPECT_EQ(runCycles({0x0F, 0x01, 0x1E, 0x00, 0x02}), 11);  // LIDT [0200h]
    EXPECT_EQ(runCycles({0x0F, 0x01, 0x06, 0x00, 0x02}), 10);  // SGDT [0200h]
    EXPECT_EQ(runCycles({0x0F, 0x01, 0x0E, 0x00, 0x02}), 10);  // SIDT [0200h]
    EXPECT_EQ(runCycles({0x0F, 0x01, 0xE0}), 2);               // SMSW AX
    EXPECT_EQ(runCycles({0x0F, 0x01, 0x26, 0x00, 0x02}), 3);   // SMSW [0200h]
    EXPECT_EQ(runCycles({0x0F, 0x01, 0xF0}), 13);              // LMSW AX
    EXPECT_EQ(runCycles({0x0F, 0x06}), 7);                     // CLTS
    EXPECT_EQ(runCycles({0x0F, 0x08}), 4);                     // INVD
    EXPECT_EQ(runCycles({0x0F, 0x09}), 5);                     // WBINVD
    EXPECT_EQ(runCycles({0x0F, 0x01, 0x3E, 0x00, 0x02}), 12);  // INVLPG [0200h]
    EXPECT_EQ(runCycles({0x0F, 0x20, 0xC0}), 4);               // MOV EAX,CR0
    EXPECT_EQ(runCycles({0x0F, 0x22, 0xC0}), 16);              // MOV CR0,EAX
    EXPECT_EQ(runCycles({0x0F, 0x20, 0xD8}), 4);               // MOV EAX,CR3
    EXPECT_EQ(runCycles({0x0F, 0x22, 0xD8}), 4);               // MOV CR3,EAX
    EXPECT_EQ(runCycles({0x0F, 0x21, 0xC0}), 10);              // MOV EAX,DR0
    EXPECT_EQ(runCycles({0x0F, 0x23, 0xC0}), 11);              // MOV DR0,EAX
}

TEST_F(Cpu80486TimingTest, AProtectedModeOnlyOpcodeInRealModeCostsItsFaultDelivery) {
    // LAR/LSL/ARPL are #UD in real mode, and a fault is not free: the
    // real-mode vectoring work is charged at INT3's published 26, the same
    // figure interrupt() uses (cpu80486.cpp's deliver_fault).
    EXPECT_EQ(runCycles({0x0F, 0x02, 0xC3}), 26);   // LAR
    EXPECT_EQ(runCycles({0x0F, 0x03, 0xC3}), 26);   // LSL
    EXPECT_EQ(runCycles({0x63, 0xC3}), 26);         // ARPL
}

// --- Protected-mode costs ------------------------------------------------
// A separate fixture, because the protected-mode-only instructions and the
// protected-mode rows of the control-transfer entries can only be reached
// from inside protected mode. Entry is the real instruction sequence, so the
// cost of the *entry* is covered too.

class Cpu80486PmTimingTest : public Cpu80486TimingTest {
protected:
    static constexpr uint32_t kGdtPtr = 0x0500;
    static constexpr uint32_t kGdt    = 0x1000;
    static constexpr uint32_t kTss    = 0x2800;
    static constexpr uint32_t kPre    = 0x3000;
    static constexpr uint16_t kCode32 = 0x08;
    static constexpr uint16_t kData32 = 0x10;
    static constexpr uint16_t kTssSel = 0x20;
    static constexpr uint16_t kLdtSel = 0x40;
    uint32_t pm_code_ = 0;

    void w8(uint32_t a, uint8_t v) { mem[a] = v; }
    void w16(uint32_t a, uint16_t v) { w8(a, uint8_t(v)); w8(a + 1, uint8_t(v >> 8)); }
    void w32(uint32_t a, uint32_t v) { w16(a, uint16_t(v)); w16(a + 2, uint16_t(v >> 16)); }
    void w64(uint32_t a, uint64_t v) { w32(a, uint32_t(v)); w32(a + 4, uint32_t(v >> 32)); }
    static uint64_t seg_desc(uint32_t base, uint32_t limit, uint8_t access, bool big, bool gran) {
        uint32_t lim = gran ? (limit >> 12) : limit;
        uint64_t d = uint64_t(lim & 0xFFFFu);
        d |= uint64_t(base & 0xFFFFu) << 16;
        d |= uint64_t((base >> 16) & 0xFFu) << 32;
        d |= uint64_t(access) << 40;
        d |= uint64_t((lim >> 16) & 0x0Fu) << 48;
        if (big) d |= uint64_t(1) << 54;
        if (gran) d |= uint64_t(1) << 55;
        d |= uint64_t((base >> 24) & 0xFFu) << 56;
        return d;
    }

    void enter_pm32() {
        w64(kGdt + 0x00, 0);
        w64(kGdt + kCode32, seg_desc(0, 0xFFFFFFFFu, 0x9A, true, true));
        w64(kGdt + kData32, seg_desc(0, 0xFFFFFFFFu, 0x92, true, true));
        w64(kGdt + kTssSel, seg_desc(kTss, 0x67, 0x89, false, false));
        w64(kGdt + kLdtSel, seg_desc(0x1800, 0xFF, 0x82, false, false));
        w16(kGdtPtr, 0x7F);
        w32(kGdtPtr + 2, kGdt);
        for (uint32_t i = 0; i < 104; i += 4) w32(kTss + i, 0);
        std::vector<uint8_t> e;
        auto eb = [&](std::initializer_list<int> v) { for (int x : v) e.push_back(uint8_t(x)); };
        auto ew = [&](uint32_t v) { e.push_back(uint8_t(v)); e.push_back(uint8_t(v >> 8)); };
        auto ed = [&](uint32_t v) { ew(v & 0xFFFF); ew(v >> 16); };
        eb({0x0F, 0x01, 0x16}); ew(kGdtPtr);
        eb({0x0F, 0x20, 0xC0, 0x0C, 0x01, 0x0F, 0x22, 0xC0});
        eb({0x66, 0xEA}); ed(kPre); ew(kCode32);
        for (size_t i = 0; i < e.size(); ++i) w8(0x0600 + uint32_t(i), e[i]);
        std::vector<uint8_t> p;
        auto pb = [&](std::initializer_list<int> v) { for (int x : v) p.push_back(uint8_t(x)); };
        auto pw = [&](uint32_t v) { p.push_back(uint8_t(v)); p.push_back(uint8_t(v >> 8)); };
        pb({0x66, 0xB8}); pw(kData32);
        pb({0x8E, 0xD8, 0x8E, 0xC0, 0x8E, 0xD0});
        pb({0xBC}); pw(0x7F00); pw(0x0000);
        for (size_t i = 0; i < p.size(); ++i) w8(kPre + uint32_t(i), p[i]);
        pm_code_ = kPre + uint32_t(p.size());
        cpu->cs = 0;
        cpu->eip = 0x0600;
        for (int i = 0; i < 10; ++i) cpu->step();
    }
    // Assembles at pm_code_ and returns what step() charged for one instruction.
    int pmCycles(std::initializer_list<uint8_t> code) {
        uint32_t a = pm_code_;
        for (uint8_t b : code) w8(a++, b);
        cpu->halted = false;
        cpu->eip = pm_code_;
        return cpu->step();
    }
};

TEST_F(Cpu80486PmTimingTest, ProtectedModeOnlyOpcodesChargeTheirPublishedCosts) {
    enter_pm32();
    ASSERT_TRUE(cpu->protected_mode());
    EXPECT_EQ(pmCycles({0x0F, 0x00, 0xC0}), 2);    // SLDT AX
    EXPECT_EQ(pmCycles({0x0F, 0x00, 0xC8}), 2);    // STR AX
    EXPECT_EQ(pmCycles({0x0F, 0x00, 0x06, 0x00, 0x02, 0x00, 0x00}), 3);  // SLDT [0200h]
    cpu->eax = kLdtSel;
    EXPECT_EQ(pmCycles({0x0F, 0x00, 0xD0}), 11);   // LLDT AX
    cpu->eax = kTssSel;
    EXPECT_EQ(pmCycles({0x0F, 0x00, 0xD8}), 20);   // LTR AX
    cpu->eax = kData32;
    EXPECT_EQ(pmCycles({0x0F, 0x00, 0xE0}), 11);   // VERR AX
    EXPECT_EQ(pmCycles({0x0F, 0x00, 0xE8}), 11);   // VERW AX
    EXPECT_EQ(pmCycles({0x0F, 0x02, 0xD8}), 11);   // LAR EBX,EAX
    EXPECT_EQ(pmCycles({0x0F, 0x03, 0xD8}), 10);   // LSL EBX,EAX
    EXPECT_EQ(pmCycles({0x63, 0xC3}), 9);          // ARPL BX,AX
}

TEST_F(Cpu80486PmTimingTest, FarControlTransfersChargeTheirProtectedModeRows) {
    enter_pm32();
    // The 486 column carries separate real-mode and protected-mode rows for
    // these: JMP far 17 real / 19 protected, CALL far 18 / 20, RET far 13.
    // A plain segment-load transfer is the cheap case; a call gate and a task
    // switch are an order of magnitude more.
    w8(0x6000, 0xF4);   // HLT at the far-jump target, so nothing runs on
    EXPECT_EQ(pmCycles({0xEA, 0x00, 0x60, 0x00, 0x00, uint8_t(kCode32), 0x00}), 19);
    EXPECT_EQ(pmCycles({0x9A, 0x00, 0x60, 0x00, 0x00, uint8_t(kCode32), 0x00}), 20);
    // The CALL above left a return frame on the stack, so RETF has one to pop.
    EXPECT_EQ(pmCycles({0xCB}), 13);
}

TEST_F(Cpu80486PmTimingTest, ATaskSwitchIsTheMostExpensiveOperationInTheInstructionSet) {
    enter_pm32();
    // Charged as one labelled figure for every task-switch path rather than
    // asserting a cited split this core cannot separate (cpu80486.cpp).
    w64(kGdt + 0x28, seg_desc(0x2900, 0x67, 0x89, false, false));
    for (uint32_t i = 0; i < 104; i += 4) w32(0x2900 + i, 0);
    w8(0x6000, 0xF4);
    w32(0x2900 + 32, 0x6000);        // EIP
    w32(0x2900 + 36, 0x00000202u);   // EFLAGS
    w32(0x2900 + 56, 0x00007A00u);   // ESP
    w32(0x2900 + 72, kData32);
    w32(0x2900 + 76, kCode32);
    w32(0x2900 + 80, kData32);
    w32(0x2900 + 84, kData32);
    cpu->eax = kTssSel;
    pmCycles({0x0F, 0x00, 0xD8});    // LTR
    EXPECT_EQ(pmCycles({0xEA, 0x00, 0x00, 0x00, 0x00, 0x28, 0x00}), 199);
}

TEST_F(Cpu80486PmTimingTest, AProtectedModeFaultDeliveryCostsItsGateWork) {
    enter_pm32();
    // Published INT through a protected-mode gate at the same privilege level
    // is 44, against real mode's 26 -- the descriptor work is the difference.
    w16(0x0500, 0x07FF);
    w32(0x0502, 0x1100);
    for (uint32_t v = 0; v < 32; ++v) w64(0x1100 + v * 8, 0);
    w8(0x6000, 0xF4);
    // A 32-bit interrupt gate for #UD.
    w64(0x1100 + 6 * 8, (uint64_t(0x6000 & 0xFFFFu)) | (uint64_t(kCode32) << 16) |
                        (uint64_t(0x8E) << 40) | (uint64_t(0x6000 >> 16) << 48));
    pmCycles({0x0F, 0x01, 0x1D, 0x00, 0x05, 0x00, 0x00});   // LIDT [0500h]
    EXPECT_EQ(pmCycles({0x8E, 0xC8}), 44);   // MOV CS,AX -> #UD through the gate
}

// --- x87 FPU costs -------------------------------------------------------
// The FPU rows of the same timing table. Where the table prints a range the
// floor is charged and the data-dependence is deliberately not modelled:
// unlike the integer MUL/BSF/RCL cases, Intel documents no mechanism for
// these ranges that could be fitted to both endpoints (cpu80486.cpp).

TEST_F(Cpu80486TimingTest, FpuLoadsAndStoresChargeTheirPublishedCosts) {
    EXPECT_EQ(runCycles({0xDB, 0xE3}), 17);              // FNINIT
    EXPECT_EQ(runCycles({0xD9, 0x06, 0x00, 0x02}), 3);   // FLD m32real
    EXPECT_EQ(runCycles({0xDD, 0x06, 0x00, 0x02}), 3);   // FLD m64real
    EXPECT_EQ(runCycles({0xDB, 0x2E, 0x00, 0x02}), 6);   // FLD m80real
    EXPECT_EQ(runCycles({0xD9, 0xC1}), 4);               // FLD ST(1)
    EXPECT_EQ(runCycles({0xD9, 0x16, 0x00, 0x02}), 7);   // FST m32real
    EXPECT_EQ(runCycles({0xDD, 0x16, 0x00, 0x02}), 8);   // FST m64real
    EXPECT_EQ(runCycles({0xDB, 0x3E, 0x00, 0x02}), 6);   // FSTP m80real
    EXPECT_EQ(runCycles({0xDD, 0xD1}), 3);               // FST ST(1)
    EXPECT_EQ(runCycles({0xDB, 0x06, 0x00, 0x02}), 13);  // FILD m32int
    EXPECT_EQ(runCycles({0xDB, 0x1E, 0x00, 0x02}), 29);  // FISTP m32int
    EXPECT_EQ(runCycles({0xDF, 0x26, 0x00, 0x02}), 75);  // FBLD m80bcd
    EXPECT_EQ(runCycles({0xDF, 0x36, 0x00, 0x02}), 175); // FBSTP m80bcd
}

TEST_F(Cpu80486TimingTest, FpuArithmeticCostsDifferByAnOrderOfMagnitude) {
    // This is the whole reason the FPU needs its own cost entries: a divide
    // is nine times an add, and a square root ten times. A flat per-ESC
    // figure would make period floating-point benchmarks read wildly wrong,
    // the same failure mode the integer MUL/DIV split exists to avoid.
    EXPECT_EQ(runCycles({0xD8, 0xC1}), 8);    // FADD ST,ST(1)
    EXPECT_EQ(runCycles({0xD8, 0xC9}), 16);   // FMUL ST,ST(1)
    EXPECT_EQ(runCycles({0xD8, 0xE1}), 8);    // FSUB ST,ST(1)
    EXPECT_EQ(runCycles({0xD8, 0xF1}), 73);   // FDIV ST,ST(1)
    EXPECT_EQ(runCycles({0xD9, 0xFA}), 83);   // FSQRT
    // A memory operand costs the same as a register one: the table gives one
    // figure for "FADD ST(i),ST / m32real / m64real", so the load is already
    // inside it.
    EXPECT_EQ(runCycles({0xD8, 0x06, 0x00, 0x02}), 8);   // FADD m32real
    EXPECT_EQ(runCycles({0xDC, 0x06, 0x00, 0x02}), 8);   // FADD m64real
    EXPECT_EQ(runCycles({0xD8, 0x0E, 0x00, 0x02}), 16);  // FMUL m32real
    EXPECT_EQ(runCycles({0xD8, 0x16, 0x00, 0x02}), 4);   // FCOM m32real
    EXPECT_EQ(runCycles({0xD9, 0xE4}), 4);               // FTST
}

TEST_F(Cpu80486TimingTest, FpuControlAndTranscendentalCosts) {
    EXPECT_EQ(runCycles({0xD9, 0x2E, 0x00, 0x02}), 4);   // FLDCW
    EXPECT_EQ(runCycles({0xD9, 0x3E, 0x00, 0x02}), 3);   // FNSTCW
    EXPECT_EQ(runCycles({0xDF, 0xE0}), 3);               // FNSTSW AX
    EXPECT_EQ(runCycles({0xDB, 0xE2}), 7);               // FNCLEX
    EXPECT_EQ(runCycles({0xD9, 0xD0}), 3);               // FNOP
    EXPECT_EQ(runCycles({0xD9, 0xE0}), 6);               // FCHS
    EXPECT_EQ(runCycles({0xD9, 0xE1}), 3);               // FABS
    EXPECT_EQ(runCycles({0xD9, 0xC9}), 4);               // FXCH ST(1)
    EXPECT_EQ(runCycles({0xD9, 0xE5}), 8);               // FXAM
    EXPECT_EQ(runCycles({0xD9, 0xE8}), 8);               // FLD1
    EXPECT_EQ(runCycles({0xD9, 0xF7}), 3);               // FINCSTP
    EXPECT_EQ(runCycles({0xDD, 0xC1}), 3);               // FFREE ST(1)
    EXPECT_EQ(runCycles({0xD9, 0xFC}), 21);              // FRNDINT
    EXPECT_EQ(runCycles({0xD9, 0xFD}), 30);              // FSCALE
    EXPECT_EQ(runCycles({0xD9, 0xF8}), 70);              // FPREM
    EXPECT_EQ(runCycles({0xD9, 0xF4}), 16);              // FXTRACT
    EXPECT_EQ(runCycles({0xD9, 0xF0}), 140);             // F2XM1
    EXPECT_EQ(runCycles({0xD9, 0xF1}), 196);             // FYL2X
    EXPECT_EQ(runCycles({0xD9, 0xF2}), 200);             // FPTAN
    EXPECT_EQ(runCycles({0xD9, 0xF3}), 218);             // FPATAN
    EXPECT_EQ(runCycles({0xD9, 0xFE}), 257);             // FSIN
    EXPECT_EQ(runCycles({0xD9, 0xFB}), 292);             // FSINCOS
    EXPECT_EQ(runCycles({0xD9, 0x36, 0x00, 0x02}), 67);  // FNSTENV
    EXPECT_EQ(runCycles({0xD9, 0x26, 0x00, 0x02}), 44);  // FLDENV
    EXPECT_EQ(runCycles({0xDD, 0x36, 0x00, 0x02}), 154); // FNSAVE
    EXPECT_EQ(runCycles({0xDD, 0x26, 0x00, 0x02}), 131); // FRSTOR
    EXPECT_EQ(runCycles({0x9B}), 1);                     // FWAIT -- published 1-3
}

}  // namespace
