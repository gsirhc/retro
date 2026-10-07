// 8042 keyboard controller: self-test, command byte, A20 gate, CPU reset, scan codes, PS/2 aux (mouse) port
// Mouse protocol per Chapweske, "The PS/2 Mouse Interface" (2001), and Bochs rombios.c (INT 15h AH=C2h, INT 74h).

#include <gtest/gtest.h>

#include "i8042.h"

#include <vector>

namespace {

using pc486::I8042;

// send to the mouse: command 0xD4, then the byte (rombios.c send_to_mouse_ctrl())
void SendAux(I8042& kbc, uint8_t byte) {
    kbc.out(0x64, 0xD4);
    kbc.out(0x60, byte);
}

// rombios.c get_mouse_data() waits for (inb(0x64) & 0x21) == 0x21
uint8_t ReadAux(I8042& kbc) {
    EXPECT_EQ(kbc.in(0x64) & 0x21, 0x21);
    return kbc.in(0x60);
}

// driver-style bring-up: release AUX clock, enable both IRQs, reset, drain the answer, enable reporting
void InitMouse(I8042& kbc) {
    kbc.reset();
    kbc.in(0x60);         // power-on keyboard BAT byte
    kbc.out(0x64, 0x60);  // write command byte
    kbc.out(0x60, 0x03);  // IRQ1 + IRQ12 enabled, AUX clock released (bit5 clear)
    SendAux(kbc, 0xFF);
    ReadAux(kbc);
    ReadAux(kbc);
    ReadAux(kbc);
    SendAux(kbc, 0xF4);   // enable data reporting
    ReadAux(kbc);
    kbc.clear_irq12();
}

struct Packet { uint8_t b0, b1, b2; };
Packet ReadPacket(I8042& kbc) {
    Packet p;
    p.b0 = ReadAux(kbc);
    p.b1 = ReadAux(kbc);
    p.b2 = ReadAux(kbc);
    return p;
}

TEST(I8042Test, ResetDeliversUnsolicitedKeyboardBatByte) {
    // the keyboard sends 0xAA unsolicited after its own power-on self-test; BIOS POST waits for it
    I8042 kbc;
    kbc.reset();
    EXPECT_TRUE(kbc.in(0x64) & 0x01);
    EXPECT_EQ(kbc.in(0x60), 0xAA);
}

TEST(I8042Test, A20DisabledByDefault) {
    I8042 kbc;
    kbc.reset();
    EXPECT_FALSE(kbc.a20_enabled());
}

TEST(I8042Test, WriteOutputPortEnablesA20) {
    I8042 kbc;
    kbc.reset();
    kbc.out(0x64, 0xD1);  // write output port
    kbc.out(0x60, 0x03);  // A20 on, reset line high
    EXPECT_TRUE(kbc.a20_enabled());
    EXPECT_FALSE(kbc.reset_requested());
}

// port 0x92 Fast A20 Gate; HIMEM.SYS tries it first (OSDev Wiki, "A20 Line"). Same A20 line as the 8042.
TEST(I8042Test, FastA20PortEnablesA20) {
    I8042 kbc;
    kbc.reset();
    EXPECT_TRUE(kbc.owns_fast_a20(0x92));
    EXPECT_FALSE(kbc.a20_enabled());
    kbc.fast_a20_out(0x02);  // A20 on, no reset
    EXPECT_TRUE(kbc.a20_enabled());
    EXPECT_FALSE(kbc.reset_requested());
    EXPECT_EQ(kbc.fast_a20_in(), 0x02);
}

TEST(I8042Test, FastA20PortAndOutputPortShareTheSameA20State) {
    I8042 kbc;
    kbc.reset();
    kbc.out(0x64, 0xD1);
    kbc.out(0x60, 0x02);
    EXPECT_TRUE(kbc.a20_enabled());
    EXPECT_EQ(kbc.fast_a20_in(), 0x02);  // same state via port 0x92
    // fast-path disable is visible to the slow read-back
    kbc.fast_a20_out(0x00);
    EXPECT_FALSE(kbc.a20_enabled());
}

TEST(I8042Test, FastA20PortBitZeroTriggersReset) {
    I8042 kbc;
    kbc.reset();
    kbc.fast_a20_out(0x03);  // A20 and reset set
    EXPECT_TRUE(kbc.a20_enabled());
    EXPECT_TRUE(kbc.reset_requested());
}

TEST(I8042Test, OutputPortBitZeroLowTriggersReset) {
    I8042 kbc;
    kbc.reset();
    kbc.out(0x64, 0xD1);
    kbc.out(0x60, 0x00);  // reset line pulsed low
    EXPECT_TRUE(kbc.reset_requested());
    kbc.clear_reset_request();
    EXPECT_FALSE(kbc.reset_requested());
}

TEST(I8042Test, PulseOutputLineZeroCommandAlsoTriggersReset) {
    I8042 kbc;
    kbc.reset();
    kbc.out(0x64, 0xFE);
    EXPECT_TRUE(kbc.reset_requested());
}

TEST(I8042Test, SelfTestRespondsWithFiftyFive) {
    I8042 kbc;
    kbc.reset();
    kbc.out(0x64, 0xAA);
    EXPECT_TRUE(kbc.in(0x64) & 0x01);  // output buffer full
    EXPECT_EQ(kbc.in(0x60), 0x55);
    EXPECT_FALSE(kbc.in(0x64) & 0x01);  // reading 0x60 drains the buffer
}

TEST(I8042Test, CommandByteReadWriteRoundTrip) {
    I8042 kbc;
    kbc.reset();
    kbc.out(0x64, 0x60);  // write command byte
    kbc.out(0x60, 0x45);
    kbc.out(0x64, 0x20);  // read command byte back
    EXPECT_EQ(kbc.in(0x60), 0x45);
}

TEST(I8042Test, ScancodeSetsIrq1OnlyWhenEnabledInCommandByte) {
    I8042 kbc;
    kbc.reset();
    kbc.in(0x60);  // drain power-on BAT; a scan code queues behind it
    kbc.inject_scancode(0x1E);  // IRQ1 not yet enabled
    EXPECT_FALSE(kbc.irq1_pending());
    EXPECT_EQ(kbc.in(0x60), 0x1E);  // still delivered

    kbc.out(0x64, 0x60);
    kbc.out(0x60, 0x01);  // bit0 = enable IRQ1
    kbc.inject_scancode(0x1F);
    EXPECT_TRUE(kbc.irq1_pending());
    EXPECT_EQ(kbc.in(0x60), 0x1F);
    kbc.clear_irq1();
    EXPECT_FALSE(kbc.irq1_pending());
}

TEST(I8042Test, ResetCommandGetsAckThenBatByteOnSeparateReads) {
    // rombios.c keyboard POST: send 0xFF, expect 0xFA, then 0xAA as a distinct second byte
    I8042 kbc;
    kbc.reset();
    kbc.in(0x60);  // drain the power-on BAT byte first
    kbc.out(0x60, 0xFF);  // RESET command
    EXPECT_EQ(kbc.in(0x60), 0xFA);
    EXPECT_TRUE(kbc.in(0x64) & 0x01);  // a second byte is already waiting
    EXPECT_EQ(kbc.in(0x60), 0xAA);
    EXPECT_FALSE(kbc.in(0x64) & 0x01);
}

TEST(I8042Test, ReadIdRespondsWithAckThenTwoIdBytes) {
    // Chapweske: 0xF2 returns 0xAB 0x83 after the ACK every command gets
    I8042 kbc;
    kbc.reset();
    kbc.in(0x60);  // drain the power-on BAT byte
    kbc.out(0x60, 0xF2);  // Read ID
    EXPECT_EQ(kbc.in(0x60), 0xFA);
    EXPECT_EQ(kbc.in(0x60), 0xAB);
    EXPECT_EQ(kbc.in(0x60), 0x83);
    EXPECT_FALSE(kbc.in(0x64) & 0x01);  // nothing left queued
}

// MS-DOS 6.22 SETUP.EXE relies on IRQ1 for the 0xF2 response; the ACK must raise IRQ1 too
TEST(I8042Test, KeyboardCommandAckRaisesIrq1WhenEnabled) {
    // Chapweske, "Writing to keyboard": every keyboard response, ACK included, activates IRQ1
    I8042 kbc;
    kbc.reset();
    kbc.in(0x60);  // drain the power-on BAT byte

    kbc.out(0x60, 0xF4);  // IRQ1 not yet enabled
    EXPECT_FALSE(kbc.irq1_pending());
    EXPECT_EQ(kbc.in(0x60), 0xFA);  // ACK still delivered

    kbc.out(0x64, 0x60);
    kbc.out(0x60, 0x01);  // bit0 = enable IRQ1
    kbc.out(0x60, 0xF4);  // enable scanning again, now with IRQ1 enabled
    EXPECT_TRUE(kbc.irq1_pending());
    EXPECT_EQ(kbc.in(0x60), 0xFA);
    kbc.clear_irq1();
    EXPECT_FALSE(kbc.irq1_pending());
}

TEST(I8042Test, DisabledKeyboardDropsScancodes) {
    I8042 kbc;
    kbc.reset();
    kbc.in(0x60);  // drain BAT
    kbc.out(0x64, 0xAD);  // disable keyboard
    kbc.inject_scancode(0x1E);
    EXPECT_FALSE(kbc.in(0x64) & 0x01);  // nothing delivered
}

TEST(I8042Test, KeyboardBytesQueueInsteadOfOverwritingOneAnother) {
    // the 8042 holds the device clock low while the output buffer is full; a second scan code cannot overwrite the first
    I8042 kbc;
    kbc.reset();
    kbc.in(0x60);  // power-on BAT
    kbc.inject_scancode(0x1E);
    kbc.inject_scancode(0x9E);
    EXPECT_EQ(kbc.in(0x60), 0x1E);
    EXPECT_EQ(kbc.in(0x60), 0x9E);
    EXPECT_FALSE(kbc.in(0x64) & 0x01);
}

// --------------------------------------------------------------------------
// The PS/2 auxiliary port: controller side
// --------------------------------------------------------------------------

TEST(I8042Test, AuxBytesCarryStatusBitFiveAndKeyboardBytesDoNot) {
    // AUXB (status bit 5) separates mouse bytes from keys; rombios.c INT 74h wants 0x21
    I8042 kbc;
    kbc.reset();
    kbc.in(0x60);
    kbc.inject_scancode(0x1E);
    EXPECT_EQ(kbc.in(0x64) & 0x21, 0x01);  // OBF set, AUXB clear
    kbc.in(0x60);

    kbc.out(0x64, 0xD3);  // write to the AUX side of the output buffer
    kbc.out(0x60, 0x5A);
    EXPECT_EQ(kbc.in(0x64) & 0x21, 0x21);
    EXPECT_EQ(kbc.in(0x60), 0x5A);
}

TEST(I8042Test, WriteKeyboardOutputBufferActsLikeKeyboardData) {
    I8042 kbc;
    kbc.reset();
    kbc.in(0x60);
    kbc.out(0x64, 0x60);
    kbc.out(0x60, 0x01);  // IRQ1 enabled
    kbc.out(0x64, 0xD2);
    kbc.out(0x60, 0x3B);
    EXPECT_EQ(kbc.in(0x64) & 0x21, 0x01);  // keyboard-tagged
    EXPECT_TRUE(kbc.irq1_pending());
    EXPECT_FALSE(kbc.irq12_pending());
    EXPECT_EQ(kbc.in(0x60), 0x3B);
}

TEST(I8042Test, AuxInterfaceTestReportsNoError) {
    // 0xA9 answers 0x00 from the controller, so not AUX-tagged
    I8042 kbc;
    kbc.reset();
    kbc.in(0x60);
    kbc.out(0x64, 0xA9);
    EXPECT_EQ(kbc.in(0x64) & 0x21, 0x01);
    EXPECT_EQ(kbc.in(0x60), 0x00);
}

TEST(I8042Test, EnableDisableAuxTrackCommandByteBitFive) {
    // 0xA7/0xA8 and command-byte bit 5 are one piece of state; rombios.c drives the bit directly
    I8042 kbc;
    kbc.reset();
    kbc.in(0x60);
    kbc.out(0x64, 0xA7);
    kbc.out(0x64, 0x20);
    EXPECT_EQ(kbc.in(0x60) & 0x20, 0x20);
    kbc.out(0x64, 0xA8);
    kbc.out(0x64, 0x20);
    EXPECT_EQ(kbc.in(0x60) & 0x20, 0x00);
}

TEST(I8042Test, ControllerCommandCancelsAPendingWriteToMouse) {
    // rombios.c set_kbd_command_byte() abandons a 0xD4 by writing 0x60 to port 0x64; the new command replaces it
    I8042 kbc;
    kbc.reset();
    kbc.in(0x60);
    kbc.out(0x64, 0xD4);  // abandoned
    kbc.out(0x64, 0x60);  // write command byte
    kbc.out(0x60, 0x47);
    kbc.out(0x64, 0x20);
    EXPECT_EQ(kbc.in(0x60), 0x47);
    EXPECT_FALSE(kbc.in(0x64) & 0x20);  // nothing came back from the mouse
}

TEST(I8042Test, Irq12OnlyWhenEnabledInCommandByte) {
    // command-byte bit 1 enables IRQ12, independent of IRQ1
    I8042 kbc;
    kbc.reset();
    kbc.in(0x60);
    kbc.out(0x64, 0x60);
    kbc.out(0x60, 0x01);  // IRQ1 only
    SendAux(kbc, 0xF4);
    EXPECT_FALSE(kbc.irq12_pending());
    EXPECT_EQ(ReadAux(kbc), 0xFA);  // the byte is still delivered

    kbc.out(0x64, 0x60);
    kbc.out(0x60, 0x03);  // IRQ1 + IRQ12
    SendAux(kbc, 0xF5);
    EXPECT_TRUE(kbc.irq12_pending());
    EXPECT_FALSE(kbc.irq1_pending());
    EXPECT_EQ(ReadAux(kbc), 0xFA);
    kbc.clear_irq12();
    EXPECT_FALSE(kbc.irq12_pending());
}

TEST(I8042Test, Irq12ReassertsForEveryByteOfAPacket) {
    // IRQ12 follows the output buffer: one interrupt per packet byte, as INT 74h expects
    I8042 kbc;
    InitMouse(kbc);
    kbc.inject_mouse_event(1, 0, 0);
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(kbc.irq12_pending()) << "byte " << i;
        EXPECT_EQ(kbc.in(0x64) & 0x21, 0x21);
        kbc.in(0x60);
    }
    EXPECT_FALSE(kbc.irq12_pending());
    EXPECT_FALSE(kbc.in(0x64) & 0x01);
}

