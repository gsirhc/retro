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
using Flag = cpu80286::Flag;

class Cpu80286Test : public ::testing::Test {
protected:
    std::array<uint8_t, 0x100000> mem{};  // 1MB flat, enough for real-mode addressing
    std::unique_ptr<Cpu> cpu;
    uint8_t last_out_port_val = 0;
    uint16_t last_out_port = 0;
    uint8_t next_in_val = 0xFF;
    uint16_t last_out16_port = 0;
    uint16_t last_out16_val = 0;
    uint16_t next_in16_val = 0xFFFF;

    void SetUp() override {
        Bus bus;
        bus.read  = [this](uint32_t a) { return mem[a & 0xFFFFF]; };
        bus.write = [this](uint32_t a, uint8_t v) { mem[a & 0xFFFFF] = v; };
        bus.in    = [this](uint16_t) -> uint8_t { return next_in_val; };
        bus.out   = [this](uint16_t p, uint8_t v) { last_out_port = p; last_out_port_val = v; };
        // Atomic 16-bit port access, so tests can prove IN AX,DX / OUT DX,AX do not split into two 8-bit accesses.
        bus.in16  = [this](uint16_t) -> uint16_t { return next_in16_val; };
        bus.out16 = [this](uint16_t p, uint16_t v) { last_out16_port = p; last_out16_val = v; };
        cpu = std::make_unique<Cpu>(bus);
        cpu->reset();
        cpu->cs = 0;
        cpu->ip = 0;
    }

    void load(std::initializer_list<uint8_t> code, uint16_t at = 0) {
        uint16_t addr = at;
        for (uint8_t b : code) mem[addr++] = b;
    }
    // Assembles `code` at CS:0 and executes one instruction.
    void run(std::initializer_list<uint8_t> code) {
        load(code);
        cpu->ip = 0;
        cpu->step();
    }
    // Assembles `code` at CS:0 and executes `n` instructions.
    void runN(std::initializer_list<uint8_t> code, int n) {
        load(code);
        cpu->ip = 0;
        for (int i = 0; i < n; ++i) cpu->step();
    }

    bool CF() const { return cpu->flag(cpu80286::FLAG_CF); }
    bool ZF() const { return cpu->flag(cpu80286::FLAG_ZF); }
    bool SF() const { return cpu->flag(cpu80286::FLAG_SF); }
    bool OF() const { return cpu->flag(cpu80286::FLAG_OF); }
    bool AF() const { return cpu->flag(cpu80286::FLAG_AF); }
    bool PF() const { return cpu->flag(cpu80286::FLAG_PF); }

    void allow_firmware() { cpu->firmware_at = [](uint32_t) { return true; }; }
    // Points vector `v` at 0700:0000.
    void handler(int v) {
        mem[v * 4 + 0] = 0x00; mem[v * 4 + 1] = 0x00;
        mem[v * 4 + 2] = 0x00; mem[v * 4 + 3] = 0x07;
    }
    bool in_handler() const { return cpu->cs == 0x0700 && cpu->ip == 0; }
    // Runs `code` at 0100:0000, clear of the IVT, with the stack at 0000:8000.
    void exec(std::initializer_list<uint8_t> code, int n = 1) {
        uint32_t a = 0x1000;
        for (uint8_t b : code) mem[a++] = b;
        cpu->cs = 0x0100;
        cpu->ip = 0;
        cpu->ss = 0;
        cpu->sp = 0x8000;
        for (int i = 0; i < n; ++i) cpu->step();
    }
    // Word i of the interrupt frame: 0 = IP, 1 = CS, 2 = FLAGS.
    uint16_t frame(int i) const {
        uint32_t a = uint32_t(cpu->ss) * 16 + uint16_t(cpu->sp + 2 * i);
        return uint16_t(mem[a] | (mem[a + 1] << 8));
    }
};

// ---------------------------------------------------------------------------
// MOV / data movement
// ---------------------------------------------------------------------------

TEST_F(Cpu80286Test, MovRegImm16) {
    run({0xB8, 0x34, 0x12});  // MOV AX, 1234h
    EXPECT_EQ(cpu->ax, 0x1234);
}

TEST_F(Cpu80286Test, MovRegImm8) {
    run({0xB3, 0x42});  // MOV BL, 42h
    EXPECT_EQ(cpu->bx & 0xFF, 0x42);
}

TEST_F(Cpu80286Test, MovRegToRegRegisterForm) {
    cpu->ax = 0x0099;  // AL = 0x99
    run({0x88, 0xC3});  // MOV BL, AL  (modrm: mod=11 reg=AL(0) rm=BX(3))
    EXPECT_EQ(cpu->bx & 0xFF, 0x99);
}

TEST_F(Cpu80286Test, MovMemDirectAddressing) {
    mem[0x0200] = 0x78;
    mem[0x0201] = 0x56;
    run({0xA1, 0x00, 0x02});  // MOV AX, [0200h]
    EXPECT_EQ(cpu->ax, 0x5678);
}

TEST_F(Cpu80286Test, LeaLoadsEffectiveAddressNotContents) {
    mem[0x1234] = 0xAA;  // decoy -- LEA must not read memory contents
    run({0x8D, 0x06, 0x34, 0x12});  // LEA AX, [1234h]
    EXPECT_EQ(cpu->ax, 0x1234);
}

TEST_F(Cpu80286Test, XchgAxReg) {
    cpu->ax = 0x1111;
    cpu->bx = 0x2222;
    run({0x93});  // XCHG AX, BX
    EXPECT_EQ(cpu->ax, 0x2222);
    EXPECT_EQ(cpu->bx, 0x1111);
}

TEST_F(Cpu80286Test, SegmentOverridePrefixRedirectsMemoryAccess) {
    cpu->es = 0x1000;
    cpu->bx = 0;
    mem[(0x1000u << 4) + 0] = 0x55;  // ES:[BX]
    mem[0] = 0x11;                   // DS:[BX] (decoy, DS=0 after reset)
    run({0x26, 0x8A, 0x07});  // ES: MOV AL, [BX]
    EXPECT_EQ(cpu->ax & 0xFF, 0x55);
}

// ---------------------------------------------------------------------------
// ALU group (fast-path 0x00-0x3D block)
// ---------------------------------------------------------------------------

