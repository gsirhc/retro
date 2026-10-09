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
    uint16_t reported = 0;

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
        cpu->on_unimplemented = [this](uint16_t, uint16_t, uint16_t op) { reported = op; };
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


TEST_F(Cpu80286Test, DaaLeavesAValidDigitAloneAndAdjustsTheHighNibble) {
    cpu->ax = 0x0009;
    run({0x27});
    EXPECT_EQ(cpu->ax & 0xFF, 0x09u);
    EXPECT_FALSE(AF());
    EXPECT_FALSE(CF());

    cpu->ax = 0x00A0;
    run({0x27});
    EXPECT_EQ(cpu->ax & 0xFF, 0x00u);
    EXPECT_TRUE(CF());
    EXPECT_TRUE(ZF());
}

TEST_F(Cpu80286Test, DasAdjustsAfterBcdSubtraction) {
    cpu->ax = 0x001F;
    run({0x2F});
    EXPECT_EQ(cpu->ax & 0xFF, 0x19u);
    EXPECT_TRUE(AF());
    EXPECT_FALSE(CF());

    cpu->ax = 0x00A0;
    cpu->set_flag(cpu80286::FLAG_AF, false);
    run({0x2F});
    EXPECT_EQ(cpu->ax & 0xFF, 0x40u);
    EXPECT_FALSE(AF());
    EXPECT_TRUE(CF());

    cpu->ax = 0x0003;
    cpu->set_flag(cpu80286::FLAG_AF, true);
    cpu->set_flag(cpu80286::FLAG_CF, false);
    run({0x2F});
    EXPECT_EQ(cpu->ax & 0xFF, 0xFDu);
    EXPECT_TRUE(CF()) << "the borrow out of AL - 6 sets CF";
}

TEST_F(Cpu80286Test, AaaAddsSixToAxNotAl) {
    // AX=00FFh gives 0105h on an 8088 and 0205h on later parts (Hummel)
    cpu->ax = 0x00FF;
    run({0x37});
    EXPECT_EQ(cpu->ax, 0x0205u);
    EXPECT_TRUE(AF());
    EXPECT_TRUE(CF());

    cpu->ax = 0x000B;
    run({0x37});
    EXPECT_EQ(cpu->ax, 0x0101u);

    cpu->ax = 0x0135;
    cpu->set_flag(cpu80286::FLAG_AF, false);
    run({0x37});
    EXPECT_EQ(cpu->ax, 0x0105u);
    EXPECT_FALSE(AF());
    EXPECT_FALSE(CF());
}

TEST_F(Cpu80286Test, AasSubtractsSixFromAxThenOneFromAh) {
    cpu->ax = 0x0200;
    cpu->set_flag(cpu80286::FLAG_AF, true);
    run({0x3F});
    EXPECT_EQ(cpu->ax, 0x000Au) << "the borrow out of AL reaches AH";
    EXPECT_TRUE(AF());
    EXPECT_TRUE(CF());

    cpu->ax = 0x010F;
    run({0x3F});
    EXPECT_EQ(cpu->ax, 0x0009u);

    cpu->ax = 0x0135;
    cpu->set_flag(cpu80286::FLAG_AF, false);
    run({0x3F});
    EXPECT_EQ(cpu->ax, 0x0105u);
    EXPECT_FALSE(CF());
}

TEST_F(Cpu80286Test, AamSplitsAlByItsImmediateBase) {
    cpu->ax = 0x004F;
    exec({0xD4, 0x0A});
    EXPECT_EQ(cpu->ax, 0x0709u);
    cpu->ax = 0x004F;
    exec({0xD4, 0x10});
    EXPECT_EQ(cpu->ax, 0x040Fu);
}

TEST_F(Cpu80286Test, AamByZeroIsADivideError) {
    handler(0);
    cpu->ax = 0x004F;
    exec({0xD4, 0x00});
    EXPECT_TRUE(in_handler());
    EXPECT_EQ(frame(0), 0);
    EXPECT_EQ(cpu->ax, 0x004Fu);
}

TEST_F(Cpu80286Test, AadJoinsAhAndAlByItsImmediateBase) {
    cpu->ax = 0x0709;
    exec({0xD5, 0x0A});
    EXPECT_EQ(cpu->ax, 0x004Fu);
    cpu->ax = 0x0405;
    exec({0xD5, 0x10});
    EXPECT_EQ(cpu->ax, 0x0045u);
    cpu->ax = 0x0000;
    exec({0xD5, 0x0A});
    EXPECT_TRUE(ZF());
}

// ---------------------------------------------------------------------------
// ALU, shift and rotate forms
// ---------------------------------------------------------------------------

TEST_F(Cpu80286Test, AdcAndSbbTakeTheCarryIn) {
    cpu->set_flag(cpu80286::FLAG_CF, true);
    cpu->ax = 0x007F; cpu->bx = 0;
    run({0x10, 0xD8});  // ADC AL, BL
    EXPECT_EQ(cpu->ax & 0xFF, 0x80u);
    EXPECT_TRUE(OF());

    cpu->set_flag(cpu80286::FLAG_CF, true);
    cpu->ax = 0x0000;
    run({0x18, 0xD8});  // SBB AL, BL
    EXPECT_EQ(cpu->ax & 0xFF, 0xFFu);
    EXPECT_TRUE(CF());

    cpu->set_flag(cpu80286::FLAG_CF, true);
    cpu->ax = 0xFFFF;
    run({0x11, 0xD8});  // ADC AX, BX
    EXPECT_EQ(cpu->ax, 0u);
    EXPECT_TRUE(CF());
    EXPECT_TRUE(ZF());

    cpu->set_flag(cpu80286::FLAG_CF, true);
    cpu->ax = 0x8000;
    run({0x19, 0xD8});  // SBB AX, BX
    EXPECT_EQ(cpu->ax, 0x7FFFu);
    EXPECT_TRUE(OF());
}

TEST_F(Cpu80286Test, OrWordClearsCarryAndOverflow) {
    cpu->set_flag(cpu80286::FLAG_CF, true);
    cpu->set_flag(cpu80286::FLAG_OF, true);
    cpu->ax = 0x8000; cpu->bx = 0x0001;
    run({0x09, 0xD8});  // OR AX, BX
    EXPECT_EQ(cpu->ax, 0x8001u);
    EXPECT_FALSE(CF());
    EXPECT_FALSE(OF());
    EXPECT_TRUE(SF());
}

TEST_F(Cpu80286Test, ByteRotatesAndSarByOne) {
    cpu->ax = 0x0081;
    run({0xD0, 0xC0});  // ROL AL, 1
    EXPECT_EQ(cpu->ax & 0xFF, 0x03u);
    EXPECT_TRUE(CF());
    EXPECT_TRUE(OF());

    cpu->ax = 0x0001;
    run({0xD0, 0xC8});  // ROR AL, 1
    EXPECT_EQ(cpu->ax & 0xFF, 0x80u);
    EXPECT_TRUE(CF());
    EXPECT_TRUE(OF());

    cpu->ax = 0x0000;
    cpu->set_flag(cpu80286::FLAG_CF, true);
    run({0xD0, 0xD8});  // RCR AL, 1
    EXPECT_EQ(cpu->ax & 0xFF, 0x80u);
    EXPECT_FALSE(CF());
    EXPECT_TRUE(OF());

    cpu->ax = 0x0081;
    run({0xD0, 0xF8});  // SAR AL, 1
    EXPECT_EQ(cpu->ax & 0xFF, 0xC0u);
    EXPECT_TRUE(CF());
    EXPECT_FALSE(OF());
    EXPECT_TRUE(SF());
}