// --------------------------------------------------------------------------
// The mouse itself
// --------------------------------------------------------------------------

TEST(I8042Test, MouseResetAnswersAckThenBatThenDeviceId) {
    // Chapweske: BAT then device ID 0x00; rombios.c INT 15h AH=C2h AL=01h reads three bytes and panics unless the first is 0xFA
    I8042 kbc;
    kbc.reset();
    kbc.in(0x60);
    kbc.out(0x64, 0x60);
    kbc.out(0x60, 0x03);
    SendAux(kbc, 0xFF);
    EXPECT_EQ(ReadAux(kbc), 0xFA);
    EXPECT_EQ(ReadAux(kbc), 0xAA);
    EXPECT_EQ(ReadAux(kbc), 0x00);
    EXPECT_FALSE(kbc.in(0x64) & 0x01);
}

TEST(I8042Test, NoPacketsUntilReportingIsEnabled) {
    // reset leaves data reporting disabled until 0xF4
    I8042 kbc;
    kbc.reset();
    kbc.in(0x60);
    kbc.out(0x64, 0x60);
    kbc.out(0x60, 0x03);
    SendAux(kbc, 0xFF);
    ReadAux(kbc);
    ReadAux(kbc);
    ReadAux(kbc);

    kbc.inject_mouse_event(5, 5, I8042::kMouseLeft);
    EXPECT_FALSE(kbc.in(0x64) & 0x01);
    EXPECT_FALSE(kbc.mouse_reporting_enabled());

    SendAux(kbc, 0xF4);
    EXPECT_EQ(ReadAux(kbc), 0xFA);
    EXPECT_TRUE(kbc.mouse_reporting_enabled());
}