TEST_F(Cpu80286Test, AddRegRegSetsFlags) {
    cpu->ax = 0x0001;
    cpu->bx = 0xFFFF;
    run({0x01, 0xD8});  // ADD AX, BX  -> 0x10000, wraps to 0, carry out
    EXPECT_EQ(cpu->ax, 0x0000);
    EXPECT_TRUE(ZF());
    EXPECT_TRUE(CF());
}

TEST_F(Cpu80286Test, SubRegImmBorrow) {
    cpu->ax = 0x0005;
    run({0x2D, 0x0A, 0x00});  // SUB AX, 000Ah -> -5, borrow
    EXPECT_EQ(cpu->ax, uint16_t(-5));
    EXPECT_TRUE(CF());
    EXPECT_TRUE(SF());
}

TEST_F(Cpu80286Test, CmpDoesNotModifyOperand) {
    cpu->ax = 0x0005;
    run({0x3D, 0x05, 0x00});  // CMP AX, 5 -> equal
    EXPECT_EQ(cpu->ax, 0x0005);  // unchanged
    EXPECT_TRUE(ZF());
}

TEST_F(Cpu80286Test, AndClearsCarryAndOverflow) {
    cpu->ax = 0xFF00;
    run({0x25, 0xFF, 0x00});  // AND AX, 00FFh -> 0 (masks are complementary)
    EXPECT_EQ(cpu->ax, 0x0000);
    EXPECT_TRUE(ZF());
    EXPECT_FALSE(CF());
    EXPECT_FALSE(OF());
}

TEST_F(Cpu80286Test, TestAlImmDoesNotModifyAl) {
    cpu->ax = 0x00FF;
    run({0xA8, 0x01});  // TEST AL, 1
    EXPECT_EQ(cpu->ax & 0xFF, 0xFF);
    EXPECT_FALSE(ZF());
}

TEST_F(Cpu80286Test, IncRegDoesNotAffectCarry) {
    cpu->ax = 0x00FF;  // AL = 0xFF
    cpu->set_flag(cpu80286::FLAG_CF, true);
    run({0x40});  // INC AX -> 0x0100
    EXPECT_EQ(cpu->ax, 0x0100);
    EXPECT_TRUE(CF());  // INC never touches CF -- classic 8086/286 quirk
}

TEST_F(Cpu80286Test, DecRegDoesNotAffectCarry) {
    cpu->ax = 0x0000;
    cpu->set_flag(cpu80286::FLAG_CF, false);
    run({0x48});  // DEC AX -> 0xFFFF
    EXPECT_EQ(cpu->ax, 0xFFFF);
    EXPECT_FALSE(CF());
    EXPECT_TRUE(SF());
}

// ---------------------------------------------------------------------------
// Stack: PUSH/POP, and the real PUSH-SP-pushes-decremented-value quirk
// ---------------------------------------------------------------------------

TEST_F(Cpu80286Test, PushPopRoundTripRestoresValue) {
    cpu->ss = 0;
    cpu->sp = 0x1000;
    cpu->bx = 0xABCD;
    cpu->cx = 0;
    runN({0x53, 0x59}, 2);  // PUSH BX; POP CX
    EXPECT_EQ(cpu->cx, 0xABCD);
    EXPECT_EQ(cpu->sp, 0x1000);  // net zero stack movement
}

TEST_F(Cpu80286Test, PushSpPushesValueFromBeforeThePush) {
    cpu->ss = 0;
    cpu->sp = 0x2000;
    run({0x54});  // PUSH SP
    uint16_t new_sp = 0x2000 - 2;
    EXPECT_EQ(cpu->sp, new_sp);
    uint16_t pushed = mem[new_sp] | (uint16_t(mem[new_sp + 1]) << 8);
    EXPECT_EQ(pushed, 0x2000) << "the 8086 pushes the decremented value, the 286 the original";
}

TEST_F(Cpu80286Test, PushSpPopAxMatchesSpLikeA286) {
    cpu->ss = 0;
    cpu->sp = 0x2000;
    runN({0x54, 0x58}, 2);  // PUSH SP ; POP AX, the period 8086-or-286 test
    EXPECT_EQ(cpu->ax, cpu->sp);
}

TEST_F(Cpu80286Test, PushaPopaRoundTrip) {
    cpu->ax = 1; cpu->cx = 2; cpu->dx = 3; cpu->bx = 4;
    cpu->bp = 5; cpu->si = 6; cpu->di = 7;
    cpu->ss = 0;
    cpu->sp = 0x4000;
    runN({0x60}, 1);  // PUSHA
    cpu->ax = cpu->cx = cpu->dx = cpu->bx = 0;
    cpu->bp = cpu->si = cpu->di = 0;
    load({0x61}, cpu->ip);
    cpu->step();  // POPA
    EXPECT_EQ(cpu->ax, 1);
    EXPECT_EQ(cpu->cx, 2);
    EXPECT_EQ(cpu->dx, 3);
    EXPECT_EQ(cpu->bx, 4);
    EXPECT_EQ(cpu->bp, 5);
    EXPECT_EQ(cpu->si, 6);
    EXPECT_EQ(cpu->di, 7);
    EXPECT_EQ(cpu->sp, 0x4000);
}

// ---------------------------------------------------------------------------
// PUSHF/POPF/IRET: IOPL and NT are always cleared
// ---------------------------------------------------------------------------