TEST_F(Cpu80286Test, WordRotatesAndSarByOne) {
    cpu->ax = 0x0001;
    run({0xD1, 0xC8});  // ROR AX, 1
    EXPECT_EQ(cpu->ax, 0x8000u);
    EXPECT_TRUE(CF());
    EXPECT_TRUE(OF());

    cpu->ax = 0x8000;
    cpu->set_flag(cpu80286::FLAG_CF, false);
    run({0xD1, 0xD0});  // RCL AX, 1
    EXPECT_EQ(cpu->ax, 0x0000u);
    EXPECT_TRUE(CF());
    EXPECT_TRUE(OF());

    cpu->ax = 0x0000;
    cpu->set_flag(cpu80286::FLAG_CF, true);
    run({0xD1, 0xD8});  // RCR AX, 1
    EXPECT_EQ(cpu->ax, 0x8000u);
    EXPECT_FALSE(CF());
    EXPECT_TRUE(OF());

    cpu->ax = 0x8001;
    run({0xD1, 0xF8});  // SAR AX, 1
    EXPECT_EQ(cpu->ax, 0xC000u);
    EXPECT_TRUE(CF());
    EXPECT_FALSE(OF());
}

TEST_F(Cpu80286Test, ConditionalJumpsReadTheirFlags) {
    using cpu80286::FLAG_OF; using cpu80286::FLAG_SF; using cpu80286::FLAG_PF; using cpu80286::FLAG_ZF;
    struct Case { uint8_t op; bool of, sf, pf, zf, taken; };
    const Case cases[] = {
        {0x70, true,  false, false, false, true},   // JO
        {0x70, false, false, false, false, false},
        {0x71, false, false, false, false, true},   // JNO
        {0x71, true,  false, false, false, false},
        {0x78, false, true,  false, false, true},   // JS
        {0x7A, false, false, true,  false, true},   // JP
        {0x7A, false, false, false, false, false},
        {0x7B, false, false, false, false, true},   // JNP
        {0x7C, true,  false, false, false, true},   // JL: SF != OF
        {0x7C, true,  true,  false, false, false},
        {0x7D, true,  true,  false, false, true},   // JGE: SF == OF
        {0x7D, false, true,  false, false, false},
        {0x7E, false, false, false, true,  true},   // JLE: ZF or SF != OF
        {0x7E, false, true,  false, false, true},
        {0x7E, false, false, false, false, false},
        {0x7F, false, false, false, false, true},   // JG: !ZF and SF == OF
        {0x7F, false, false, false, true,  false},
        {0x7F, true,  false, false, false, false},
    };
    for (const auto &k : cases) {
        cpu->set_flag(FLAG_OF, k.of); cpu->set_flag(FLAG_SF, k.sf);
        cpu->set_flag(FLAG_PF, k.pf); cpu->set_flag(FLAG_ZF, k.zf);
        run({k.op, 0x10});
        EXPECT_EQ(cpu->ip, k.taken ? 0x12 : 0x02) << "opcode " << std::hex << int(k.op);
    }
}

TEST_F(Cpu80286Test, LoopneAndLoopeTestZfAsWellAsCx) {
    cpu->cx = 2;
    cpu->set_flag(cpu80286::FLAG_ZF, false);
    run({0xE0, 0x10});  // LOOPNE
    EXPECT_EQ(cpu->ip, 0x12);
    cpu->cx = 2;
    cpu->set_flag(cpu80286::FLAG_ZF, true);
    run({0xE0, 0x10});
    EXPECT_EQ(cpu->ip, 0x02);
    EXPECT_EQ(cpu->cx, 1u);

    cpu->cx = 2;
    run({0xE1, 0x10});  // LOOPE, ZF still set
    EXPECT_EQ(cpu->ip, 0x12);
    cpu->cx = 2;
    cpu->set_flag(cpu80286::FLAG_ZF, false);
    run({0xE1, 0x10});
    EXPECT_EQ(cpu->ip, 0x02);
}

// ---------------------------------------------------------------------------
// Group 3 and group 5
// ---------------------------------------------------------------------------

TEST_F(Cpu80286Test, NegByteSetsCarryUnlessZero) {
    cpu->ax = 0x0001;
    run({0xF6, 0xD8});  // NEG AL
    EXPECT_EQ(cpu->ax & 0xFF, 0xFFu);
    EXPECT_TRUE(CF());
    cpu->ax = 0x0000;
    run({0xF6, 0xD8});
    EXPECT_EQ(cpu->ax & 0xFF, 0x00u);
    EXPECT_FALSE(CF());
}

TEST_F(Cpu80286Test, ImulByteSignExtendsIntoAh) {
    cpu->ax = 0x00FE; cpu->bx = 100;
    run({0xF6, 0xEB});  // IMUL BL: -2 * 100
    EXPECT_EQ(cpu->ax, 0xFF38u);
    EXPECT_TRUE(CF());
    EXPECT_TRUE(OF());
    cpu->ax = 0x00FE; cpu->bx = 3;
    run({0xF6, 0xEB});
    EXPECT_EQ(cpu->ax, 0xFFFAu);
    EXPECT_FALSE(CF());
}

TEST_F(Cpu80286Test, IdivByteTruncatesTowardZero) {
    cpu->ax = 0xFFF9; cpu->bx = 2;
    exec({0xF6, 0xFB});  // IDIV BL: -7 / 2
    EXPECT_EQ(cpu->ax, 0xFFFDu);  // AH = -1, AL = -3
    cpu->ax = 0xFF00; cpu->bx = 2;
    exec({0xF6, 0xFB});  // -256 / 2 = -128 fits on the 286
    EXPECT_EQ(cpu->ax, 0x0080u);
}

TEST_F(Cpu80286Test, IdivByteOverflowAndZeroAreDivideErrors) {
    handler(0);
    cpu->ax = 0x0200; cpu->bx = 2;
    exec({0xF6, 0xFB});
    EXPECT_TRUE(in_handler());
    cpu->ax = 0x0010; cpu->bx = 0;
    exec({0xF6, 0xFB});
    EXPECT_TRUE(in_handler());
    EXPECT_EQ(cpu->ax, 0x0010u);
}

TEST_F(Cpu80286Test, TestAndNotWord) {
    cpu->ax = 0x8000;
    run({0xF7, 0xC0, 0x00, 0x80});  // TEST AX, 8000h
    EXPECT_EQ(cpu->ax, 0x8000u);
    EXPECT_TRUE(SF());
    EXPECT_FALSE(ZF());
    cpu->ax = 0x00FF;
    run({0xF7, 0xD0});  // NOT AX
    EXPECT_EQ(cpu->ax, 0xFF00u);
}

TEST_F(Cpu80286Test, ImulWordFillsDx) {
    cpu->ax = 0xFED4; cpu->bx = 300;
    run({0xF7, 0xEB});  // IMUL BX: -300 * 300
    EXPECT_EQ(cpu->ax, 0xA070u);
    EXPECT_EQ(cpu->dx, 0xFFFEu);
    EXPECT_TRUE(OF());
}