TEST(I8042Test, MovementPacketMatchesTheReferenceByteSequences) {
    // Chapweske "Emulated Action" table. Bit 3 of byte 1 is always set; +Y is away from the user.
    struct Case { int dx, dy; uint8_t b0, b1, b2; };
    const Case cases[] = {
        {0, 1, 0x08, 0x00, 0x01},    // move up one
        {0, -1, 0x28, 0x00, 0xFF},   // move down one
        {1, 0, 0x08, 0x01, 0x00},    // move right one
        {-1, 0, 0x18, 0xFF, 0x00},   // move left one
    };
    for (const Case& c : cases) {
        I8042 kbc;
        InitMouse(kbc);
        kbc.inject_mouse_event(c.dx, c.dy, 0);
        Packet p = ReadPacket(kbc);
        EXPECT_EQ(p.b0, c.b0) << "dx=" << c.dx << " dy=" << c.dy;
        EXPECT_EQ(p.b1, c.b1) << "dx=" << c.dx << " dy=" << c.dy;
        EXPECT_EQ(p.b2, c.b2) << "dx=" << c.dx << " dy=" << c.dy;
    }
}

TEST(I8042Test, ButtonBitsMatchTheReferenceByteSequences) {
    // same table, buttons only
    struct Case { uint8_t buttons; uint8_t b0; };
    const Case cases[] = {
        {I8042::kMouseLeft, 0x09},
        {I8042::kMouseMiddle, 0x0C},
        {I8042::kMouseRight, 0x0A},
        {0, 0x08},  // all released
    };
    I8042 kbc;
    InitMouse(kbc);
    for (const Case& c : cases) {
        kbc.inject_mouse_event(0, 0, c.buttons);
        Packet p = ReadPacket(kbc);
        EXPECT_EQ(p.b0, c.b0) << "buttons=" << static_cast<int>(c.buttons);
        EXPECT_EQ(p.b1, 0x00);
        EXPECT_EQ(p.b2, 0x00);
    }
}

