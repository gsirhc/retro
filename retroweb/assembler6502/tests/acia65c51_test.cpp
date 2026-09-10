// GoogleTest suite for acia65c51::Acia. Register offsets and status bits
// per the WDC W65C51N datasheet (see acia65c51.h).

#include <gtest/gtest.h>

#include "../acia65c51.h"

using namespace acia65c51;

namespace {

TEST(Acia65C51, BaudTableMatchesCtrlBits) {
    Acia a;
    a.write(3, 0x0F);   // CTRL: 19200 baud (bios.s's own configuration)
    EXPECT_EQ(a.baud(), 19200);
    a.write(3, 0x0E);
    EXPECT_EQ(a.baud(), 9600);
    a.write(3, 0x00);
    EXPECT_EQ(a.baud(), 0);   // external clock, not driven on this board
}

TEST(Acia65C51, TdreClearsOnWriteAndSetsAfterRealTransmitTime) {
    Acia a;
    a.write(3, 0x0F);        // 19200 baud
    uint8_t tx_byte = 0;
    a.on_tx = [&](uint8_t b) { tx_byte = b; };
    EXPECT_TRUE(a.read(1) & 0x10);   // TDRE set at reset
    a.write(0, 0x41);                 // DATA <- 'A'
    EXPECT_FALSE(a.read(1) & 0x10);   // busy immediately -- see acia65c51.h
    a.tick(100);                      // not yet a full byte time at 19200 baud
    EXPECT_FALSE(a.read(1) & 0x10);
    a.tick(1000);                     // now well past 10 bits / 19200 baud (~521 cycles)
    EXPECT_TRUE(a.read(1) & 0x10);
    EXPECT_EQ(tx_byte, 0x41);
}

TEST(Acia65C51, RxSetsRdrfAndOverrunsWithoutAFifo) {
    Acia a;
    EXPECT_FALSE(a.read(1) & 0x08);   // RDRF clear
    a.rx_push('X');
    EXPECT_TRUE(a.read(1) & 0x08);
    a.rx_push('Y');                    // dropped -- single holding register, real 6551/65C51 behavior
    EXPECT_TRUE(a.read(1) & 0x04);     // OVRN set
    EXPECT_EQ(a.read(0), 'X');          // original byte retained, not overwritten
    EXPECT_FALSE(a.read(1) & 0x08);     // reading DATA clears RDRF
}

TEST(Acia65C51, DcdDsrAlwaysAssertedCtsHasNoBearing) {
    // Confirmed from the netlist: ~DTR/~DCD/~DSR/~CTS are all grounded --
    // see acia65c51.h. DCD/DSR bits (5/6) should never be set.
    Acia a;
    EXPECT_EQ(a.read(1) & 0x60, 0);
}

TEST(Acia65C51, RxIrqEnabledWhenCommandBit1Clear) {
    Acia a;
    a.write(2, 0x89);   // bios.s's own CMD value: no parity, no echo, rx interrupts enabled
    EXPECT_FALSE(a.irq());
    a.rx_push('Z');
    EXPECT_TRUE(a.irq());
}

} // namespace