TEST_F(Cpu80286Test, PopfAndIretAlwaysClearIoplAndNt) {
    // Real mode on a 286 can't set FLAGS bits 12-15; a 386 can (Intel AP-485).
    cpu->ss = 0;
    uint16_t want = uint16_t(cpu80286::FLAG_IOPL | cpu80286::FLAG_NT | cpu80286::FLAG_R1);

    cpu->sp = 0x2FFE;
    mem[0x2FFE] = uint8_t(want & 0xFF);
    mem[0x2FFF] = uint8_t(want >> 8);
    run({0x9D});  // POPF
    EXPECT_FALSE(cpu->flags & cpu80286::FLAG_IOPL);
    EXPECT_FALSE(cpu->flags & cpu80286::FLAG_NT);
    EXPECT_EQ(cpu->flags & 0x8000, 0) << "bit 15 stays reserved-0 on the 286, unlike the 8086 family";

    uint16_t ret_ip = 0x1234, ret_cs = 0x0050;
    cpu->sp = 0x3000 - 6;
    uint16_t base = cpu->sp;
    mem[base + 0] = uint8_t(ret_ip & 0xFF);      mem[base + 1] = uint8_t(ret_ip >> 8);
    mem[base + 2] = uint8_t(ret_cs & 0xFF);      mem[base + 3] = uint8_t(ret_cs >> 8);
    mem[base + 4] = uint8_t(want & 0xFF);        mem[base + 5] = uint8_t(want >> 8);
    run({0xCF});  // IRET
    EXPECT_EQ(cpu->ip, ret_ip);
    EXPECT_EQ(cpu->cs, ret_cs);
    EXPECT_FALSE(cpu->flags & cpu80286::FLAG_IOPL);
    EXPECT_FALSE(cpu->flags & cpu80286::FLAG_NT);
}

// ---------------------------------------------------------------------------
// Shift/rotate, including the 286 mod-32 count mask
// ---------------------------------------------------------------------------

TEST_F(Cpu80286Test, ShlByImmediate) {
    cpu->bx = 0x0001;
    run({0xC1, 0xE3, 0x04});  // SHL BX, 4   (grp2 C1, modrm C0|reg4<<3|rm3 = 0xE3)
    EXPECT_EQ(cpu->bx, 0x0010);
}

TEST_F(Cpu80286Test, ShiftCountMaskedMod32) {
    // 33 mod 32 == 1: SHL BX,33 behaves like SHL BX,1 on a 286.
    cpu->bx = 0x0001;
    run({0xC1, 0xE3, 33});
    EXPECT_EQ(cpu->bx, 0x0002);
}

TEST_F(Cpu80286Test, RolByOneSetsCarryAndOverflow) {
    cpu->bx = 0x8001;
    run({0xD1, 0xC3});  // ROL BX, 1  (grp2 D1, reg=0/ROL, rm=BX -> modrm C0|0|3=0xC3)
    EXPECT_EQ(cpu->bx, 0x0003);
    EXPECT_TRUE(CF());  // bit 15 (1) rotated into CF
}

TEST_F(Cpu80286Test, ShrSetsOverflowFromOriginalMsb) {
    cpu->bx = 0x8000;
    run({0xD1, 0xEB});  // SHR BX, 1  (reg=5/SHR, rm=BX -> modrm C0|(5<<3)|3=0xEB)
    EXPECT_EQ(cpu->bx, 0x4000);
    EXPECT_TRUE(OF());  // OF = MSB of the *original* operand for SHR,1
}

// ---------------------------------------------------------------------------
// Control flow: Jcc, LOOP, CALL/RET
// ---------------------------------------------------------------------------

TEST_F(Cpu80286Test, JccNear0FEncodingTakenWhenZero) {
    allow_firmware();
    // 0x0F 0x8x (Jcc rel16) is an 80386 addition the BIOS substitute needs.
    cpu->set_flag(cpu80286::FLAG_ZF, true);
    run({0x0F, 0x84, 0x05, 0x00});  // JZ near +5
    EXPECT_EQ(cpu->ip, 4 + 5);
}

TEST_F(Cpu80286Test, JccNear0FEncodingNotTakenWhenNotZero) {
    allow_firmware();
    cpu->set_flag(cpu80286::FLAG_ZF, false);
    run({0x0F, 0x84, 0x05, 0x00});
    EXPECT_EQ(cpu->ip, 4);
}

TEST_F(Cpu80286Test, JzTakenWhenZero) {
    cpu->set_flag(cpu80286::FLAG_ZF, true);
    run({0x74, 0x05});  // JZ +5
    EXPECT_EQ(cpu->ip, 2 + 5);
}

TEST_F(Cpu80286Test, JzNotTakenWhenNotZero) {
    cpu->set_flag(cpu80286::FLAG_ZF, false);
    run({0x74, 0x05});
    EXPECT_EQ(cpu->ip, 2);
}

TEST_F(Cpu80286Test, LoopDecrementsCxAndBranchesUntilZero) {
    cpu->cx = 3;
    cpu->ss = 0; cpu->sp = 0x1000;
    // LOOP -2 (branch back to itself) three times, then fall through.
    load({0xE2, 0xFE});
    cpu->ip = 0;
    cpu->step(); EXPECT_EQ(cpu->cx, 2); EXPECT_EQ(cpu->ip, 0);
    cpu->step(); EXPECT_EQ(cpu->cx, 1); EXPECT_EQ(cpu->ip, 0);
    cpu->step(); EXPECT_EQ(cpu->cx, 0); EXPECT_EQ(cpu->ip, 2);  // not taken, falls through
}

TEST_F(Cpu80286Test, CallNearThenRetReturnsToCaller) {
    cpu->ss = 0;
    cpu->sp = 0x1000;
    // At CS:0: CALL rel16=+2 (IP=3 after fetch, target 5). At CS:5: RET.
    load({0xE8, 0x02, 0x00}, 0);
    load({0xC3}, 5);
    cpu->ip = 0;
    cpu->step();  // CALL -> return address 3 pushed, IP jumps to 5
    EXPECT_EQ(cpu->ip, 5);
    cpu->step();  // RET at CS:5
    EXPECT_EQ(cpu->ip, 3);  // back to the byte right after CALL
    EXPECT_EQ(cpu->sp, 0x1000);
}

// ---------------------------------------------------------------------------
// String instructions with REP
// ---------------------------------------------------------------------------

TEST_F(Cpu80286Test, RepMovsbCopiesCxBytes) {
    cpu->ds = 0; cpu->es = 0;
    cpu->si = 0x0300;
    cpu->di = 0x0400;
    cpu->cx = 4;
    cpu->set_flag(cpu80286::FLAG_DF, false);
    for (int i = 0; i < 4; ++i) mem[0x0300 + i] = uint8_t(0x10 + i);
    run({0xF3, 0xA4});  // REP MOVSB
    for (int i = 0; i < 4; ++i) EXPECT_EQ(mem[0x0400 + i], 0x10 + i);
    EXPECT_EQ(cpu->cx, 0);
    EXPECT_EQ(cpu->si, 0x0304);
    EXPECT_EQ(cpu->di, 0x0404);
}