TEST(I8042Test, MovementAccumulatesWhileAPacketIsStillGoingOut) {
    // a busy line banks movement counts and sends the total after
    I8042 kbc;
    InitMouse(kbc);
    kbc.inject_mouse_event(3, 0, 0);
    EXPECT_EQ(ReadAux(kbc), 0x08);
    kbc.inject_mouse_event(4, 0, 0);   // mid-packet
    EXPECT_EQ(ReadAux(kbc), 0x03);     // first packet's X is untouched
    kbc.inject_mouse_event(5, 0, 0);
    EXPECT_EQ(ReadAux(kbc), 0x00);
    Packet next = ReadPacket(kbc);     // and the banked counts follow
    EXPECT_EQ(next.b0, 0x08);
    EXPECT_EQ(next.b1, 0x09);          // 4 + 5
    EXPECT_EQ(next.b2, 0x00);
    EXPECT_FALSE(kbc.in(0x64) & 0x01);
}

TEST(I8042Test, CountersSaturateAtNineBitsAndSetTheOverflowFlag) {
    // Chapweske: counters saturate at +/-255 with overflow bits set; the excess is lost
    I8042 kbc;
    InitMouse(kbc);
    kbc.inject_mouse_event(400, -400, 0);
    Packet p = ReadPacket(kbc);
    EXPECT_EQ(p.b0 & 0x40, 0x40);  // X overflow
    EXPECT_EQ(p.b0 & 0x80, 0x80);  // Y overflow
    EXPECT_EQ(p.b0 & 0x20, 0x20);  // Y sign (negative)
    EXPECT_EQ(p.b0 & 0x10, 0x00);  // X positive
    EXPECT_EQ(p.b1, 0xFF);         // +255
    EXPECT_EQ(p.b2, 0x01);         // -255
    kbc.inject_mouse_event(1, 0, 0);
    Packet after = ReadPacket(kbc);
    EXPECT_EQ(after.b0 & 0xC0, 0x00);  // counters reset with the packet
    EXPECT_EQ(after.b1, 0x01);
}