TEST_F(Cpu80286Test, DivAndIdivWord) {
    cpu->dx = 0x0001; cpu->ax = 0x0000; cpu->bx = 3;
    exec({0xF7, 0xF3});  // DIV BX
    EXPECT_EQ(cpu->ax, 0x5555u);
    EXPECT_EQ(cpu->dx, 1u);
    cpu->dx = 0xFFFE; cpu->ax = 0x7960; cpu->bx = 7;
    exec({0xF7, 0xFB});  // IDIV BX: -100000 / 7
    EXPECT_EQ(cpu->ax, 0xC833u);
    EXPECT_EQ(cpu->dx, 0xFFFBu);
}

TEST_F(Cpu80286Test, WordDivideErrors) {
    handler(0);
    const struct { uint16_t dx, ax, bx; uint8_t modrm; } cases[] = {
        {0x0005, 0x0000, 0x0005, 0xF3},  // DIV quotient over FFFFh
        {0x0000, 0x0010, 0x0000, 0xF3},  // DIV by zero
        {0x8000, 0x0000, 0xFFFF, 0xFB},  // IDIV -2^31 / -1
        {0x0001, 0x0000, 0x0001, 0xFB},  // IDIV quotient over 7FFFh
        {0x0000, 0x0010, 0x0000, 0xFB},  // IDIV by zero
    };
    for (const auto &k : cases) {
        cpu->dx = k.dx; cpu->ax = k.ax; cpu->bx = k.bx;
        exec({0xF7, k.modrm});
        EXPECT_TRUE(in_handler()) << std::hex << k.dx << ":" << k.ax << " / " << k.bx;
        EXPECT_EQ(frame(0), 0);
    }
}

TEST_F(Cpu80286Test, IndirectJumpsCallsAndPush) {
    cpu->ds = 0;
    mem[0x500] = 0x78; mem[0x501] = 0x56; mem[0x502] = 0x34; mem[0x503] = 0x12;

    cpu->bx = 0x1234;
    exec({0xFF, 0xE3});  // JMP BX
    EXPECT_EQ(cpu->cs, 0x0100);
    EXPECT_EQ(cpu->ip, 0x1234);

    exec({0xFF, 0x2E, 0x00, 0x05});  // JMP FAR [0500h]
    EXPECT_EQ(cpu->cs, 0x1234);
    EXPECT_EQ(cpu->ip, 0x5678);
    EXPECT_EQ(cpu->sp, 0x8000u);

    exec({0xFF, 0x1E, 0x00, 0x05});  // CALL FAR [0500h]
    EXPECT_EQ(cpu->cs, 0x1234);
    EXPECT_EQ(cpu->ip, 0x5678);
    EXPECT_EQ(frame(0), 4);
    EXPECT_EQ(frame(1), 0x0100);

    exec({0xFF, 0x36, 0x00, 0x05});  // PUSH [0500h]
    EXPECT_EQ(cpu->sp, 0x7FFEu);
    EXPECT_EQ(frame(0), 0x5678);
}

// ---------------------------------------------------------------------------
// Strings, prefixes, addressing, registers
// ---------------------------------------------------------------------------

TEST_F(Cpu80286Test, WordStringOps) {
    cpu->ds = 0x0200; cpu->es = 0x0300;
    mem[0x2000] = 0x34; mem[0x2001] = 0x12;
    cpu->si = 0; cpu->di = 0;
    run({0xA5});  // MOVSW
    EXPECT_EQ(mem[0x3000], 0x34);
    EXPECT_EQ(mem[0x3001], 0x12);
    EXPECT_EQ(cpu->si, 2u);
    EXPECT_EQ(cpu->di, 2u);

    cpu->si = 0; cpu->di = 0;
    run({0xA7});  // CMPSW
    EXPECT_TRUE(ZF());

    cpu->si = 0;
    run({0xAD});  // LODSW
    EXPECT_EQ(cpu->ax, 0x1234u);

    cpu->di = 0;
    run({0xAF});  // SCASW
    EXPECT_TRUE(ZF());
    EXPECT_EQ(cpu->di, 2u);
}

TEST_F(Cpu80286Test, RepInsbFillsCxBytes) {
    cpu->es = 0x0300; cpu->di = 0;
    cpu->dx = 0x0060; cpu->cx = 3;
    next_in_val = 0xAB;
    run({0xF3, 0x6C});  // REP INSB
    EXPECT_EQ(mem[0x3000], 0xAB);
    EXPECT_EQ(mem[0x3002], 0xAB);
    EXPECT_EQ(mem[0x3003], 0x00);
    EXPECT_EQ(cpu->cx, 0u);
    EXPECT_EQ(cpu->di, 3u);
}

TEST_F(Cpu80286Test, SsAndDsOverridesAndLock) {
    cpu->ds = 0x0200;
    mem[0x0010] = 0x77;
    mem[0x2010] = 0x11;
    exec({0x36, 0xA0, 0x10, 0x00});  // MOV AL, SS:[0010h]
    EXPECT_EQ(cpu->ax & 0xFF, 0x77u);
    cpu->bp = 0x0010;
    exec({0x3E, 0x8A, 0x46, 0x00});  // MOV AL, DS:[BP+0]
    EXPECT_EQ(cpu->ax & 0xFF, 0x11u);
    exec({0xF0, 0xB0, 0x5A});  // LOCK ; MOV AL, 5Ah
    EXPECT_EQ(cpu->ax & 0xFF, 0x5Au);
    EXPECT_EQ(cpu->ip, 3);
}

TEST_F(Cpu80286Test, BxPlusDiAndBpPlusDiAddressing) {
    cpu->ds = 0x0200; cpu->ss = 0x0300;
    cpu->bx = 0x10; cpu->bp = 0x20; cpu->di = 0x05;
    mem[0x2015] = 0xAA;
    mem[0x3025] = 0xBB;
    run({0x8A, 0x01});  // MOV AL, [BX+DI]
    EXPECT_EQ(cpu->ax & 0xFF, 0xAAu);
    run({0x8A, 0x03});  // MOV AL, [BP+DI], SS by default
    EXPECT_EQ(cpu->ax & 0xFF, 0xBBu);
}

TEST_F(Cpu80286Test, MovReachesEveryWordRegister) {
    cpu->ax = 0x1234;
    // MOV BP,AX ; MOV SP,BP ; MOV SI,SP ; MOV DI,SI ; MOV DX,DI
    runN({0x8B, 0xE8, 0x8B, 0xE5, 0x8B, 0xF4, 0x8B, 0xFE, 0x8B, 0xD7}, 5);
    EXPECT_EQ(cpu->bp, 0x1234u);
    EXPECT_EQ(cpu->sp, 0x1234u);
    EXPECT_EQ(cpu->si, 0x1234u);
    EXPECT_EQ(cpu->di, 0x1234u);
    EXPECT_EQ(cpu->dx, 0x1234u);
}

TEST_F(Cpu80286Test, LoadallIsReported) {
    exec({0x0F, 0x05});
    EXPECT_EQ(reported, 0x0F05u);
    EXPECT_EQ(cpu->ip, 2);
}

TEST_F(Cpu80286Test, FirmwareOpcodesWithNoModelAreReportedNotFaulted) {
    allow_firmware();
    handler(6);
    exec({0x0F, 0x00, 0xC0});  // SLDT AX
    EXPECT_EQ(reported, 0x0F00u);
    EXPECT_FALSE(in_handler());
    exec({0x64});  // FS:
    EXPECT_EQ(reported, 0x0064u);
    EXPECT_FALSE(in_handler());
}