TEST_F(Cpu80286Test, StoswFillsWordAndAdvancesDi) {
    cpu->es = 0;
    cpu->di = 0x0500;
    cpu->ax = 0xBEEF;
    run({0xAB});  // STOSW
    EXPECT_EQ(mem[0x0500] | (uint16_t(mem[0x0501]) << 8), 0xBEEF);
    EXPECT_EQ(cpu->di, 0x0502);
}

TEST_F(Cpu80286Test, RepneScasbStopsOnMatch) {
    cpu->es = 0;
    cpu->di = 0x0600;
    cpu->cx = 5;
    cpu->ax = 0x0042;  // AL = 0x42, search target
    for (int i = 0; i < 5; ++i) mem[0x0600 + i] = uint8_t(0x40 + i);  // matches at index 2
    run({0xF2, 0xAE});  // REPNE SCASB
    EXPECT_EQ(cpu->di, 0x0600 + 3);  // stopped right after the matching byte
    EXPECT_EQ(cpu->cx, 5 - 3);
    EXPECT_TRUE(ZF());
}

// ---------------------------------------------------------------------------
// 286-native: BOUND, ENTER/LEAVE, IMUL, PUSH imm
// ---------------------------------------------------------------------------

TEST_F(Cpu80286Test, BoundWithinRangeDoesNotFault) {
    mem[0x0200] = 0x00; mem[0x0201] = 0x00;  // lower bound = 0
    mem[0x0202] = 0x0A; mem[0x0203] = 0x00;  // upper bound = 10
    cpu->ax = 5;
    run({0x62, 0x06, 0x00, 0x02});  // BOUND AX, [0200h]
    EXPECT_EQ(cpu->cs, 0);  // no interrupt taken -- CS unchanged
}

TEST_F(Cpu80286Test, BoundOutOfRangeTriggersInterruptFive) {
    mem[5 * 4 + 0] = 0x78; mem[5 * 4 + 1] = 0x56;  // vector 5 -> 1234:5678
    mem[5 * 4 + 2] = 0x34; mem[5 * 4 + 3] = 0x12;
    mem[0x0200] = 0x00; mem[0x0201] = 0x00;
    mem[0x0202] = 0x0A; mem[0x0203] = 0x00;
    cpu->ss = 0; cpu->sp = 0x8000;
    cpu->ax = 99;  // out of [0,10]
    run({0x62, 0x06, 0x00, 0x02});
    EXPECT_EQ(cpu->cs, 0x1234);
    EXPECT_EQ(cpu->ip, 0x5678);
}

TEST_F(Cpu80286Test, EnterLeaveRestoreFrame) {
    cpu->ss = 0;
    cpu->bp = 0x1000;
    cpu->sp = 0x2000;
    runN({0xC8, 0x04, 0x00, 0x00, 0xC9}, 2);  // ENTER 4,0 ; LEAVE
    EXPECT_EQ(cpu->bp, 0x1000);
    EXPECT_EQ(cpu->sp, 0x2000);
}

TEST_F(Cpu80286Test, EnterAllocatesLocalsBelowFrame) {
    cpu->ss = 0;
    cpu->bp = 0x1000;
    cpu->sp = 0x2000;
    run({0xC8, 0x04, 0x00, 0x00});  // ENTER 4, level 0
    uint16_t expected_bp = 0x2000 - 2;  // after pushing the caller's BP
    EXPECT_EQ(cpu->bp, expected_bp);
    EXPECT_EQ(cpu->sp, expected_bp - 4);
}

TEST_F(Cpu80286Test, ImulRegImm16) {
    cpu->bx = 6;
    // IMUL AX, BX, 7   (0x69 /r imm16; modrm reg=AX(0) rm=BX(3) -> 0xC3)
    run({0x69, 0xC3, 0x07, 0x00});
    EXPECT_EQ(cpu->ax, 42);
    EXPECT_FALSE(CF());
    EXPECT_FALSE(OF());
}

TEST_F(Cpu80286Test, PushImm16ThenPop) {
    cpu->ss = 0;
    cpu->sp = 0x3000;
    runN({0x68, 0xCD, 0xAB, 0x58}, 2);  // PUSH 0ABCDh ; POP AX
    EXPECT_EQ(cpu->ax, 0xABCD);
    EXPECT_EQ(cpu->sp, 0x3000);
}

// ---------------------------------------------------------------------------
// Multiply / divide (grp3, 0xF6/0xF7)
// ---------------------------------------------------------------------------

TEST_F(Cpu80286Test, MulByteNoOverflow) {
    cpu->ax = 6;   // AL = 6
    cpu->bx = 7;   // BL = 7
    run({0xF6, 0xE3});  // MUL BL  (grp3 reg=4 rm=BX -> modrm C0|(4<<3)|3=0xE3)
    EXPECT_EQ(cpu->ax, 42);
    EXPECT_FALSE(CF());
    EXPECT_FALSE(OF());
}

TEST_F(Cpu80286Test, DivByteExactQuotient) {
    cpu->ax = 10;  // dividend
    cpu->bx = 3;   // BL = divisor
    run({0xF6, 0xF3});  // DIV BL  (reg=6 rm=BX -> modrm C0|(6<<3)|3=0xF3)
    EXPECT_EQ(cpu->ax & 0xFF, 3);          // AL = quotient
    EXPECT_EQ((cpu->ax >> 8) & 0xFF, 1);   // AH = remainder
}

TEST_F(Cpu80286Test, DivByZeroFaultsThroughVectorZero) {
    // Vector 0 sits at physical 0, so the DIV goes at CS:0x100 to stay clear of the IVT.
    mem[0] = 0x11; mem[1] = 0x11; mem[2] = 0x22; mem[3] = 0x22;  // vector 0 -> 2222:1111
    cpu->ss = 0; cpu->sp = 0x9000;
    cpu->ax = 10;
    cpu->bx = 0;
    load({0xF6, 0xF3}, 0x100);  // DIV BL, BL==0
    cpu->ip = 0x100;
    cpu->step();
    EXPECT_EQ(cpu->cs, 0x2222);
    EXPECT_EQ(cpu->ip, 0x1111);
}