TEST(I8042Test, DisabledAuxClockHoldsPacketsButStillAnswersCommands) {
    // AUX clock inhibit stops reporting but host commands still work; rombios.c INT 15h AH=C2h sets bit 5 first
    I8042 kbc;
    InitMouse(kbc);
    kbc.out(0x64, 0xA7);  // disable AUX interface
    EXPECT_FALSE(kbc.mouse_reporting_enabled());
    kbc.inject_mouse_event(7, 0, 0);
    EXPECT_FALSE(kbc.in(0x64) & 0x01);  // nothing reported

    SendAux(kbc, 0xE6);                 // set scaling 1:1
    EXPECT_EQ(ReadAux(kbc), 0xFA);      // ...still acknowledged

    kbc.out(0x64, 0xA8);  // release the clock again
    kbc.inject_mouse_event(1, 0, 0);
    Packet p = ReadPacket(kbc);
    EXPECT_EQ(p.b1, 0x01);  // the 7 was cleared by the command, per Chapweske
}

TEST(I8042Test, RemoteModeReportsOnlyWhenPolled) {
    // Chapweske: remote mode reports only on 0xEB
    I8042 kbc;
    InitMouse(kbc);
    SendAux(kbc, 0xF0);  // set remote mode
    EXPECT_EQ(ReadAux(kbc), 0xFA);
    kbc.inject_mouse_event(9, 0, I8042::kMouseRight);
    EXPECT_FALSE(kbc.in(0x64) & 0x01);

    SendAux(kbc, 0xEB);  // read data
    EXPECT_EQ(ReadAux(kbc), 0xFA);
    Packet p = ReadPacket(kbc);
    EXPECT_EQ(p.b0, 0x0A);  // right button, bit 3 set
    EXPECT_EQ(p.b1, 0x09);
    EXPECT_FALSE(kbc.in(0x64) & 0x01);  // and only the one packet

    SendAux(kbc, 0xEA);  // back to stream mode
    EXPECT_EQ(ReadAux(kbc), 0xFA);
    kbc.inject_mouse_event(2, 0, 0);
    EXPECT_EQ(ReadPacket(kbc).b1, 0x02);
}

TEST(I8042Test, TwoToOneScalingAppliesToStreamReportsButNotToReadData) {
    // 2:1 scaling applies only to stream reports, not 0xEB (Chapweske, footnote 1)
    I8042 kbc;
    InitMouse(kbc);
    SendAux(kbc, 0xE7);  // set scaling 2:1
    EXPECT_EQ(ReadAux(kbc), 0xFA);

    kbc.inject_mouse_event(4, -5, 0);
    Packet stream = ReadPacket(kbc);
    EXPECT_EQ(stream.b1, 0x06);  // 4 -> 6
    EXPECT_EQ(stream.b2, 0xF7);  // -5 -> -9
    EXPECT_EQ(stream.b0 & 0x20, 0x20);

    SendAux(kbc, 0xF0);  // remote mode, so the poll is the only report
    ReadAux(kbc);
    kbc.inject_mouse_event(4, 0, 0);
    SendAux(kbc, 0xEB);
    EXPECT_EQ(ReadAux(kbc), 0xFA);
    EXPECT_EQ(ReadPacket(kbc).b1, 0x04);  // unscaled
}

TEST(I8042Test, StatusRequestReportsDefaultsThenTheProgrammedState) {
    // Chapweske: status at defaults is 0xFA, 0x00, 0x02, 0x64
    I8042 kbc;
    InitMouse(kbc);
    SendAux(kbc, 0xF5);  // disable reporting so the flags start clear
    ReadAux(kbc);
    SendAux(kbc, 0xE9);
    EXPECT_EQ(ReadAux(kbc), 0xFA);
    EXPECT_EQ(ReadAux(kbc), 0x00);
    EXPECT_EQ(ReadAux(kbc), 0x02);
    EXPECT_EQ(ReadAux(kbc), 0x64);

    SendAux(kbc, 0xE8);  // set resolution
    ReadAux(kbc);
    SendAux(kbc, 0x03);  // 8 counts/mm
    ReadAux(kbc);
    SendAux(kbc, 0xF3);  // set sample rate
    ReadAux(kbc);
    SendAux(kbc, 200);
    ReadAux(kbc);
    SendAux(kbc, 0xE7);  // scaling 2:1
    ReadAux(kbc);
    SendAux(kbc, 0xF0);  // remote mode
    ReadAux(kbc);
    SendAux(kbc, 0xF4);  // reporting enabled
    ReadAux(kbc);
    kbc.inject_mouse_event(0, 0, I8042::kMouseLeft | I8042::kMouseRight);

    SendAux(kbc, 0xE9);
    EXPECT_EQ(ReadAux(kbc), 0xFA);
    // status flags byte; button order is reversed from the movement packet
    EXPECT_EQ(ReadAux(kbc), 0x40 | 0x20 | 0x10 | 0x04 | 0x01);
    EXPECT_EQ(ReadAux(kbc), 0x03);
    EXPECT_EQ(ReadAux(kbc), 200);
}

