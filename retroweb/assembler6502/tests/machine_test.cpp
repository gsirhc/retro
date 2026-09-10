// GoogleTest suite for machine::Machine -- reset timing, the LCD
// accessory's default-attached behavior, and (when the real ROM has been
// built -- `make -C .. rom`, gitignored like every other *.bin in this
// repo) a full end-to-end boot of the actual, unmodified firmware.

#include <gtest/gtest.h>

#include "../machine.h"

#include <fstream>
#include <string>
#include <vector>

using namespace machine;

namespace {

TEST(Machine, ResetHoldsPcAtZeroUntilTheDs1813DelayElapses) {
    Machine m;
    uint8_t img[32768] = {};
    img[32768 - 4] = 0x00; img[32768 - 3] = 0x90;   // reset vector -> $9000
    m.bus.rom.load_image(img, 32768);
    m.power_on_reset();
    EXPECT_EQ(m.cpu.pc, 0);
    m.run_cycles(kResetHoldCycles - 1000);
    EXPECT_EQ(m.cpu.pc, 0);          // still held
    m.run_cycles(1000);               // exactly the remainder of the hold, no further execution yet
    EXPECT_EQ(m.cpu.pc, 0x9000);      // released, real reset vector read
}

TEST(Machine, LcdAttachedByDefaultSoResetViaIrqDoesNotHang) {
    Machine m;
    EXPECT_TRUE(m.lcd_attached());
    // reset_via_irq's busy-poll (PB configured as input, bit7 checked)
    // must read "not busy" with the accessory attached.
    m.bus.via.write(0x2, 0x00);       // DDRB: all input, as lcd_wait sets it
    EXPECT_EQ(m.bus.via.read(0x0) & 0x80, 0);
}

TEST(Machine, DetachingTheLcdReproducesTheGenuineBareBoardHang) {
    Machine m;
    m.set_lcd_attached(false);
    m.bus.via.write(0x2, 0x00);
    EXPECT_NE(m.bus.via.read(0x0) & 0x80, 0);   // floating input reads high -- busy-wait never clears
}

TEST(Machine, TypedCharacterReachesTheAciaAsAReceivedByte) {
    Machine m;
    m.bus.acia.write(3, 0x0F);
    m.type_char('A');
    EXPECT_TRUE(m.bus.acia.read(1) & 0x08);   // RDRF
    EXPECT_EQ(m.bus.acia.read(0), 'A');
}

// --- real-firmware end-to-end boot, skipped if the ROM hasn't been built ---

TEST(Machine, BootsTheRealFirmwareStraightToWozmon) {
    // ctest's cwd for gtest_discover_tests is tests/build/ -- four levels
    // up reaches the repo's cpu6502/rom/ tree.
    std::ifstream f("../../../../cpu6502/rom/tmp/firmware.bin", std::ios::binary);
    if (!f) GTEST_SKIP() << "firmware.bin not built -- run `make -C .. rom` first";
    std::vector<uint8_t> img((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    ASSERT_EQ(img.size(), 32768u);

    Machine m;
    m.bus.rom.load_image(img.data(), int(img.size()));
    std::string out;
    m.on_serial_out = [&](uint8_t c) { out += char(c); };

    m.run_cycles(200000);   // clears the DS1813 hold + reset init + CLEAR_TERMINAL + the "\" banner

    // Real Wozmon's cold/re-sync entry (ESCAPE) prints "\" then CR/LF --
    // this is the actual, unmodified banner, not a custom boot message.
    EXPECT_NE(out.find("\\\r\n"), std::string::npos);

    // The prompt should accept a real examine command: "0.F" dumps 16
    // bytes of zero page starting at $00 -- a real end-to-end interaction,
    // not just a banner check.
    out.clear();
    // The ACIA has only a one-byte RX register -- pushing a second char
    // before the NMI handler has drained the first (into SERIAL_BUFFER)
    // overwrites it, a real overrun, so each type_char() needs cycles to
    // run in between (same reason the real browser pump interleaves them).
    for (char c : std::string("0.F\r")) { m.type_char(uint8_t(c)); m.run_cycles(1000); }
    m.run_cycles(200000);
    EXPECT_NE(out.find("0000:"), std::string::npos);
}

} // namespace
