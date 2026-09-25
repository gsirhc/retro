// GoogleTest suite for the AT keyboard controller: self-test, command-byte
// read/write, the A20 gate (Output Port bit 1), the CPU-reset trick (bit 0
// / command 0xFE), keyboard scan-code delivery with IRQ1 gating, and the
// PS/2 auxiliary (mouse) port -- AUX routing and status bit 5, IRQ12, the
// controller's AUX commands, and the mouse's own command set and movement
// packets.
//
// Mouse protocol expectations are cited against Adam Chapweske, "The PS/2
// Mouse Interface" (2001) -- byte values below are that document's own,
// including its worked "Emulated Action" table -- and against what this
// machine's shipped firmware does with them (Bochs BIOS `rombios.c`, INT
// 15h AH=C2h and the INT 74h handler). See PC486_REVIEW.md §10.

#include <gtest/gtest.h>

#include "i8042.h"

namespace {

using pc486::I8042;

// Send one byte to the mouse itself: controller command 0xD4, then the byte
// at the data port -- exactly what rombios.c's send_to_mouse_ctrl() does.
void SendAux(I8042& kbc, uint8_t byte) {
    kbc.out(0x64, 0xD4);
    kbc.out(0x60, byte);
}

// Read one byte from the mouse the way rombios.c's get_mouse_data() does:
// it spins until status reads OBF *and* AUXB -- `(inb(0x64) & 0x21) == 0x21`
// -- so a byte that fails this assertion is one the real BIOS never reads.
uint8_t ReadAux(I8042& kbc) {
    EXPECT_EQ(kbc.in(0x64) & 0x21, 0x21);
    return kbc.in(0x60);
}

// Bring the mouse up the way a driver does: release the AUX clock and
// enable both interrupts in the command byte, reset the device, drain its
// three-byte answer, then enable data reporting.
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

// One whole 3-byte movement packet, read back byte by byte.
struct Packet { uint8_t b0, b1, b2; };
Packet ReadPacket(I8042& kbc) {
    Packet p;
    p.b0 = ReadAux(kbc);
    p.b1 = ReadAux(kbc);
    p.b2 = ReadAux(kbc);
    return p;
}

TEST(I8042Test, ResetDeliversUnsolicitedKeyboardBatByte) {
    // A real AT keyboard sends 0xAA unsolicited after its own power-on
    // self-test, independent of the controller's 0xAA self-test command --
    // BIOS's keyboard-presence POST check waits for exactly this.
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
    kbc.out(0x60, 0x03);  // bit1 set -> A20 enabled; bit0 set -> reset line held high (inactive)
    EXPECT_TRUE(kbc.a20_enabled());
    EXPECT_FALSE(kbc.reset_requested());
}

// Port 0x92, the "Fast A20 Gate" / System Control Port A almost every
// 386+ chipset carries alongside the 8042 -- real, MS-DOS-era software
// (Microsoft's own HIMEM.SYS included) commonly tries this first, since
// toggling A20 through the keyboard controller's command protocol is much
// slower (OSDev Wiki, "A20 Line"). This is the same physical A20 line the
// 8042's own output port drives, not a second, independent latch.
TEST(I8042Test, FastA20PortEnablesA20) {
    I8042 kbc;
    kbc.reset();
    EXPECT_TRUE(kbc.owns_fast_a20(0x92));
    EXPECT_FALSE(kbc.a20_enabled());
    kbc.fast_a20_out(0x02);  // bit1 set -> A20 enabled; bit0 clear -> no reset
    EXPECT_TRUE(kbc.a20_enabled());
    EXPECT_FALSE(kbc.reset_requested());
    EXPECT_EQ(kbc.fast_a20_in(), 0x02);
}

TEST(I8042Test, FastA20PortAndOutputPortShareTheSameA20State) {
    I8042 kbc;
    kbc.reset();
    // Enabled via the slow (keyboard-controller) path...
    kbc.out(0x64, 0xD1);
    kbc.out(0x60, 0x02);
    EXPECT_TRUE(kbc.a20_enabled());
    EXPECT_EQ(kbc.fast_a20_in(), 0x02);  // ...reads back the same state via port 0x92
    // ...and disabled via the fast path is visible to the slow path's own read-back.
    kbc.fast_a20_out(0x00);
    EXPECT_FALSE(kbc.a20_enabled());
}

TEST(I8042Test, FastA20PortBitZeroTriggersReset) {
    I8042 kbc;
    kbc.reset();
    kbc.fast_a20_out(0x03);  // bit1 (A20) and bit0 (reset) both set
    EXPECT_TRUE(kbc.a20_enabled());
    EXPECT_TRUE(kbc.reset_requested());
}

TEST(I8042Test, OutputPortBitZeroLowTriggersReset) {
    I8042 kbc;
    kbc.reset();
    kbc.out(0x64, 0xD1);
    kbc.out(0x60, 0x00);  // bit0 clear -> reset line pulsed low
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
    kbc.out(0x60, 0x45);  // IRQ1 enabled, translation on (bit examples)
    kbc.out(0x64, 0x20);  // read command byte back
    EXPECT_EQ(kbc.in(0x60), 0x45);
}

TEST(I8042Test, ScancodeSetsIrq1OnlyWhenEnabledInCommandByte) {
    I8042 kbc;
    kbc.reset();
    kbc.in(0x60);  // drain the power-on BAT byte: a scan code queues behind it, it is not overwritten
    kbc.inject_scancode(0x1E);  // command byte's IRQ1-enable bit not yet set
    EXPECT_FALSE(kbc.irq1_pending());
    EXPECT_EQ(kbc.in(0x60), 0x1E);  // byte still delivered to the data port

    kbc.out(0x64, 0x60);
    kbc.out(0x60, 0x01);  // bit0 = enable IRQ1
    kbc.inject_scancode(0x1F);
    EXPECT_TRUE(kbc.irq1_pending());
    EXPECT_EQ(kbc.in(0x60), 0x1F);
    kbc.clear_irq1();
    EXPECT_FALSE(kbc.irq1_pending());
}

TEST(I8042Test, ResetCommandGetsAckThenBatByteOnSeparateReads) {
    // Real BIOS keyboard POST (confirmed against the Bochs rombios.c
    // source, see PC486_REVIEW.md) sends 0xFF, expects 0xFA back
    // immediately, THEN polls again and expects 0xAA as a second, distinct
    // byte -- not both at once, and not just the unsolicited power-on BAT.
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
    // "0xF2 (Read ID) - The keyboard responds by sending a two-byte device
    // ID of 0xAB, 0x83" (Chapweske, "The AT-PS/2 Keyboard Interface") --
    // on top of the ACK every keyboard command gets, per the same
    // document's command-set list ("Every byte sent to the keyboard gets a
    // response of 0xFA").
    I8042 kbc;
    kbc.reset();
    kbc.in(0x60);  // drain the power-on BAT byte
    kbc.out(0x60, 0xF2);  // Read ID
    EXPECT_EQ(kbc.in(0x60), 0xFA);
    EXPECT_EQ(kbc.in(0x60), 0xAB);
    EXPECT_EQ(kbc.in(0x60), 0x83);
    EXPECT_FALSE(kbc.in(0x64) & 0x01);  // nothing left queued
}

// Real, live bug: MS-DOS 6.22's SETUP.EXE sends 0xF2 during its own
// keyboard probe and relies on IRQ1 (not polling) to learn the response
// arrived. Before this fix every keyboard-command response -- the ACK
// included -- was pushed with irq=false, so Setup never saw an interrupt,
// retried 0xF2 three times over, and each retry's unread response piled up
// behind the single-byte output register, wedging it full forever and
// silently dropping every keystroke typed afterward.
TEST(I8042Test, KeyboardCommandAckRaisesIrq1WhenEnabled) {
    // "If no errors occur, the response byte is placed in the input
    // buffer, the IBF flag is set, and IRQ1 is activated, signaling the
    // keyboard driver" (Chapweske, "The AT-PS/2 Keyboard Interface",
    // "Writing to keyboard") -- true of every response the keyboard itself
    // sends back, an ACK included, with no special case for command
    // replies vs. scan codes.
    I8042 kbc;
    kbc.reset();
    kbc.in(0x60);  // drain the power-on BAT byte

    kbc.out(0x60, 0xF4);  // enable scanning -- IRQ1 not yet enabled in the command byte
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
    kbc.in(0x60);  // drain the keyboard's own unsolicited post-reset BAT byte first
    kbc.out(0x64, 0xAD);  // disable keyboard
    kbc.inject_scancode(0x1E);
    EXPECT_FALSE(kbc.in(0x64) & 0x01);  // nothing delivered
}

TEST(I8042Test, KeyboardBytesQueueInsteadOfOverwritingOneAnother) {
    // The 8042 holds a device's clock line low while its output buffer is
    // still full, and the device keeps its bytes until released -- so a
    // second scan code cannot destroy an unread first one.
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
    // Status bit 5 (AUXB) is how software tells a mouse byte from a key --
    // rombios.c's INT 74h handler returns immediately unless it reads 0x21.
    I8042 kbc;
    kbc.reset();
    kbc.in(0x60);
    kbc.inject_scancode(0x1E);
    EXPECT_EQ(kbc.in(0x64) & 0x21, 0x01);  // OBF set, AUXB clear
    kbc.in(0x60);

    kbc.out(0x64, 0xD3);  // write to the AUX side of the output buffer
    kbc.out(0x60, 0x5A);  // "...and act as if this was mouse data"
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
    // 0xA9 tests the link to the mouse; 0x00 means no error. It is the
    // *controller* answering, so the byte is not AUX-tagged.
    I8042 kbc;
    kbc.reset();
    kbc.in(0x60);
    kbc.out(0x64, 0xA9);
    EXPECT_EQ(kbc.in(0x64) & 0x21, 0x01);
    EXPECT_EQ(kbc.in(0x60), 0x00);
}

TEST(I8042Test, EnableDisableAuxTrackCommandByteBitFive) {
    // On a PS/2-superset controller 0xA7 sets command-byte bit 5 (AUX clock
    // driven low) and 0xA8 clears it, so the command byte and the two
    // commands are two views of one piece of state -- BIOS drives bit 5
    // directly (rombios.c inhibit_mouse_int_and_events), a driver may use
    // the commands, and they must agree.
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
    // rombios.c's set_kbd_command_byte() writes 0xD4 to port 0x64 and then
    // immediately 0x60 to port 0x64, abandoning that "write to mouse"
    // without ever supplying its data byte. The 8042 tells commands from
    // data by the A2 line, so the second command simply replaces the first
    // -- if the stale 0xD4 survived, the command byte would be delivered to
    // the mouse instead and the machine would lose its interrupts.
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
    // Command-byte bit 1 is the IRQ12 enable, independent of bit 0's IRQ1.
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
    // IRQ12 follows the output buffer, so a 3-byte packet is three
    // interrupts -- which is exactly how the BIOS's INT 74h handler
    // assembles one (it stores one byte per interrupt and only calls the
    // driver on the last).
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
    // "Following the BAT completion code (0xAA or 0xFC), the mouse sends its
    // device ID of 0x00." rombios.c's INT 15h AH=C2h AL=01h reads exactly
    // these three bytes with three separate get_mouse_data() calls, and
    // panics if the first is not 0xFA.
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
    // Reset leaves "Data Reporting Disabled": the mouse samples but sends
    // nothing until 0xF4.
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
    // Chapweske's own "Emulated Action" table, verbatim: the four unit
    // moves and their exact 3-byte packets. Note bit 3 of byte 1 is always
    // set (drivers that check it discard packets without it), and +Y is
    // *away* from the user, so "move down one" carries the Y sign bit.
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
    // Same table: press/release of each button with no movement.
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
    // A real mouse cannot start a second transmission on top of the first;
    // it keeps counting into its movement counters and sends the total once
    // the line frees up. Nothing is dropped, and no half-packet interleaves.
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
    // "The range of values that can be expressed by the movement counters
    // is -255 to +255. If this range is exceeded, the appropriate overflow
    // bit is set and the counter is not incremented/decremented until it is
    // reset." The excess is genuinely lost, not carried.
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
    // Command-byte bit 5 drives the AUX clock line low, which stops the
    // mouse reporting -- but the controller still raises the line to carry
    // a host command, and rombios.c depends on exactly that: every INT 15h
    // AH=C2h path calls inhibit_mouse_int_and_events() (which *sets* bit 5)
    // and then talks to the device and waits for its ACKs.
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
    // "In this mode, the mouse reads its inputs and updates its
    // counters/flags at the current sampling rate, but it only notifies the
    // host of movement ... when that information is requested" -- 0xEB.
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
    // The 2:1 table (0,1,1,3,6,9,2N) is applied to the counters before they
    // are reported -- but only for automatic stream-mode reporting.
    // Chapweske's footnote 1: "It does not effect the reported data sent in
    // response to the Read Data (0xEB) command."
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
    // Chapweske's emulation notes give the answer at defaults outright:
    // "Respond to the Status Request (0xE9) command with 0xFA, 0x00, 0x02,
    // 0x64" -- ACK, flags, resolution *code* 2 (4 counts/mm), 100 samples.
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
    // [0][mode=remote][enable][scaling 2:1][0][left][middle][right] -- the
    // button order here is the reverse of the movement packet's.
    EXPECT_EQ(ReadAux(kbc), 0x40 | 0x20 | 0x10 | 0x04 | 0x01);
    EXPECT_EQ(ReadAux(kbc), 0x03);
    EXPECT_EQ(ReadAux(kbc), 200);
}

TEST(I8042Test, SetDefaultsRestoresTheResetState) {
    // 0xF6 loads the same values the BAT does: 100 samples/sec, 4 counts/mm,
    // 1:1 scaling, reporting disabled, stream mode.
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
    // A driver probing for a scrolling wheel sends "set sample rate 200,
    // 100, 80" and then reads the device ID. This machine's mouse is a
    // period 3-button PS/2 mouse, not a 1996 IntelliMouse, so it answers
    // that probe the way a standard mouse does -- every rate accepted, ID
    // still 0x00 -- and the driver correctly concludes there is no wheel.
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
    // "every byte received by the mouse is sent back to the host. Even if
    // the byte represents a valid command... There are two exceptions to
    // this: the Reset (0xFF) command and Reset Wrap Mode (0xEC) command."
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
    // 0xFE is what a host sends when it decides a packet was garbled. It is
    // also the one command that does *not* reset the movement counters.
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

    // In remote mode nothing is sent unasked, so the counters can be
    // watched across a resend: the counts banked before it survive it.
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
    // A device that does not recognise a command answers 0xFE, which is how
    // a driver probing for an extension learns it is absent.
    I8042 kbc;
    InitMouse(kbc);
    SendAux(kbc, 0x9C);
    EXPECT_EQ(ReadAux(kbc), 0xFE);
}

TEST(I8042Test, MouseCommandsClearTheMovementCounters) {
    // "the movement counters are reset ... after the mouse receives any
    // command from the host other than the Resend (0xFE) command."
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
    // A player moving the mouse while typing produces both streams at once.
    // Every byte must survive, and each must keep its own AUXB tag -- that
    // tag is the only thing letting INT 09h and INT 74h sort them out.
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
    // Real, reported bug: a movement key's break code landing mid-burst of
    // mouse-look packets got silently dropped once the queue (previously
    // 16 entries) filled up, leaving the key stuck "held" forever from the
    // guest's side. Floods the queue with far more than the old capacity
    // of mouse bytes first, then confirms a keyboard byte queued after
    // still comes through rather than vanishing -- see i8042.h's
    // kQueueSize comment.
    I8042 kbc;
    InitMouse(kbc);
    for (int i = 0; i < 200; ++i) kbc.inject_mouse_event(1, 0, 0);  // 600 bytes queued
    kbc.inject_scancode(0xD1);  // W's break code, arriving after the flood

    bool found_break_code = false;
    for (int i = 0; i < 700; ++i) {
        uint8_t status = kbc.in(0x64);
        if ((status & 0x01) == 0) break;  // OBF clear -- queue drained
        bool aux = (status & 0x20) != 0;
        uint8_t byte = kbc.in(0x60);
        if (!aux && byte == 0xD1) found_break_code = true;
    }
    EXPECT_TRUE(found_break_code);
}

TEST(I8042Test, BiosPointingDeviceInitSequenceRunsEndToEnd) {
    // The exact shape of rombios.c's INT 15h AH=C2h AL=01h (reset) and
    // AL=00h BH=01h (enable): read the command byte, clear IRQ12 and drive
    // the AUX clock low, talk to the device, then restore both. A mouse
    // that answers this sequence is one the BIOS will report as present.
    I8042 kbc;
    kbc.reset();
    kbc.in(0x60);

    kbc.out(0x64, 0x20);
    uint8_t comm = kbc.in(0x60);
    comm &= ~0x02;  // turn off IRQ12 generation
    comm |= 0x20;   // disable the mouse serial clock line
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
    comm |= 0x02;   // turn on IRQ12 generation
    comm &= ~0x20;  // enable the mouse serial clock line
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

}  // namespace