TEST(I8042Test, SetDefaultsRestoresTheResetState) {
    // 0xF6 loads the BAT defaults
    I8042 kbc;
    InitMouse(kbc);
    SendAux(kbc, 0xE7);  // 2:1 scaling
    ReadAux(kbc);
    SendAux(kbc, 0xF0);  // remote mode
    ReadAux(kbc);
    SendAux(kbc, 0xF6);
    EXPECT_EQ(ReadAux(kbc), 0xFA);
    EXPECT_FALSE(kbc.mouse_reporting_enabled());  // reporting back off

    SendAux(kbc, 0xE9);
    EXPECT_EQ(ReadAux(kbc), 0xFA);
    EXPECT_EQ(ReadAux(kbc), 0x00);  // stream mode, reporting off, 1:1
    EXPECT_EQ(ReadAux(kbc), 0x02);
    EXPECT_EQ(ReadAux(kbc), 0x64);
}

TEST(I8042Test, DeviceIdStaysZeroThroughTheIntelliMouseKnock) {
    // the 200/100/80 wheel probe: a standard 3-button mouse accepts every rate and keeps ID 0x00
    I8042 kbc;
    InitMouse(kbc);
    const uint8_t knock[] = {0xF3, 200, 0xF3, 100, 0xF3, 80};
    for (uint8_t b : knock) {
        SendAux(kbc, b);
        EXPECT_EQ(ReadAux(kbc), 0xFA);
    }
    SendAux(kbc, 0xF2);
    EXPECT_EQ(ReadAux(kbc), 0xFA);
    EXPECT_EQ(ReadAux(kbc), 0x00);

    kbc.inject_mouse_event(1, 0, 0);  // and packets stay 3 bytes long
    ReadPacket(kbc);
    EXPECT_FALSE(kbc.in(0x64) & 0x01);
}

TEST(I8042Test, WrapModeEchoesEverythingExceptResetAndResetWrap) {
    // Chapweske: wrap mode echoes everything except Reset (0xFF) and Reset Wrap Mode (0xEC)
    I8042 kbc;
    InitMouse(kbc);
    SendAux(kbc, 0xEE);  // set wrap mode
    EXPECT_EQ(ReadAux(kbc), 0xFA);

    SendAux(kbc, 0xF2);  // would be "get device ID"
    EXPECT_EQ(ReadAux(kbc), 0xF2);
    SendAux(kbc, 0x7B);  // not a command at all
    EXPECT_EQ(ReadAux(kbc), 0x7B);

    SendAux(kbc, 0xEC);  // obeyed, not echoed
    EXPECT_EQ(ReadAux(kbc), 0xFA);
    SendAux(kbc, 0xF2);
    EXPECT_EQ(ReadAux(kbc), 0xFA);
    EXPECT_EQ(ReadAux(kbc), 0x00);
}

TEST(I8042Test, WrapModeIsLeftByResetToo) {
    I8042 kbc;
    InitMouse(kbc);
    SendAux(kbc, 0xEE);
    ReadAux(kbc);
    SendAux(kbc, 0xFF);  // the other exception
    EXPECT_EQ(ReadAux(kbc), 0xFA);
    EXPECT_EQ(ReadAux(kbc), 0xAA);
    EXPECT_EQ(ReadAux(kbc), 0x00);
    SendAux(kbc, 0xF2);
    EXPECT_EQ(ReadAux(kbc), 0xFA);
    EXPECT_EQ(ReadAux(kbc), 0x00);
}

TEST(I8042Test, ResendRepeatsTheLastPacketAndLeavesCountersAlone) {
    // 0xFE resend is the one command that does not reset the movement counters
    I8042 kbc;
    InitMouse(kbc);
    kbc.inject_mouse_event(6, 0, I8042::kMouseLeft);
    Packet first = ReadPacket(kbc);
    SendAux(kbc, 0xFE);  // "that last one was garbled -- say it again"
    Packet again = ReadPacket(kbc);
    EXPECT_EQ(again.b0, first.b0);
    EXPECT_EQ(again.b1, first.b1);
    EXPECT_EQ(again.b2, first.b2);
    EXPECT_FALSE(kbc.in(0x64) & 0x01);  // a repeat, not a second report

    // remote mode lets counters be watched across a resend
    SendAux(kbc, 0xF0);
    ReadAux(kbc);
    kbc.inject_mouse_event(5, 0, 0);
    SendAux(kbc, 0xEB);
    ReadAux(kbc);
    Packet polled = ReadPacket(kbc);
    EXPECT_EQ(polled.b1, 0x05);
    kbc.inject_mouse_event(2, 0, 0);
    SendAux(kbc, 0xFE);
    Packet repeat = ReadPacket(kbc);
    EXPECT_EQ(repeat.b1, 0x05);  // the poll's packet, repeated
    SendAux(kbc, 0xEB);
    ReadAux(kbc);
    EXPECT_EQ(ReadPacket(kbc).b1, 0x02);  // and the 2 was never cleared
}