// ---------------------------------------------------------------------------
// INT / IRET
// ---------------------------------------------------------------------------

TEST_F(Cpu80286Test, IntThenIretRestoresFlagsCsIp) {
    mem[0x21 * 4 + 0] = 0x00; mem[0x21 * 4 + 1] = 0x10;  // vector 21h -> 2000:1000
    mem[0x21 * 4 + 2] = 0x00; mem[0x21 * 4 + 3] = 0x20;
    cpu->ss = 0; cpu->sp = 0xA000;
    cpu->set_flag(cpu80286::FLAG_IF, true);
    load({0xCD, 0x21}, 0);                          // at CS:0 -- INT 21h
    mem[(0x2000u << 4) + 0x1000] = 0xCF;             // at 2000:1000 -- IRET
    cpu->ip = 0;
    cpu->step();  // INT 21h
    EXPECT_EQ(cpu->cs, 0x2000);
    EXPECT_EQ(cpu->ip, 0x1000);
    EXPECT_FALSE(cpu->flag(cpu80286::FLAG_IF));  // IF cleared by INT
    cpu->step();  // IRET
    EXPECT_EQ(cpu->cs, 0);
    EXPECT_EQ(cpu->ip, 2);  // back to right after the 2-byte INT 21h
    EXPECT_TRUE(cpu->flag(cpu80286::FLAG_IF));  // IF restored from the pushed FLAGS
    EXPECT_EQ(cpu->sp, 0xA000);
}

// ---------------------------------------------------------------------------
// IN/OUT port I/O
// ---------------------------------------------------------------------------

TEST_F(Cpu80286Test, InAlImmReadsPort) {
    next_in_val = 0x99;
    run({0xE4, 0x60});  // IN AL, 60h
    EXPECT_EQ(cpu->ax & 0xFF, 0x99);
}

TEST_F(Cpu80286Test, OutImmAlWritesPort) {
    cpu->ax = 0x00AB;
    run({0xE6, 0x61});  // OUT 61h, AL
    EXPECT_EQ(last_out_port, 0x61);
    EXPECT_EQ(last_out_port_val, 0xAB);
}

TEST_F(Cpu80286Test, InAxImmGoesThroughAtomicSixteenBitPath) {
    // IN AX,imm8 must use Bus::in16, not two Bus::in calls.
    next_in16_val = 0x1234;
    run({0xE5, 0x60});  // IN AX, 60h
    EXPECT_EQ(cpu->ax & 0xFFFF, 0x1234);
}

TEST_F(Cpu80286Test, OutAxImmGoesThroughAtomicSixteenBitPath) {
    cpu->ax = 0xBEEF;
    run({0xE7, 0x60});  // OUT 60h, AX
    EXPECT_EQ(last_out16_port, 0x60);
    EXPECT_EQ(last_out16_val, 0xBEEF);
}

TEST_F(Cpu80286Test, InAxDxGoesThroughAtomicSixteenBitPath) {
    cpu->dx = 0x1F0;
    next_in16_val = 0x55AA;
    run({0xED});  // IN AX, DX
    EXPECT_EQ(cpu->ax & 0xFFFF, 0x55AA);
}

TEST_F(Cpu80286Test, OutDxAxGoesThroughAtomicSixteenBitPath) {
    cpu->dx = 0x1F0;
    cpu->ax = 0xCAFE;
    run({0xEF});  // OUT DX, AX
    EXPECT_EQ(last_out16_port, 0x1F0);
    EXPECT_EQ(last_out16_val, 0xCAFE);
}

TEST_F(Cpu80286Test, InswStoresAtomicSixteenBitPortReadAtEsDi) {
    cpu->dx = 0x1F0;
    cpu->es = 0; cpu->di = 0x0500;
    cpu->set_flag(cpu80286::FLAG_DF, false);
    next_in16_val = 0xABCD;
    run({0x6D});  // INSW
    EXPECT_EQ(mem[0x0500], 0xCD);
    EXPECT_EQ(mem[0x0501], 0xAB);
    EXPECT_EQ(cpu->di, 0x0502);
}

TEST_F(Cpu80286Test, OutswSendsAtomicSixteenBitPortWriteFromDsSi) {
    cpu->dx = 0x1F0;
    cpu->ds = 0; cpu->si = 0x0600;
    cpu->set_flag(cpu80286::FLAG_DF, false);
    mem[0x0600] = 0x34; mem[0x0601] = 0x12;
    run({0x6F});  // OUTSW
    EXPECT_EQ(last_out16_port, 0x1F0);
    EXPECT_EQ(last_out16_val, 0x1234);
    EXPECT_EQ(cpu->si, 0x0602);
}

// ---------------------------------------------------------------------------
// 0x66 operand-size prefix (386 concession, not 80286 behavior)
// ---------------------------------------------------------------------------

TEST_F(Cpu80286Test, OpSize32MovImmediateLoadsFullThirtyTwoBits) {
    allow_firmware();
    run({0x66, 0xB8, 0x78, 0x56, 0x34, 0x12});  // MOV EAX, 12345678h
    EXPECT_EQ(cpu->ax, 0x12345678u);
}

TEST_F(Cpu80286Test, OpSize16WritePreservesUpperHalfOfFullRegister) {
    cpu->ax = 0xDEAD0000;
    run({0xB8, 0x34, 0x12});  // MOV AX, 1234h (no 0x66 -- 16-bit form)
    EXPECT_EQ(cpu->ax, 0xDEAD1234u);
}

TEST_F(Cpu80286Test, OpSize32AddRegReg) {
    allow_firmware();
    run({0x66, 0xB8, 0x01, 0x00, 0x00, 0x00});  // MOV EAX, 1
    run({0x66, 0xBB, 0xFF, 0xFF, 0xFF, 0xFF});  // MOV EBX, FFFFFFFFh (-1)
    // ADD EAX, EBX  (0x66 01 D8) -> 0, carry out
    load({0x66, 0x01, 0xD8}, cpu->ip);
    cpu->step();
    EXPECT_EQ(cpu->ax, 0u);
    EXPECT_TRUE(CF());
}