// ---------------------------------------------------------------------------
// 386 forms the BIOS ROM uses (0x66 and 0F, firmware only)
// ---------------------------------------------------------------------------

TEST_F(Cpu80286Test, OpSize32MovReachesEveryRegister) {
    allow_firmware();
    exec({0x66, 0xBA, 0x02, 0x00, 0x00, 0x80,   // MOV EDX
          0x66, 0xBC, 0x04, 0x00, 0x00, 0x80,   // MOV ESP
          0x66, 0xBD, 0x05, 0x00, 0x00, 0x80,   // MOV EBP
          0x66, 0xBE, 0x06, 0x00, 0x00, 0x80,   // MOV ESI
          0x66, 0xBF, 0x07, 0x00, 0x00, 0x80},  // MOV EDI
         5);
    EXPECT_EQ(cpu->dx, 0x80000002u);
    EXPECT_EQ(cpu->sp, 0x80000004u);
    EXPECT_EQ(cpu->bp, 0x80000005u);
    EXPECT_EQ(cpu->si, 0x80000006u);
    EXPECT_EQ(cpu->di, 0x80000007u);
}

TEST_F(Cpu80286Test, OpSize32AluForms) {
    allow_firmware();
    struct Case { uint8_t op; bool cf_in; uint32_t want; };
    const Case cases[] = {
        {0x09, false, 0x1F3F5F7Fu},  // OR
        {0x11, true,  0x21436588u},  // ADC
        {0x19, true,  0x03254768u},  // SBB
        {0x21, false, 0x02040608u},  // AND
        {0x29, false, 0x03254769u},  // SUB
        {0x31, false, 0x1D3B5977u},  // XOR
        {0x39, false, 0x12345678u},  // CMP leaves EAX
    };
    for (const auto &k : cases) {
        cpu->ax = 0x12345678u; cpu->bx = 0x0F0F0F0Fu;
        cpu->set_flag(cpu80286::FLAG_CF, k.cf_in);
        exec({0x66, k.op, 0xD8});
        EXPECT_EQ(cpu->ax, k.want) << "opcode " << std::hex << int(k.op);
    }
    cpu->ax = 0x80000000u; cpu->bx = 1;
    exec({0x66, 0x39, 0xD8});  // CMP EAX, EBX
    EXPECT_TRUE(OF());
    EXPECT_FALSE(CF());
    cpu->ax = 0; cpu->bx = 1;
    exec({0x66, 0x29, 0xD8});
    EXPECT_EQ(cpu->ax, 0xFFFFFFFFu);
    EXPECT_TRUE(CF());
    EXPECT_TRUE(SF());
}

TEST_F(Cpu80286Test, OpSize32ImmediateGroup) {
    allow_firmware();
    cpu->ax = 1;
    exec({0x66, 0x81, 0xC0, 0xFF, 0xFF, 0xFF, 0x7F});  // ADD EAX, 7FFFFFFFh
    EXPECT_EQ(cpu->ax, 0x80000000u);
    EXPECT_TRUE(OF());
    cpu->ax = 0xFFFFFFFFu;
    exec({0x66, 0x83, 0xF8, 0xFF});  // CMP EAX, -1
    EXPECT_TRUE(ZF());
    EXPECT_EQ(cpu->ax, 0xFFFFFFFFu);
}

TEST_F(Cpu80286Test, OpSize32ShiftsAndRotates) {
    allow_firmware();
    struct Case { uint8_t modrm; uint32_t want; bool cf, of; };
    const Case cases[] = {
        {0xC0, 0x00000003u, true, true},   // ROL
        {0xC8, 0xC0000000u, true, false},  // ROR
        {0xD0, 0x00000002u, true, true},   // RCL
        {0xD8, 0x40000000u, true, true},   // RCR
        {0xE0, 0x00000002u, true, true},   // SHL
        {0xE8, 0x40000000u, true, true},   // SHR
        {0xF8, 0xC0000000u, true, false},  // SAR
    };
    for (const auto &k : cases) {
        cpu->ax = 0x80000001u;
        cpu->set_flag(cpu80286::FLAG_CF, false);
        exec({0x66, 0xD1, k.modrm});
        EXPECT_EQ(cpu->ax, k.want) << std::hex << int(k.modrm);
        EXPECT_EQ(CF(), k.cf) << std::hex << int(k.modrm);
        EXPECT_EQ(OF(), k.of) << std::hex << int(k.modrm);
    }
    cpu->ax = 0x80000001u;
    exec({0x66, 0xC1, 0xE0, 0x04});  // SHL EAX, 4
    EXPECT_EQ(cpu->ax, 0x00000010u);
    EXPECT_FALSE(CF());
    cpu->set_flag(cpu80286::FLAG_CF, true);
    exec({0x66, 0xC1, 0xE0, 0x00});  // SHL EAX, 0
    EXPECT_EQ(cpu->ax, 0x00000010u);
    EXPECT_TRUE(CF());
}

TEST_F(Cpu80286Test, OpSize32UnaryGroup) {
    allow_firmware();
    cpu->ax = 0x80000000u;
    exec({0x66, 0xF7, 0xC0, 0x00, 0x00, 0x00, 0x80});  // TEST EAX, 80000000h
    EXPECT_TRUE(SF());
    EXPECT_EQ(cpu->ax, 0x80000000u);
    exec({0x66, 0xF7, 0xD0});  // NOT EAX
    EXPECT_EQ(cpu->ax, 0x7FFFFFFFu);
    exec({0x66, 0xF7, 0xD8});  // NEG EAX
    EXPECT_EQ(cpu->ax, 0x80000001u);
    EXPECT_TRUE(CF());

    cpu->ax = 0x80000000u; cpu->bx = 4;
    exec({0x66, 0xF7, 0xE3});  // MUL EBX
    EXPECT_EQ(cpu->ax, 0u);
    EXPECT_EQ(cpu->dx, 2u);
    EXPECT_TRUE(CF());
    cpu->ax = 0xFFFFFFFEu; cpu->bx = 3;
    exec({0x66, 0xF7, 0xEB});  // IMUL EBX
    EXPECT_EQ(cpu->ax, 0xFFFFFFFAu);
    EXPECT_EQ(cpu->dx, 0xFFFFFFFFu);
    EXPECT_FALSE(CF());
    cpu->dx = 1; cpu->ax = 0; cpu->bx = 2;
    exec({0x66, 0xF7, 0xF3});  // DIV EBX
    EXPECT_EQ(cpu->ax, 0x80000000u);
    EXPECT_EQ(cpu->dx, 0u);
    cpu->dx = 0xFFFFFFFFu; cpu->ax = 0xFFFFFFF9u; cpu->bx = 2;
    exec({0x66, 0xF7, 0xFB});  // IDIV EBX: -7 / 2
    EXPECT_EQ(cpu->ax, 0xFFFFFFFDu);
    EXPECT_EQ(cpu->dx, 0xFFFFFFFFu);
}