TEST(I8042Test, UnknownMouseCommandAnswersResendNotAck) {
    // an unrecognised command answers 0xFE
    I8042 kbc;
    InitMouse(kbc);
    SendAux(kbc, 0x9C);
    EXPECT_EQ(ReadAux(kbc), 0xFE);
}

TEST(I8042Test, MouseCommandsClearTheMovementCounters) {
    // Chapweske: counters reset on any command except Resend
    I8042 kbc;
    InitMouse(kbc);
    SendAux(kbc, 0xF5);  // reporting off, so counts just accumulate
    ReadAux(kbc);
    kbc.inject_mouse_event(40, 40, 0);
    SendAux(kbc, 0xE6);  // set scaling 1:1 -- clears the counters
    ReadAux(kbc);
    SendAux(kbc, 0xEB);  // read data
    ReadAux(kbc);
    Packet p = ReadPacket(kbc);
    EXPECT_EQ(p.b1, 0x00);
    EXPECT_EQ(p.b2, 0x00);
}

TEST(I8042Test, KeyboardAndMouseBytesInterleaveWithoutLosingEither) {
    // keyboard and mouse bytes interleave; each keeps its own AUXB tag
    I8042 kbc;
    InitMouse(kbc);
    kbc.inject_mouse_event(1, 0, 0);
    kbc.inject_scancode(0x11);  // W down, mid-packet

    EXPECT_EQ(kbc.in(0x64) & 0x21, 0x21);
    EXPECT_EQ(kbc.in(0x60), 0x08);
    EXPECT_EQ(kbc.in(0x64) & 0x21, 0x21);
    EXPECT_EQ(kbc.in(0x60), 0x01);
    EXPECT_EQ(kbc.in(0x64) & 0x21, 0x21);
    EXPECT_EQ(kbc.in(0x60), 0x00);
    EXPECT_EQ(kbc.in(0x64) & 0x21, 0x01);  // now the key, keyboard-tagged
    EXPECT_TRUE(kbc.irq1_pending());
    EXPECT_EQ(kbc.in(0x60), 0x11);
    EXPECT_FALSE(kbc.in(0x64) & 0x01);
}

TEST(I8042Test, AKeyboardByteSurvivesABurstOfMousePacketsBiggerThanTheOldQueue) {
    // a key break mid mouse-look burst was dropped when the queue filled; see i8042.h kQueueSize
    I8042 kbc;
    InitMouse(kbc);
    for (int i = 0; i < 200; ++i) kbc.inject_mouse_event(1, 0, 0);
    kbc.inject_scancode(0xD1);

    bool found_break_code = false;
    for (int i = 0; i < 700; ++i) {
        uint8_t status = kbc.in(0x64);
        if ((status & 0x01) == 0) break;
        bool aux = (status & 0x20) != 0;
        uint8_t byte = kbc.in(0x60);
        if (!aux && byte == 0xD1) found_break_code = true;
    }
    EXPECT_TRUE(found_break_code);
}

TEST(I8042Test, BiosPointingDeviceInitSequenceRunsEndToEnd) {
    // rombios.c INT 15h AH=C2h reset/enable sequence: inhibit IRQ12 and the AUX clock, talk to the device, restore
    I8042 kbc;
    kbc.reset();
    kbc.in(0x60);

    kbc.out(0x64, 0x20);
    uint8_t comm = kbc.in(0x60);
    comm &= ~0x02;
    comm |= 0x20;
    kbc.out(0x64, 0x60);
    kbc.out(0x60, comm);

    SendAux(kbc, 0xFF);
    EXPECT_EQ(ReadAux(kbc), 0xFA);
    EXPECT_EQ(ReadAux(kbc), 0xAA);  // BAT passed
    EXPECT_EQ(ReadAux(kbc), 0x00);  // device ID -> INT 15h returns it in BL
    EXPECT_FALSE(kbc.irq12_pending());  // IRQ12 was inhibited throughout

    SendAux(kbc, 0xF4);
    EXPECT_EQ(ReadAux(kbc), 0xFA);

    kbc.out(0x64, 0x20);
    comm = kbc.in(0x60);
    comm |= 0x02;
    comm &= ~0x20;
    kbc.out(0x64, 0x60);
    kbc.out(0x60, comm);

    EXPECT_TRUE(kbc.mouse_reporting_enabled());
    kbc.inject_mouse_event(-2, 3, I8042::kMouseMiddle);
    EXPECT_TRUE(kbc.irq12_pending());
    Packet p = ReadPacket(kbc);
    EXPECT_EQ(p.b0, 0x08 | 0x04 | 0x10);  // bit3, middle button, X negative
    EXPECT_EQ(p.b1, 0xFE);
    EXPECT_EQ(p.b2, 0x03);
}

// --- typematic repeat ------------------------------------------------------

class TypematicTest : public ::testing::Test {
protected:
    static constexpr double kHz = 66000000.0;
    I8042 kbc;
    double t = 0.0;

