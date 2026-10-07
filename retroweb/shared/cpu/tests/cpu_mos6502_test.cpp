#include "cpu_mos6502.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

struct Mem {
    std::array<uint8_t, 65536> ram{};
    mos6502::Bus bus() {
        mos6502::Bus b;
        b.read  = [this](uint16_t a) { return ram[a]; };
        b.write = [this](uint16_t a, uint8_t v) { ram[a] = v; };
        return b;
    }
};

}  // namespace

TEST(Mos6502, ResetLoadsVectorLeavesD) {
    Mem m;
    m.ram[0xFFFC] = 0x00;
    m.ram[0xFFFD] = 0x80;
    mos6502::Cpu cpu(m.bus());
    cpu.p = mos6502::FLAG_U | mos6502::FLAG_I | mos6502::FLAG_D;
    cpu.reset();
    EXPECT_EQ(cpu.pc, 0x8000);
    EXPECT_TRUE(cpu.flag(mos6502::FLAG_D));
    EXPECT_TRUE(cpu.flag(mos6502::FLAG_I));
    EXPECT_EQ(cpu.sp, 0xFD);
}

TEST(Mos6502, JmpIndirectPageWrap) {
    Mem m;
    m.ram[0x10FF] = 0x34;
    m.ram[0x1000] = 0x12;  // wrap target for hi
    m.ram[0x1100] = 0x99;  // would be CMOS hi
    m.ram[0x0200] = 0x6C;
    m.ram[0x0201] = 0xFF;
    m.ram[0x0202] = 0x10;
    mos6502::Cpu cpu(m.bus());
    cpu.pc = 0x0200;
    int c = cpu.step();
    EXPECT_EQ(c, 5);
    EXPECT_EQ(cpu.pc, 0x1234);
}

TEST(Mos6502, AslAbsXTakesSevenCycles) {
    Mem m;
    m.ram[0x0200] = 0x1E;
    m.ram[0x0201] = 0x00;
    m.ram[0x0202] = 0x30;
    m.ram[0x3005] = 0x80;
    mos6502::Cpu cpu(m.bus());
    cpu.x = 5;
    cpu.pc = 0x0200;
    EXPECT_EQ(cpu.step(), 7);
    EXPECT_EQ(m.ram[0x3005], 0x00);
    EXPECT_TRUE(cpu.flag(mos6502::FLAG_C));
    EXPECT_TRUE(cpu.flag(mos6502::FLAG_Z));
}

TEST(Mos6502, LaxZp) {
    Mem m;
    m.ram[0x0010] = 0x55;
    m.ram[0x0200] = 0xA7;
    m.ram[0x0201] = 0x10;
    mos6502::Cpu cpu(m.bus());
    cpu.pc = 0x0200;
    cpu.step();
    EXPECT_EQ(cpu.a, 0x55);
    EXPECT_EQ(cpu.x, 0x55);
}

TEST(Mos6502, SaxZp) {
    Mem m;
    m.ram[0x0200] = 0x87;
    m.ram[0x0201] = 0x20;
    mos6502::Cpu cpu(m.bus());
    cpu.a = 0xF0;
    cpu.x = 0x0F;
    cpu.pc = 0x0200;
    cpu.step();
    EXPECT_EQ(m.ram[0x0020], 0x00);
}

TEST(Mos6502, DcpZp) {
    Mem m;
    m.ram[0x0030] = 0x05;
    m.ram[0x0200] = 0xC7;
    m.ram[0x0201] = 0x30;
    mos6502::Cpu cpu(m.bus());
    cpu.a = 0x04;
    cpu.pc = 0x0200;
    cpu.step();
    EXPECT_EQ(m.ram[0x0030], 0x04);
    EXPECT_TRUE(cpu.flag(mos6502::FLAG_Z));
    EXPECT_TRUE(cpu.flag(mos6502::FLAG_C));
}

TEST(Mos6502, KilJams) {
    Mem m;
    m.ram[0x0200] = 0x02;
    mos6502::Cpu cpu(m.bus());
    cpu.pc = 0x0200;
    cpu.step();
    EXPECT_TRUE(cpu.jammed);
    uint16_t pc = cpu.pc;
    cpu.step();
    EXPECT_EQ(cpu.pc, pc);
}

TEST(Mos6502, IrqDoesNotClearD) {
    Mem m;
    m.ram[0xFFFE] = 0x00;
    m.ram[0xFFFF] = 0x90;
    mos6502::Cpu cpu(m.bus());
    cpu.pc = 0x0200;
    cpu.p = mos6502::FLAG_U | mos6502::FLAG_D;  // I clear
    cpu.irq_line = true;
    cpu.step();
    EXPECT_EQ(cpu.pc, 0x9000);
    EXPECT_TRUE(cpu.flag(mos6502::FLAG_D));
    EXPECT_TRUE(cpu.flag(mos6502::FLAG_I));
}

TEST(Mos6502, AncSetsCarryFromN) {
    Mem m;
    m.ram[0x0200] = 0x0B;
    m.ram[0x0201] = 0x80;
    mos6502::Cpu cpu(m.bus());
    cpu.a = 0xFF;
    cpu.pc = 0x0200;
    cpu.step();
    EXPECT_EQ(cpu.a, 0x80);
    EXPECT_TRUE(cpu.flag(mos6502::FLAG_N));
    EXPECT_TRUE(cpu.flag(mos6502::FLAG_C));
}

TEST(Mos6502, Axs) {
    Mem m;
    m.ram[0x0200] = 0xCB;
    m.ram[0x0201] = 0x01;
    mos6502::Cpu cpu(m.bus());
    cpu.a = 0x0F;
    cpu.x = 0xF0;
    cpu.pc = 0x0200;
    cpu.step();
    EXPECT_EQ(cpu.x, 0xFF);  // (0x0F & 0xF0) - 1 = 0 - 1
    EXPECT_FALSE(cpu.flag(mos6502::FLAG_C));
}
