// GoogleTest suite for via65c22::Via. Register offsets and bit meanings
// per the WDC W65C22 datasheet (see via65c22.h).

#include <gtest/gtest.h>

#include "../via65c22.h"

using namespace via65c22;

namespace {

TEST(Via65C22, PortAOutputReflectsDdrAndOra) {
    Via v;
    v.write(0x3, 0x0F);   // DDRA: low nibble out, high nibble in
    v.read_pa = [] { return uint8_t(0xA0); };   // external drive on the input bits
    v.write(0x1, 0x05);   // ORA
    EXPECT_EQ(v.read(0x1), 0xA5);   // 0x05 (driven) | 0xA0 (external, masked to input bits)
}

TEST(Via65C22, Timer1FreeRunInterruptsAndTogglesPB7) {
    Via v;
    v.write(0x3, 0x80);          // DDRA -- irrelevant for this
    v.write(0xB, 0x40);          // ACR: T1 free-run, PB7 output disabled here (bit7=0)
    v.write(0x2, 0x80);          // DDRB bit7 = output, so PB7 reflects the T1 toggle once ACR bit7 is set too
    v.write(0xB, 0xC0);          // ACR: free-run + PB7 toggle enabled
    v.write(0xE, 0xC0);          // IER: enable T1 interrupt (bit7=set, bit6=T1)
    v.write(0x4, 0x02);          // T1L-L = 2
    v.write(0x5, 0x00);          // T1L-H = 0, latches+starts counter at 2, clears IRQ

    EXPECT_FALSE(v.irq());
    v.tick(3);                   // counts 2,1,0 -> underflow on this tick
    EXPECT_TRUE(v.irq());
    uint8_t pb_after_first = v.read(0x0) & 0x80;
    v.read(0x4);                 // reading T1C-L clears the T1 IRQ flag
    EXPECT_FALSE(v.irq());
    v.tick(3);                   // free-run reloads and underflows again
    EXPECT_TRUE(v.irq());
    EXPECT_NE(v.read(0x0) & 0x80, pb_after_first);   // PB7 toggled between underflows
}

TEST(Via65C22, IfrIerGateTheSharedIrqLine) {
    Via v;
    v.set_ca1(true);            // idle high first, so the next low edge is the active one
    v.write(0xC, 0x00);          // PCR: CA1 negative edge (default already, explicit here)
    v.write(0xE, 0x82);          // IER: enable CA1 (bit1) only
    v.set_ca1(false);            // active (negative) edge
    EXPECT_TRUE(v.irq());
    v.write(0xE, 0x02);          // disable CA1 in IER (bit7=0 means clear the listed bits)
    EXPECT_FALSE(v.irq());       // flag is still set in IFR, but no longer enabled -> no IRQ
}

TEST(Via65C22, WritingOrbClearsCb1Flag) {
    Via v;
    v.set_cb1(true);
    v.set_cb1(false);            // active edge sets IFR CB1
    v.write(0xE, 0x90);          // enable CB1
    EXPECT_TRUE(v.irq());
    v.write(0x0, 0xFF);          // any ORB access clears CB1 (PCR=0 -> dependent CB2 clear too, unused here)
    EXPECT_FALSE(v.irq());
}

} // namespace