TEST_F(Cpu80286Test, OpSize32DivideErrors) {
    allow_firmware();
    handler(0);
    const struct { uint32_t dx, ax, bx; uint8_t modrm; } cases[] = {
        {2, 0, 2, 0xF3},                    // DIV quotient over 32 bits
        {0, 1, 0, 0xF3},                    // DIV by zero
        {0x80000000u, 0, 0xFFFFFFFFu, 0xFB},  // IDIV -2^63 / -1
        {1, 0, 1, 0xFB},                    // IDIV quotient over 7FFFFFFFh
        {0, 1, 0, 0xFB},                    // IDIV by zero
    };
    for (const auto &k : cases) {
        cpu->dx = k.dx; cpu->ax = k.ax; cpu->bx = k.bx;
        exec({0x66, 0xF7, k.modrm});
        EXPECT_TRUE(in_handler()) << std::hex << k.dx << ":" << k.ax << " / " << k.bx;
        EXPECT_EQ(frame(0), 0);
    }
}

TEST_F(Cpu80286Test, OpSize32MultipliesAndExtends) {
    allow_firmware();
    cpu->bx = 0x10000;
    exec({0x66, 0x69, 0xC3, 0x00, 0x00, 0x01, 0x00});  // IMUL EAX, EBX, 10000h
    EXPECT_EQ(cpu->ax, 0u);
    EXPECT_TRUE(OF());
    cpu->bx = 5;
    exec({0x66, 0x6B, 0xC3, 0xFE});  // IMUL EAX, EBX, -2
    EXPECT_EQ(cpu->ax, 0xFFFFFFF6u);
    EXPECT_FALSE(OF());
    cpu->ax = 6; cpu->bx = 7;
    exec({0x66, 0x0F, 0xAF, 0xC3});  // IMUL EAX, EBX
    EXPECT_EQ(cpu->ax, 42u);

    cpu->bx = 0x8000;
    exec({0x66, 0x0F, 0xB7, 0xC3});  // MOVZX EAX, BX
    EXPECT_EQ(cpu->ax, 0x00008000u);
    exec({0x66, 0x0F, 0xBF, 0xC3});  // MOVSX EAX, BX
    EXPECT_EQ(cpu->ax, 0xFFFF8000u);
    cpu->ax = 0xAAAA0000u;
    exec({0x0F, 0xB7, 0xC3});  // MOVZX AX, BX
    EXPECT_EQ(cpu->ax, 0xAAAA8000u);
    exec({0x0F, 0xBF, 0xC3});  // MOVSX AX, BX
    EXPECT_EQ(cpu->ax, 0xAAAA8000u);
}

TEST_F(Cpu80286Test, OpSize32PushAndStringOps) {
    allow_firmware();
    cpu->bx = 0x11223344u;
    exec({0x66, 0xFF, 0xF3});  // PUSH EBX
    EXPECT_EQ(cpu->sp, 0x7FFCu);
    EXPECT_EQ(frame(0), 0x3344);
    EXPECT_EQ(frame(1), 0x1122);

    cpu->ds = 0x0200; cpu->es = 0x0300;
    mem[0x2000] = 0x44; mem[0x2001] = 0x33; mem[0x2002] = 0x22; mem[0x2003] = 0x11;
    cpu->si = 0; cpu->di = 0;
    exec({0x66, 0xA5});  // MOVSD
    EXPECT_EQ(mem[0x3003], 0x11);
    EXPECT_EQ(cpu->si, 4u);
    cpu->si = 0; cpu->di = 0;
    exec({0x66, 0xA7});  // CMPSD
    EXPECT_TRUE(ZF());
    cpu->si = 0;
    exec({0x66, 0xAD});  // LODSD
    EXPECT_EQ(cpu->ax, 0x11223344u);
    cpu->di = 0;
    exec({0x66, 0xAF});  // SCASD
    EXPECT_TRUE(ZF());
    EXPECT_EQ(cpu->di, 4u);
}

// ---------------------------------------------------------------------------
// Single-byte instructions and their remaining forms
// ---------------------------------------------------------------------------

TEST_F(Cpu80286Test, CbwAndCwdSignExtend) {
    cpu->ax = 0x1280;
    run({0x98});  // CBW
    EXPECT_EQ(cpu->ax, 0xFF80u);
    cpu->ax = 0x127F;
    run({0x98});
    EXPECT_EQ(cpu->ax, 0x007Fu);
    cpu->ax = 0x8000; cpu->dx = 0x1234;
    run({0x99});  // CWD
    EXPECT_EQ(cpu->dx, 0xFFFFu);
    cpu->ax = 0x7FFF;
    run({0x99});
    EXPECT_EQ(cpu->dx, 0x0000u);
}

TEST_F(Cpu80286Test, SahfLoadsOnlyTheFlagBitsAndLahfReadsThemBack) {
    cpu->ax = 0xFF00;
    run({0x9E});  // SAHF
    EXPECT_TRUE(SF()); EXPECT_TRUE(ZF()); EXPECT_TRUE(AF()); EXPECT_TRUE(PF()); EXPECT_TRUE(CF());
    EXPECT_EQ(cpu->flags & 0xFF, 0xD7) << "bits 3 and 5 stay clear, bit 1 stays set";
    cpu->ax = 0;
    run({0x9F});  // LAHF
    EXPECT_EQ(cpu->ax, 0xD700u);
}

TEST_F(Cpu80286Test, CmcAndStd) {
    cpu->set_flag(cpu80286::FLAG_CF, false);
    run({0xF5});
    EXPECT_TRUE(CF());
    run({0xF5});
    EXPECT_FALSE(CF());
    run({0xFD});
    EXPECT_TRUE(cpu->flag(cpu80286::FLAG_DF));
}

TEST_F(Cpu80286Test, StringOpsRunBackwardWithDfSet) {
    cpu->set_flag(cpu80286::FLAG_DF, true);
    cpu->es = 0x0300; cpu->di = 0x10;
    cpu->ax = 0x00AA;
    run({0xAA});  // STOSB
    EXPECT_EQ(mem[0x3010], 0xAA);
    EXPECT_EQ(cpu->di, 0x0Fu);
    cpu->ds = 0x0200; cpu->si = 0x10; cpu->dx = 0x60;
    mem[0x2010] = 0x5A;
    run({0x6E});  // OUTSB
    EXPECT_EQ(last_out_port, 0x60);
    EXPECT_EQ(last_out_port_val, 0x5A);
    EXPECT_EQ(cpu->si, 0x0Fu);
}

TEST_F(Cpu80286Test, XlatLooksUpAlInTheTableAtBx) {
    cpu->ds = 0x0200; cpu->es = 0x0300;
    cpu->bx = 0x0100; cpu->ax = 0x0005;
    mem[0x2105] = 0x42;
    mem[0x3105] = 0x24;
    run({0xD7});
    EXPECT_EQ(cpu->ax & 0xFF, 0x42u);
    cpu->ax = 0x0005;
    run({0x26, 0xD7});  // ES: XLAT
    EXPECT_EQ(cpu->ax & 0xFF, 0x24u);
}

TEST_F(Cpu80286Test, WaitFaultsOnlyWithMpAndTsBothSet) {
    handler(7);
    exec({0x9B});
    EXPECT_FALSE(in_handler());
    EXPECT_EQ(cpu->ip, 1);
    cpu->ax = 0x0002;
    exec({0x0F, 0x01, 0xF0, 0x9B}, 2);  // LMSW (MP) ; WAIT
    EXPECT_FALSE(in_handler());
    cpu->ax = 0x000A;
    exec({0x0F, 0x01, 0xF0, 0x9B}, 2);  // LMSW (MP|TS) ; WAIT
    EXPECT_TRUE(in_handler());
    EXPECT_EQ(frame(0), 3);
}

