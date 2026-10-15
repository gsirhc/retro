#include <gtest/gtest.h>

#include "galaxian/i8255.h"


TEST(I8255, ResetLeavesPortsAsInputs) {
    galaxian::I8255 p;
    p.reset();
    p.in_a = [] { return uint8_t(0xA5); };
    EXPECT_TRUE(p.a_in);
    EXPECT_EQ(p.read(0), 0xA5);
}

TEST(I8255, Mode0AllOutputsLatchesAndCallbacks) {
    galaxian::I8255 p;
    p.reset();
    uint8_t seen = 0;
    p.out_a = [&](uint8_t v) { seen = v; };
    p.write(3, 0x80);  // mode 0, all outputs
    EXPECT_FALSE(p.a_in);
    p.write(0, 0x3C);
    EXPECT_EQ(p.a, 0x3C);
    EXPECT_EQ(seen, 0x3C);
    EXPECT_EQ(p.read(0), 0x3C);
}

TEST(I8255, PortCHalvesReadPinsOrLatchByDirection) {
    galaxian::I8255 p;
    p.reset();
    p.in_c = [] { return uint8_t(0xA5); };
    p.write(3, 0x88);  // C upper in, C lower out
    p.write(2, 0x3C);
    EXPECT_EQ(p.read(2), 0xAC);
    p.write(3, 0x81);  // C upper out, C lower in
    p.write(2, 0x3C);
    EXPECT_EQ(p.read(2), 0x35);
    p.write(3, 0x89);
    EXPECT_EQ(p.read(2), 0xA5);
    p.write(3, 0x80);
    p.write(2, 0x3C);
    EXPECT_EQ(p.read(2), 0x3C);
}

TEST(I8255, BitSetResetDrivesOnePortCLine) {
    galaxian::I8255 p;
    p.reset();
    uint8_t seen = 0;
    int writes = 0;
    p.out_c = [&](uint8_t v) {
        seen = v;
        writes++;
    };
    p.write(3, 0x80);
    p.write(3, 0x07);  // set PC3
    EXPECT_EQ(p.c, 0x08);
    EXPECT_EQ(seen, 0x08);
    p.write(3, 0x0F);  // set PC7
    p.write(3, 0x06);  // reset PC3
    EXPECT_EQ(p.c, 0x80);
    EXPECT_EQ(seen, 0x80);
    EXPECT_EQ(writes, 3);
    EXPECT_TRUE(p.cl_in == false && p.cu_in == false) << "BSR leaves the mode alone";
}