TEST_F(Cpu80286Test, OpSize32PushPopRoundTrip) {
    allow_firmware();
    cpu->ss = 0;
    cpu->sp = 0x2000;
    cpu->bx = 0x12345678;
    runN({0x66, 0x53, 0x66, 0x59}, 2);  // PUSH EBX ; POP ECX  (0x66-prefixed)
    EXPECT_EQ(cpu->cx, 0x12345678u);
    EXPECT_EQ(cpu->sp, 0x2000u);
}

TEST_F(Cpu80286Test, MovzxByteToWord) {
    allow_firmware();
    cpu->bx = 0x00FF;  // BL = 0xFF
    run({0x0F, 0xB6, 0xC3});  // MOVZX AX, BL
    EXPECT_EQ(cpu->ax, 0x00FFu);  // zero-extended, not sign-extended
}

TEST_F(Cpu80286Test, MovsxByteToWordSignExtends) {
    allow_firmware();
    cpu->bx = 0x00FF;  // BL = 0xFF (-1)
    run({0x0F, 0xBE, 0xC3});  // MOVSX AX, BL
    EXPECT_EQ(cpu->ax, 0xFFFFu);
}

TEST_F(Cpu80286Test, SetccSetsByteFromCondition) {
    allow_firmware();
    cpu->set_flag(cpu80286::FLAG_ZF, true);
    cpu->bx = 0;
    run({0x0F, 0x94, 0xC3});  // SETZ BL
    EXPECT_EQ(cpu->bx & 0xFF, 1u);
    cpu->set_flag(cpu80286::FLAG_ZF, false);
    run({0x0F, 0x94, 0xC3});
    EXPECT_EQ(cpu->bx & 0xFF, 0u);
}

TEST_F(Cpu80286Test, TwoOperandImulSixteenBit) {
    allow_firmware();
    cpu->ax = 6;
    cpu->bx = 7;
    run({0x0F, 0xAF, 0xC3});  // IMUL AX, BX
    EXPECT_EQ(cpu->ax, 42u);
    EXPECT_FALSE(CF());
}

TEST_F(Cpu80286Test, Opcodes386OutsideFirmwareRaiseInvalidOpcode) {
    handler(6);
    const std::initializer_list<uint8_t> forms[] = {
        {0x66, 0xB8, 0x78, 0x56, 0x34, 0x12},  // MOV EAX, imm32
        {0x0F, 0x84, 0x05, 0x00},              // JZ rel16
        {0x0F, 0x94, 0xC3},                    // SETZ BL
        {0x0F, 0xAF, 0xC3},                    // IMUL AX, BX
        {0x0F, 0xB6, 0xC3},                    // MOVZX AX, BL
        {0x0F, 0xBE, 0xC3},                    // MOVSX AX, BL
    };
    for (const auto &code : forms) {
        cpu->ax = 0x1111;
        exec(code);
        EXPECT_TRUE(in_handler());
        EXPECT_EQ(frame(0), 0) << "#UD saves the faulting instruction's address";
        EXPECT_EQ(cpu->ax, 0x1111u);
    }
}

TEST_F(Cpu80286Test, FirmwarePredicateSeesTheInstructionsPhysicalAddress) {
    std::vector<uint32_t> asked;
    cpu->firmware_at = [&](uint32_t a) { asked.push_back(a); return true; };
    cpu->bx = 0x80;
    exec({0x90, 0x0F, 0xB6, 0xC3}, 2);  // NOP ; MOVZX AX, BL
    ASSERT_FALSE(asked.empty());
    EXPECT_EQ(asked.back(), 0x1001u);
    EXPECT_EQ(cpu->ax, 0x80u);
}

TEST_F(Cpu80286Test, UndefinedAndRealModeOnlyOpcodesRaiseInvalidOpcode) {
    handler(6);
    const std::initializer_list<uint8_t> forms[] = {
        {0x63, 0xC0},        // ARPL, protected mode only
        {0x64, 0x90},        // FS: prefix, 386
        {0x65, 0x90},        // GS: prefix, 386
        {0x67, 0x90},        // address-size prefix, 386
        {0x0F, 0x00, 0xC0},  // SLDT AX, protected mode only
        {0x0F, 0x02, 0xC0},  // LAR AX, AX
        {0x0F, 0x01, 0xE8},  // 0F 01 /5
        {0x8D, 0xC3},        // LEA AX, BX
        {0xC4, 0xC3},        // LES AX, BX
        {0xC5, 0xC3},        // LDS AX, BX
        {0x62, 0xC3},        // BOUND AX, BX
        {0x8E, 0xC8},        // MOV CS, AX
        {0xFF, 0xD8},        // CALL FAR BX
        {0xFF, 0xE8},        // JMP FAR BX
        {0xFF, 0xF8},        // FF /7
    };
    for (const auto &code : forms) {
        exec(code);
        EXPECT_TRUE(in_handler()) << "opcode " << int(*code.begin());
        EXPECT_EQ(frame(0), 0);
    }
}

TEST_F(Cpu80286Test, F1IsALockAliasPrefix) {
    exec({0xF1, 0xB8, 0x34, 0x12});  // F1 ; MOV AX, 1234h
    EXPECT_EQ(cpu->ax, 0x1234u);
    EXPECT_EQ(cpu->ip, 4);
}

TEST_F(Cpu80286Test, SalcSetsAlFromCarry) {
    cpu->set_flag(cpu80286::FLAG_CF, true);
    exec({0xD6});
    EXPECT_EQ(cpu->ax & 0xFF, 0xFFu);
    cpu->set_flag(cpu80286::FLAG_CF, false);
    exec({0xD6});
    EXPECT_EQ(cpu->ax & 0xFF, 0x00u);
}

// ---------------------------------------------------------------------------
// Real-mode system instructions
// ---------------------------------------------------------------------------

TEST_F(Cpu80286Test, SmswReadsTheResetMsw) {
    exec({0x0F, 0x01, 0xE0});  // SMSW AX
    EXPECT_EQ(cpu->ax, 0xFFF0u);
}

TEST_F(Cpu80286Test, LmswLoadsMpEmTsButCannotSetPe) {
    uint16_t reported = 0;
    cpu->on_unimplemented = [&](uint16_t, uint16_t, uint16_t op) { reported = op; };
    cpu->ax = 0x000F;
    exec({0x0F, 0x01, 0xF0});  // LMSW AX
    EXPECT_EQ(cpu->msw(), 0xFFFEu);
    EXPECT_EQ(reported, 0x0F01u);
}