TEST_F(Cpu80286Test, XchgAndPopMemoryAndTestAxImmediate) {
    cpu->ds = 0;
    mem[0x0500] = 0x34; mem[0x0501] = 0x12;
    cpu->ax = 0xBEEF;
    exec({0x87, 0x06, 0x00, 0x05});  // XCHG AX, [0500h]
    EXPECT_EQ(cpu->ax, 0x1234u);
    EXPECT_EQ(mem[0x0500], 0xEF);

    cpu->ax = 0x5678;
    exec({0x50, 0x8F, 0x06, 0x00, 0x05}, 2);  // PUSH AX ; POP [0500h]
    EXPECT_EQ(mem[0x0500], 0x78);
    EXPECT_EQ(cpu->sp, 0x8000u);

    cpu->ax = 0x0F00;
    exec({0xA9, 0xF0, 0x00});  // TEST AX, 00F0h
    EXPECT_TRUE(ZF());
    EXPECT_EQ(cpu->ax, 0x0F00u);
}

TEST_F(Cpu80286Test, PushImm8SignExtends) {
    exec({0x6A, 0xFE});
    EXPECT_EQ(cpu->sp, 0x7FFEu);
    EXPECT_EQ(frame(0), 0xFFFE);
}

TEST_F(Cpu80286Test, RegisterAndMemoryFormsOfTheMoveAndTestGroup) {
    cpu->ds = 0; cpu->bx = 0x0500;
    mem[0x0500] = 0x0F; mem[0x0501] = 0x00;
    cpu->ax = 0x00F0;
    run({0x84, 0x07});  // TEST [BX], AL
    EXPECT_TRUE(ZF());
    run({0x85, 0x07});  // TEST [BX], AX
    EXPECT_TRUE(ZF());

    cpu->bx = 0x0042;
    run({0x8A, 0xC3});  // MOV AL, BL
    EXPECT_EQ(cpu->ax & 0xFF, 0x42u);
    cpu->ds = 0x1234;
    run({0x8C, 0xD8});  // MOV AX, DS
    EXPECT_EQ(cpu->ax, 0x1234u);
    cpu->ds = 0;
    mem[0x0600] = 0x00; mem[0x0601] = 0x30;
    run({0x8E, 0x06, 0x00, 0x06});  // MOV ES, [0600h]
    EXPECT_EQ(cpu->es, 0x3000);
    run({0xC6, 0xC0, 0x99});  // MOV AL, 99h (C6 /0 register form)
    EXPECT_EQ(cpu->ax & 0xFF, 0x99u);
    run({0xC7, 0xC3, 0x34, 0x12});  // MOV BX, 1234h (C7 /0 register form)
    EXPECT_EQ(cpu->bx, 0x1234u);
}

TEST_F(Cpu80286Test, AluRegisterFormsAndThe82Alias) {
    cpu->ax = 0x0005; cpu->bx = 0x0005;
    run({0x3A, 0xC3});  // CMP AL, BL
    EXPECT_TRUE(ZF());
    EXPECT_EQ(cpu->ax, 0x0005u);
    run({0x03, 0xC3});  // ADD AX, BX
    EXPECT_EQ(cpu->ax, 0x000Au);
    run({0x82, 0xC0, 0x05});  // 82 is 80 again: ADD AL, 5
    EXPECT_EQ(cpu->ax, 0x000Fu);
}

TEST_F(Cpu80286Test, TestSlashOneIsAnAliasOfTest) {
    cpu->ax = 0x0080;
    run({0xF6, 0xC8, 0x80});  // TEST AL, 80h via /1
    EXPECT_TRUE(SF());
    cpu->ax = 0x8000;
    run({0xF7, 0xC8, 0x00, 0x80});  // TEST AX, 8000h via /1
    EXPECT_TRUE(SF());
}

TEST_F(Cpu80286Test, UnaryGroupMemoryForms) {
    cpu->ds = 0;
    mem[0x0500] = 0x03; mem[0x0501] = 0x00;
    cpu->ax = 0x0009;
    exec({0xF6, 0x36, 0x00, 0x05});  // DIV BYTE [0500h]
    EXPECT_EQ(cpu->ax, 0x0003u);
    cpu->ax = 0x0009;
    exec({0xF6, 0x3E, 0x00, 0x05});  // IDIV BYTE [0500h]
    EXPECT_EQ(cpu->ax, 0x0003u);
    cpu->dx = 0; cpu->ax = 9;
    exec({0xF7, 0x36, 0x00, 0x05});  // DIV WORD [0500h]
    EXPECT_EQ(cpu->ax, 3u);
    cpu->dx = 0; cpu->ax = 9;
    exec({0xF7, 0x3E, 0x00, 0x05});  // IDIV WORD [0500h]
    EXPECT_EQ(cpu->ax, 3u);
    exec({0xF6, 0x06, 0x00, 0x05, 0x02});  // TEST BYTE [0500h], 2
    EXPECT_FALSE(ZF());
    exec({0xF6, 0x16, 0x00, 0x05});  // NOT BYTE [0500h]
    EXPECT_EQ(mem[0x0500], 0xFC);
    cpu->bx = 3;
    exec({0xF6, 0xF3});  // DIV BL, register form
    EXPECT_EQ(cpu->ax, 0x0001u);
}

TEST_F(Cpu80286Test, DivideOverflowsOnTheNegativeSideToo) {
    handler(0);
    const struct { std::initializer_list<uint8_t> code; uint16_t dx = 0, ax = 0, bx = 0; } cases[] = {
        {{0xF6, 0xF3}, 0, 0x0300, 2},       // DIV BL: 384/2 over FFh
        {{0xF6, 0xFB}, 0, 0xFE00, 2},       // IDIV BL: -512/2 under -128
        {{0xF7, 0xFB}, 0xFFFE, 0x0000, 1},  // IDIV BX: -131072 under -32768
    };
    for (const auto &k : cases) {
        cpu->dx = k.dx; cpu->ax = k.ax; cpu->bx = k.bx;
        exec(k.code);
        EXPECT_TRUE(in_handler()) << std::hex << k.ax;
    }
}

TEST_F(Cpu80286Test, BoundBelowTheLowerLimitFaults) {
    handler(5);
    cpu->ds = 0;
    mem[0x0600] = 0x05; mem[0x0601] = 0x00; mem[0x0602] = 0x0A; mem[0x0603] = 0x00;
    cpu->ax = 2;
    exec({0x62, 0x06, 0x00, 0x06});
    EXPECT_TRUE(in_handler());
}

TEST_F(Cpu80286Test, ShiftCountsOfZeroWriteNothing) {
    cpu->set_flag(cpu80286::FLAG_CF, true);
    cpu->ax = 0x0081; cpu->cx = 0x20;  // CL masks to 0
    run({0xD2, 0xE0});  // SHL AL, CL
    EXPECT_EQ(cpu->ax, 0x0081u);
    EXPECT_TRUE(CF());
    run({0xD3, 0xE0});  // SHL AX, CL
    EXPECT_EQ(cpu->ax, 0x0081u);
    cpu->cx = 0;
    run({0xD2, 0xE0});
    run({0xD3, 0xE0});
    EXPECT_EQ(cpu->ax, 0x0081u);
    cpu->cx = 4;
    run({0xD3, 0xE0});
    EXPECT_EQ(cpu->ax, 0x0810u);
}