    void SetUp() override { kbc.reset(); Drain(); }
    std::vector<uint8_t> Drain() {
        std::vector<uint8_t> out;
        while (kbc.in(0x64) & 0x01) out.push_back(kbc.in(0x60));
        return out;
    }
    // advances the clock in 1 ms steps, returns everything sent
    std::vector<uint8_t> RunTo(double seconds) {
        std::vector<uint8_t> out;
        for (; t < seconds; t += 0.001) {
            kbc.tick(uint64_t(t * kHz), kHz);
            for (uint8_t b : Drain()) out.push_back(b);
        }
        return out;
    }
};

TEST_F(TypematicTest, AHeldKeyRepeatsAfterHalfASecondAtTenPointNineCps) {
    kbc.inject_scancode(0x1E);
    EXPECT_EQ(Drain(), std::vector<uint8_t>{0x1E});
    EXPECT_TRUE(RunTo(0.49).empty()) << "nothing before the 500 ms delay";
    auto first = RunTo(0.51);
    EXPECT_EQ(first, std::vector<uint8_t>{0x1E});
    auto second = RunTo(1.51);
    EXPECT_GE(second.size(), 10u);
    EXPECT_LE(second.size(), 11u) << "one every 91.7 ms";
    kbc.inject_scancode(0x9E);
    EXPECT_EQ(Drain(), std::vector<uint8_t>{0x9E});
    EXPECT_TRUE(RunTo(3.0).empty()) << "the break ends the repeat";
}

TEST_F(TypematicTest, OnlyTheMostRecentlyPressedKeyRepeats) {
    kbc.inject_scancode(0x1E);
    kbc.inject_scancode(0x1F);
    Drain();
    auto out = RunTo(0.55);
    EXPECT_EQ(out, std::vector<uint8_t>{0x1F});
    kbc.inject_scancode(0x9F);
    Drain();
    EXPECT_TRUE(RunTo(2.0).empty()) << "releasing it does not hand the repeat back to A";
}

TEST_F(TypematicTest, AGreyKeyRepeatsWithItsE0Prefix) {
    kbc.inject_scancode(0xE0);
    kbc.inject_scancode(0x48);
    Drain();
    auto out = RunTo(0.55);
    EXPECT_EQ(out, (std::vector<uint8_t>{0xE0, 0x48}));
    kbc.inject_scancode(0xE0);
    kbc.inject_scancode(0xC8);
    Drain();
    EXPECT_TRUE(RunTo(2.0).empty());
}

TEST_F(TypematicTest, SetTypematicRateChangesTheDelayAndTheRate) {
    kbc.out(0x60, 0xF3);
    kbc.out(0x60, 0x00);  // 250 ms, 30 cps
    EXPECT_EQ(Drain(), (std::vector<uint8_t>{0xFA, 0xFA})) << "command and argument each ACKed";
    EXPECT_EQ(kbc.typematic_byte(), 0x00);
    kbc.inject_scancode(0x1E);
    Drain();
    EXPECT_EQ(RunTo(0.26).size(), 1u);
    auto out = RunTo(1.26);
    EXPECT_GE(out.size(), 29u);
    EXPECT_LE(out.size(), 31u);

    kbc.out(0x60, 0xFF);  // reset restores the default
    Drain();
    EXPECT_EQ(kbc.typematic_byte(), 0x2B);
}

TEST_F(TypematicTest, PauseNeverRepeats) {
    for (uint8_t b : {0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5}) kbc.inject_scancode(b);
    Drain();
    EXPECT_TRUE(RunTo(2.0).empty());
}

TEST_F(TypematicTest, AReleaseWhileTheControllerHoldsTheKeyboardOffStillEndsTheRepeat) {
    kbc.inject_scancode(0x1E);
    Drain();
    kbc.out(0x64, 0xAD);  // disable keyboard
    kbc.inject_scancode(0x9E);
    kbc.out(0x64, 0xAE);
    EXPECT_TRUE(RunTo(2.0).empty());
}

TEST_F(TypematicTest, InterleavedGreyKeyBreaksStillEndTheRepeat) {
    // W and D on the WASD preset are Up and Right; their E0 sequences must not interleave
    for (uint8_t b : {0xE0, 0x48, 0xE0, 0x4D}) kbc.inject_scancode(b);
    Drain();
    for (uint8_t b : {0xE0, 0xE0, 0xC8, 0xCD}) kbc.inject_scancode(b);
    Drain();
    EXPECT_TRUE(RunTo(2.0).empty());
}

TEST_F(TypematicTest, ARepeatNeverLandsInsideAHostSequence) {
    kbc.inject_scancode(0xE0);
    kbc.inject_scancode(0x48);
    Drain();
    RunTo(0.49);
    kbc.inject_scancode(0xE0);           // the page's next sequence, mid-flight
    Drain();
    auto during = RunTo(0.60);
    EXPECT_TRUE(during.empty()) << "a real keyboard never splits one scan code's bytes";
    kbc.inject_scancode(0x4B);           // Left make completes it
    EXPECT_EQ(Drain(), std::vector<uint8_t>{0x4B});
}

}  // namespace