TEST_F(Cpu80286Test, CltsClearsTaskSwitched) {
    cpu->ax = 0x0008;
    exec({0x0F, 0x01, 0xF0, 0x0F, 0x06}, 2);  // LMSW AX ; CLTS
    EXPECT_EQ(cpu->msw() & 0x0008, 0);
}

TEST_F(Cpu80286Test, SidtStoresTheResetIdtAndFfInTheSixthByte) {
    exec({0x0F, 0x01, 0x0E, 0x00, 0x03});  // SIDT [0300h]
    EXPECT_EQ(mem[0x300] | (mem[0x301] << 8), 0x03FF);
    EXPECT_EQ(mem[0x302] | (mem[0x303] << 8) | (mem[0x304] << 16), 0);
    EXPECT_EQ(mem[0x305], 0xFF) << "a 386 stores 00h here";
}

TEST_F(Cpu80286Test, SgdtReturnsWhatLgdtLoaded) {
    uint8_t table[6] = {0x27, 0x00, 0x00, 0x40, 0x01, 0x55};  // limit 27h, base 014000h
    for (int i = 0; i < 6; ++i) mem[0x300 + i] = table[i];
    exec({0x0F, 0x01, 0x16, 0x00, 0x03, 0x0F, 0x01, 0x06, 0x10, 0x03}, 2);  // LGDT [0300h] ; SGDT [0310h]
    EXPECT_EQ(mem[0x310], 0x27);
    EXPECT_EQ(mem[0x313], 0x40);
    EXPECT_EQ(mem[0x314], 0x01);
    EXPECT_EQ(mem[0x315], 0xFF);
}

TEST_F(Cpu80286Test, LidtMovesTheRealModeVectorTable) {
    uint8_t table[6] = {0xFF, 0x03, 0x00, 0x20, 0x00, 0x00};  // limit 3FFh, base 2000h
    for (int i = 0; i < 6; ++i) mem[0x300 + i] = table[i];
    mem[0x2000 + 0x21 * 4 + 0] = 0x34; mem[0x2000 + 0x21 * 4 + 1] = 0x12;
    mem[0x2000 + 0x21 * 4 + 2] = 0x00; mem[0x2000 + 0x21 * 4 + 3] = 0x09;
    exec({0x0F, 0x01, 0x1E, 0x00, 0x03, 0xCD, 0x21}, 2);  // LIDT [0300h] ; INT 21h
    EXPECT_EQ(cpu->cs, 0x0900);
    EXPECT_EQ(cpu->ip, 0x1234);
}

TEST_F(Cpu80286Test, EscWithEmulateSetRaisesDeviceNotAvailable) {
    handler(7);
    cpu->ax = 0x0004;
    exec({0xDB, 0xE3});  // FNINIT, EM clear: no coprocessor, nothing happens
    EXPECT_EQ(cpu->ip, 2);
    exec({0x0F, 0x01, 0xF0, 0xDB, 0xE3}, 2);  // LMSW AX (EM) ; FNINIT
    EXPECT_TRUE(in_handler());
    EXPECT_EQ(frame(0), 3);
}

// ---------------------------------------------------------------------------
// Single-step, interrupt shadow, interruptible REP
// ---------------------------------------------------------------------------

TEST_F(Cpu80286Test, SingleStepTrapsAfterTheInstruction) {
    handler(1);
    cpu->set_flag(cpu80286::FLAG_TF, true);
    exec({0x90, 0x90});
    EXPECT_TRUE(in_handler());
    EXPECT_EQ(frame(0), 1) << "the trap saves the next instruction's address";
    EXPECT_TRUE(frame(2) & cpu80286::FLAG_TF);
    EXPECT_FALSE(cpu->flag(cpu80286::FLAG_TF)) << "the handler runs untrapped";
}

TEST_F(Cpu80286Test, PopfThatSetsTfRunsUntrapped) {
    handler(1);
    uint16_t popped = cpu80286::FLAG_TF | cpu80286::FLAG_R1;
    mem[0x8000] = uint8_t(popped); mem[0x8001] = uint8_t(popped >> 8);
    exec({0x9D, 0x90});  // POPF ; NOP
    EXPECT_EQ(cpu->cs, 0x0100);
    EXPECT_EQ(cpu->ip, 1);
    cpu->step();
    EXPECT_TRUE(in_handler()) << "the NOP after it traps";
}

TEST_F(Cpu80286Test, PopfThatClearsTfStillTraps) {
    handler(1);
    mem[0x8000] = uint8_t(cpu80286::FLAG_R1); mem[0x8001] = 0;
    cpu->set_flag(cpu80286::FLAG_TF, true);
    exec({0x9D});  // POPF
    EXPECT_TRUE(in_handler());
    EXPECT_EQ(frame(0), 1);
}

TEST_F(Cpu80286Test, IntNEntersItsHandlerUntrapped) {
    handler(1);
    mem[0x20 * 4 + 0] = 0x00; mem[0x20 * 4 + 1] = 0x00;
    mem[0x20 * 4 + 2] = 0x00; mem[0x20 * 4 + 3] = 0x08;
    cpu->set_flag(cpu80286::FLAG_TF, true);
    exec({0xCD, 0x20});
    EXPECT_EQ(cpu->cs, 0x0800);
    EXPECT_EQ(cpu->ip, 0);
}

TEST_F(Cpu80286Test, MovSsHoldsOffTheTrapForOneInstruction) {
    handler(1);
    cpu->set_flag(cpu80286::FLAG_TF, true);
    cpu->ax = 0;
    exec({0x8E, 0xD0, 0x90});  // MOV SS, AX ; NOP
    EXPECT_TRUE(cpu->interrupt_shadow());
    EXPECT_EQ(cpu->ip, 2);
    cpu->step();
    EXPECT_FALSE(cpu->interrupt_shadow());
    EXPECT_TRUE(in_handler());
    EXPECT_EQ(frame(0), 3);
}