TEST_F(Cpu80286Test, RotatesCarryTheOtherBitValue) {
    cpu->set_flag(cpu80286::FLAG_CF, true);
    cpu->ax = 0x0001;
    run({0xD0, 0xC0});  // ROL AL: out bit 0
    EXPECT_EQ(cpu->ax & 0xFF, 0x02u);
    EXPECT_FALSE(CF());
    cpu->ax = 0x0002;
    run({0xD0, 0xC8});  // ROR AL
    EXPECT_EQ(cpu->ax & 0xFF, 0x01u);
    EXPECT_FALSE(CF());
    cpu->ax = 0x0002;
    run({0xD0, 0xD8});  // RCR AL, CF=0 in
    EXPECT_EQ(cpu->ax & 0xFF, 0x01u);

    cpu->ax = 0x0001;
    run({0xD1, 0xC0});  // ROL AX
    EXPECT_EQ(cpu->ax, 0x0002u);
    cpu->ax = 0x0002;
    run({0xD1, 0xC8});  // ROR AX
    EXPECT_EQ(cpu->ax, 0x0001u);
    cpu->set_flag(cpu80286::FLAG_CF, true);
    cpu->ax = 0x0000;
    run({0xD1, 0xD0});  // RCL AX, CF=1 in
    EXPECT_EQ(cpu->ax, 0x0001u);
    cpu->set_flag(cpu80286::FLAG_CF, false);
    cpu->ax = 0x0002;
    run({0xD1, 0xD8});  // RCR AX, CF=0 in
    EXPECT_EQ(cpu->ax, 0x0001u);
}

TEST_F(Cpu80286Test, BcdAdjustsWithCarryAndAuxiliaryCarryIn) {
    cpu->ax = 0x0012;
    cpu->set_flag(cpu80286::FLAG_AF, true);
    cpu->set_flag(cpu80286::FLAG_CF, false);
    run({0x27});  // DAA, AF in
    EXPECT_EQ(cpu->ax & 0xFF, 0x18u);

    cpu->ax = 0x000B;
    cpu->set_flag(cpu80286::FLAG_AF, false);
    cpu->set_flag(cpu80286::FLAG_CF, true);
    run({0x27});  // DAA, CF in
    EXPECT_EQ(cpu->ax & 0xFF, 0x71u);
    EXPECT_TRUE(CF());

    cpu->ax = 0x00FA;
    cpu->set_flag(cpu80286::FLAG_AF, false);
    cpu->set_flag(cpu80286::FLAG_CF, false);
    run({0x27});  // DAA, AL + 6 carries out
    EXPECT_EQ(cpu->ax & 0xFF, 0x60u);
    EXPECT_TRUE(CF());

    cpu->ax = 0x001F;
    cpu->set_flag(cpu80286::FLAG_AF, false);
    cpu->set_flag(cpu80286::FLAG_CF, true);
    run({0x2F});  // DAS, CF in
    EXPECT_EQ(cpu->ax & 0xFF, 0xB9u);
    EXPECT_TRUE(CF());

    cpu->ax = 0x0003;
    cpu->set_flag(cpu80286::FLAG_AF, true);
    run({0x37});  // AAA, AF in with a valid low digit
    EXPECT_EQ(cpu->ax, 0x0109u);
}

// ---------------------------------------------------------------------------
// Descriptor-table loads and stores, MSW forms
// ---------------------------------------------------------------------------

TEST_F(Cpu80286Test, TableLoadsAndStoresNeedAMemoryOperand) {
    handler(6);
    exec({0x0F, 0x01, 0xC0});  // SGDT AX
    EXPECT_TRUE(in_handler());
    exec({0x0F, 0x01, 0xD0});  // LGDT AX
    EXPECT_TRUE(in_handler());
}

TEST_F(Cpu80286Test, TableOperandPastFffaRaisesGeneralProtection) {
    handler(13);
    cpu->ds = 0;
    exec({0x0F, 0x01, 0x06, 0xFC, 0xFF});  // SGDT [FFFCh]
    EXPECT_TRUE(in_handler());
    exec({0x0F, 0x01, 0x16, 0xFC, 0xFF});  // LGDT [FFFCh]
    EXPECT_TRUE(in_handler());
}

TEST_F(Cpu80286Test, SmswAndLmswTakeMemoryOperands) {
    cpu->ds = 0;
    exec({0x0F, 0x01, 0x26, 0x00, 0x05});  // SMSW [0500h]
    EXPECT_EQ(mem[0x0500], 0xF0);
    EXPECT_EQ(mem[0x0501], 0xFF);
    mem[0x0502] = 0x09; mem[0x0503] = 0x00;  // PE|TS
    cpu->on_unimplemented = nullptr;
    exec({0x0F, 0x01, 0x36, 0x02, 0x05});  // LMSW [0502h]
    EXPECT_EQ(cpu->msw() & 0x000F, 0x0008);
}

// ---------------------------------------------------------------------------
// REP edge cases
// ---------------------------------------------------------------------------

TEST_F(Cpu80286Test, RepWithCxZeroDoesNothing) {
    cpu->es = 0x0300; cpu->di = 0; cpu->cx = 0;
    run({0xF3, 0x6C});  // REP INSB
    EXPECT_EQ(cpu->di, 0u);
    EXPECT_EQ(cpu->ip, 2);
}

TEST_F(Cpu80286Test, OutsHonoursASegmentOverride) {
    cpu->es = 0x0300; cpu->si = 0; cpu->dx = 0x61;
    mem[0x3000] = 0x3C;
    run({0x26, 0x6E});  // ES: OUTSB
    EXPECT_EQ(last_out_port_val, 0x3C);
}

TEST_F(Cpu80286Test, RepInsYieldsAndResumes) {
    cpu->rep_yield_cycles = 6;
    cpu->es = 0x0300; cpu->di = 0; cpu->dx = 0x60; cpu->cx = 8;
    next_in_val = 0x11;
    load({0xF3, 0x6C, 0x90});
    cpu->ip = 0;
    cpu->step();
    EXPECT_EQ(cpu->ip, 0);
    EXPECT_GT(cpu->cx, 0u);
    while (cpu->ip == 0) cpu->step();
    EXPECT_EQ(cpu->cx, 0u);
    EXPECT_EQ(cpu->di, 8u);
    EXPECT_EQ(mem[0x3007], 0x11);
}

TEST_F(Cpu80286Test, FaultInsideARepStopsWithCxCounted) {
    handler(13);
    cpu->ds = 0x0200; cpu->es = 0x0300;
    cpu->si = 0xFFFD; cpu->di = 0; cpu->cx = 4;
    exec({0xF3, 0xA5});  // REP MOVSW; the second word straddles FFFFh
    EXPECT_TRUE(in_handler());
    EXPECT_EQ(cpu->cx, 3u);
    cpu->ds = 0; cpu->es = 0x0300;
    cpu->dx = 0x60; cpu->di = 0xFFFD; cpu->cx = 4;
    exec({0xF3, 0x6D});  // REP INSW
    EXPECT_TRUE(in_handler());
    EXPECT_EQ(cpu->cx, 3u);
}

TEST_F(Cpu80286Test, UnhookedReportsAreSilent) {
    cpu->on_unimplemented = nullptr;
    exec({0x0F, 0x05});  // LOADALL
    EXPECT_EQ(cpu->ip, 2);
    allow_firmware();
    exec({0x0F, 0x00, 0xC0});
    exec({0x64});
    EXPECT_EQ(cpu->ip, 1);
}

// ---------------------------------------------------------------------------
// More 0x66 forms
// ---------------------------------------------------------------------------

TEST_F(Cpu80286Test, OpSize32SingleByteForms) {
    allow_firmware();
    cpu->ax = 0xFFFFFFFFu;
    exec({0x66, 0x40});  // INC EAX
    EXPECT_EQ(cpu->ax, 0u);
    exec({0x66, 0x48});  // DEC EAX
    EXPECT_EQ(cpu->ax, 0xFFFFFFFFu);
    cpu->ax = 0x8000;
    exec({0x66, 0x98});  // CWDE
    EXPECT_EQ(cpu->ax, 0xFFFF8000u);
    exec({0x66, 0x99});  // CDQ
    EXPECT_EQ(cpu->dx, 0xFFFFFFFFu);
    cpu->ax = 0x7FFFFFFFu;
    exec({0x66, 0x99});
    EXPECT_EQ(cpu->dx, 0u);
    cpu->ax = 1; cpu->bx = 2;
    exec({0x66, 0x93});  // XCHG EAX, EBX
    EXPECT_EQ(cpu->ax, 2u);
    EXPECT_EQ(cpu->bx, 1u);
    cpu->ax = 0x80000000u;
    exec({0x66, 0xA9, 0x00, 0x00, 0x00, 0x80});  // TEST EAX, 80000000h
    EXPECT_TRUE(SF());
    cpu->ax = 1;
    exec({0x66, 0x05, 0x01, 0x00, 0x00, 0x00});  // ADD EAX, 1
    EXPECT_EQ(cpu->ax, 2u);
    exec({0x66, 0x3D, 0x02, 0x00, 0x00, 0x00});  // CMP EAX, 2
    EXPECT_TRUE(ZF());
    cpu->bx = 5;
    exec({0x66, 0x03, 0xC3});  // ADD EAX, EBX
    EXPECT_EQ(cpu->ax, 7u);
    cpu->bx = 7;
    exec({0x66, 0x3B, 0xC3});  // CMP EAX, EBX
    EXPECT_TRUE(ZF());
    EXPECT_EQ(cpu->ax, 7u);
}

TEST_F(Cpu80286Test, OpSize32StackAndMemoryForms) {
    allow_firmware();
    exec({0x66, 0x68, 0x44, 0x33, 0x22, 0x11});  // PUSH 11223344h
    EXPECT_EQ(cpu->sp, 0x7FFCu);
    EXPECT_EQ(frame(1), 0x1122);
    exec({0x66, 0x6A, 0xFF});  // PUSH -1 as a dword
    EXPECT_EQ(frame(0), 0xFFFF);
    EXPECT_EQ(frame(1), 0xFFFF);

    cpu->ds = 0;
    cpu->ax = 0xCAFEBABEu;
    exec({0x66, 0xA3, 0x00, 0x05});  // MOV [0500h], EAX
    EXPECT_EQ(mem[0x0503], 0xCA);
    cpu->ax = 0;
    exec({0x66, 0xA1, 0x00, 0x05});  // MOV EAX, [0500h]
    EXPECT_EQ(cpu->ax, 0xCAFEBABEu);
    cpu->bx = 0x01020304u;
    exec({0x66, 0x89, 0x1E, 0x00, 0x06});  // MOV [0600h], EBX
    EXPECT_EQ(mem[0x0603], 0x01);
    exec({0x66, 0x8B, 0x0E, 0x00, 0x06});  // MOV ECX, [0600h]
    EXPECT_EQ(cpu->cx, 0x01020304u);
    exec({0x66, 0xC7, 0x06, 0x00, 0x07, 0x78, 0x56, 0x34, 0x12});  // MOV DWORD [0700h], 12345678h
    EXPECT_EQ(mem[0x0703], 0x12);
    exec({0x66, 0xFF, 0x06, 0x00, 0x07});  // INC DWORD [0700h]
    EXPECT_EQ(mem[0x0700], 0x79);
    exec({0x66, 0xFF, 0x0E, 0x00, 0x07});  // DEC DWORD [0700h]
    EXPECT_EQ(mem[0x0700], 0x78);
    exec({0x66, 0x50, 0x66, 0x8F, 0x06, 0x00, 0x08}, 2);  // PUSH EAX ; POP DWORD [0800h]
    EXPECT_EQ(mem[0x0803], 0xCA);
    cpu->bx = 0x10; cpu->si = 0x20;
    exec({0x66, 0x8D, 0x00});  // LEA EAX, [BX+SI]
    EXPECT_EQ(cpu->ax, 0x30u);
}

TEST_F(Cpu80286Test, OpSize32ByteExtendsAndStosd) {
    allow_firmware();
    cpu->bx = 0x80;
    exec({0x66, 0x0F, 0xB6, 0xC3});  // MOVZX EAX, BL
    EXPECT_EQ(cpu->ax, 0x80u);
    exec({0x66, 0x0F, 0xBE, 0xC3});  // MOVSX EAX, BL
    EXPECT_EQ(cpu->ax, 0xFFFFFF80u);
    cpu->es = 0x0300; cpu->di = 0;
    exec({0x66, 0xAB});  // STOSD
    EXPECT_EQ(mem[0x3003], 0xFF);
    EXPECT_EQ(cpu->di, 4u);
}

TEST_F(Cpu80286Test, OpSize32RotatesCarryTheOtherBitValue) {
    allow_firmware();
    cpu->set_flag(cpu80286::FLAG_CF, true);
    cpu->ax = 1;
    exec({0x66, 0xD1, 0xC0});  // ROL EAX
    EXPECT_EQ(cpu->ax, 2u);
    EXPECT_FALSE(CF());
    exec({0x66, 0xD1, 0xC8});  // ROR EAX
    EXPECT_EQ(cpu->ax, 1u);
    cpu->set_flag(cpu80286::FLAG_CF, true);
    cpu->ax = 0;
    exec({0x66, 0xD1, 0xD0});  // RCL EAX, CF=1 in
    EXPECT_EQ(cpu->ax, 1u);
    cpu->set_flag(cpu80286::FLAG_CF, true);
    cpu->ax = 0;
    exec({0x66, 0xD1, 0xD8});  // RCR EAX, CF=1 in
    EXPECT_EQ(cpu->ax, 0x80000000u);
}

TEST_F(Cpu80286Test, OpSize32DivideEdges) {
    allow_firmware();
    cpu->dx = 0; cpu->ax = 5; cpu->bx = 0xFFFFFFFFu;
    exec({0x66, 0xF7, 0xFB});  // IDIV EBX: 5 / -1
    EXPECT_EQ(cpu->ax, 0xFFFFFFFBu);
    handler(0);
    cpu->dx = 0xFFFFFFFFu; cpu->ax = 0; cpu->bx = 1;
    exec({0x66, 0xF7, 0xFB});  // -2^32 / 1 under -2^31
    EXPECT_TRUE(in_handler());
    cpu->ax = 0x80;
    exec({0x66, 0xF7, 0xC8, 0x80, 0x00, 0x00, 0x00});  // TEST EAX, 80h via /1
    EXPECT_FALSE(ZF());
}

}  // namespace