TEST_F(Cpu80286Test, PopSsAndStiFromClearSetTheShadow) {
    exec({0x17});  // POP SS
    EXPECT_TRUE(cpu->interrupt_shadow());
    cpu->set_flag(cpu80286::FLAG_IF, false);
    exec({0xFB});  // STI with IF clear
    EXPECT_TRUE(cpu->interrupt_shadow());
    exec({0xFB});  // STI with IF already set
    EXPECT_FALSE(cpu->interrupt_shadow());
}

TEST_F(Cpu80286Test, RepYieldsOnItsFirstPrefixAndCostsTheSameInChunks) {
    cpu->rep_yield_cycles = 6;
    for (int i = 0; i < 10; ++i) mem[0x3000 + i] = uint8_t(i + 1);
    cpu->ds = 0; cpu->es = 0x0400;
    cpu->si = 0x3000; cpu->di = 0;
    cpu->cx = 10;
    exec({0x26, 0xF3, 0xA4, 0x90}, 0);  // ES: REP MOVSB (source ES:SI) ; NOP
    cpu->es = 0x0400;
    int total = cpu->step();
    EXPECT_EQ(cpu->ip, 0) << "IP stays on the first prefix while the REP is unfinished";
    EXPECT_GT(cpu->cx, 0u);
    EXPECT_LT(cpu->cx, 10u);
    while (cpu->ip == 0) total += cpu->step();
    EXPECT_EQ(cpu->ip, 3);
    EXPECT_EQ(cpu->cx, 0u);
    EXPECT_EQ(total, 2 * 2 + 5 + 4 * 10) << "two prefixes plus REP MOVS 5+4n, as if uninterrupted";
}

TEST_F(Cpu80286Test, RepUnderTfTrapsAfterEachIteration) {
    handler(1);
    cpu->set_flag(cpu80286::FLAG_TF, true);
    cpu->es = 0; cpu->di = 0x3000; cpu->cx = 3; cpu->ax = 0x55;
    exec({0xF3, 0xAA});  // REP STOSB
    EXPECT_TRUE(in_handler());
    EXPECT_EQ(frame(0), 0) << "the trap returns to the REP to run the next iteration";
    EXPECT_EQ(cpu->cx, 2u);
    EXPECT_EQ(mem[0x3000], 0x55);
    EXPECT_EQ(mem[0x3001], 0x00);
}

// ---------------------------------------------------------------------------
// Real-mode segment overrun and instruction length
// ---------------------------------------------------------------------------

TEST_F(Cpu80286Test, WordAtOffsetFfffRaisesGeneralProtection) {
    handler(13);
    cpu->ax = 0x1111;
    exec({0x26, 0xA1, 0xFF, 0xFF});  // MOV AX, ES:[FFFFh]
    EXPECT_TRUE(in_handler());
    EXPECT_EQ(frame(0), 0) << "a fault restarts at the first prefix";
    EXPECT_EQ(cpu->ax, 0x1111u);
}

TEST_F(Cpu80286Test, WordAtOffsetFffeIsFine) {
    handler(13);
    mem[0xFFFE] = 0x34; mem[0xFFFF] = 0x12;
    cpu->ds = 0;
    exec({0xA1, 0xFE, 0xFF});  // MOV AX, [FFFEh]
    EXPECT_EQ(cpu->ax, 0x1234u);
}

TEST_F(Cpu80286Test, SsOperandAtOffsetFfffRaisesStackFault) {
    handler(12);
    cpu->bp = 0xFFFF;
    exec({0x8B, 0x46, 0x00});  // MOV AX, [BP+0]
    EXPECT_TRUE(in_handler());
}

TEST_F(Cpu80286Test, PopAtSpFfffFaultsWithSpUnchanged) {
    handler(12);
    cpu->ax = 0x1111;
    mem[0x1000] = 0x58;  // POP AX
    cpu->cs = 0x0100; cpu->ip = 0;
    cpu->ss = 0x0900; cpu->sp = 0xFFFF;
    cpu->step();
    EXPECT_EQ(cpu->cs, 0x0700);
    EXPECT_EQ(cpu->ax, 0x1111u);
    EXPECT_EQ(frame(0), 0);
    EXPECT_EQ(uint16_t(cpu->sp + 6), 0xFFFF) << "the frame sits below the SP the POP started with";
}

TEST_F(Cpu80286Test, InstructionOverTenBytesRaisesGeneralProtection) {
    handler(13);
    exec({0x26, 0x26, 0x26, 0x26, 0x26, 0x26, 0x26, 0x26, 0x26, 0x90});  // 10 bytes: fine
    EXPECT_EQ(cpu->ip, 10);
    exec({0x26, 0x26, 0x26, 0x26, 0x26, 0x26, 0x26, 0x26, 0x26, 0x26, 0x90});  // 11
    EXPECT_TRUE(in_handler());
    EXPECT_EQ(frame(0), 0);
}

TEST_F(Cpu80286Test, InstructionRunningPastFfffRaisesGeneralProtection) {
    handler(13);
    mem[0x1000 + 0xFFFF] = 0xB8;  // MOV AX, imm16 starting at the last byte
    cpu->cs = 0x0100; cpu->ip = 0xFFFF;
    cpu->ss = 0; cpu->sp = 0x8000;
    cpu->step();
    EXPECT_TRUE(in_handler());
    EXPECT_EQ(frame(0), 0xFFFF);
}

TEST_F(Cpu80286Test, DivideErrorRestartsAtThePrefix) {
    mem[0] = 0x00; mem[1] = 0x00; mem[2] = 0x00; mem[3] = 0x07;
    cpu->bx = 0;
    exec({0x26, 0xF6, 0xF3});  // ES: DIV BL
    EXPECT_TRUE(in_handler());
    EXPECT_EQ(frame(0), 0);
}

// ---------------------------------------------------------------------------
// BCD adjust
// ---------------------------------------------------------------------------

TEST_F(Cpu80286Test, DaaAdjustsAfterBcdAddition) {
    // 6 + 5 in packed BCD: raw binary sum is 0x0B; DAA must adjust to 0x11.
    cpu->ax = 0x000B;
    run({0x27});  // DAA
    EXPECT_EQ(cpu->ax & 0xFF, 0x11);
    EXPECT_TRUE(AF());
    EXPECT_FALSE(CF());
}

}  // namespace
