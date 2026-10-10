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
    m.run_cycles(1000);
    EXPECT_EQ(m.cpu.pc, 0x9000);      // released, real reset vector read
}

TEST(Machine, NegativeBudgetIsANoOpAndTheCpuKeepsRunning) {
    Machine m;
    uint8_t img[32768] = {};
    img[32768 - 4] = 0x00; img[32768 - 3] = 0x90;
    m.bus.rom.load_image(img, 32768);
    m.power_on_reset();
    m.run_cycles(-5);
    EXPECT_EQ(m.cycles(), 0u);
    m.run_cycles(kResetHoldCycles);
    EXPECT_GE(m.cycles(), uint64_t(kResetHoldCycles));
    EXPECT_EQ(m.cpu.pc, 0x9000);
}

TEST(Machine, LcdAttachedByDefaultSoResetViaIrqDoesNotHang) {
    Machine m;
    EXPECT_TRUE(m.lcd_attached());
    // reset_via_irq's busy-poll must read not busy with the LCD attached
    m.bus.via.write(0x2, 0x00);       // DDRB: all input, as lcd_wait sets it
    EXPECT_EQ(m.bus.via.read(0x0) & 0x80, 0);
}

TEST(Machine, DetachingTheLcdReproducesTheGenuineBareBoardHang) {
    Machine m;
    m.set_lcd_attached(false);
    m.bus.via.write(0x2, 0x00);
    EXPECT_NE(m.bus.via.read(0x0) & 0x80, 0);   // floating input reads high
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
    // ctest cwd is tests/build/; four levels up is cpu6502/rom/
    std::ifstream f("../../../../cpu6502/rom/tmp/firmware.bin", std::ios::binary);
    if (!f) GTEST_SKIP() << "firmware.bin not built -- run `make -C .. rom` first";
    std::vector<uint8_t> img((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    ASSERT_EQ(img.size(), 32768u);

    Machine m;
    m.bus.rom.load_image(img.data(), int(img.size()));
    std::string out;
    m.on_serial_out = [&](uint8_t c) { out += char(c); };

    m.run_cycles(200000);   // DS1813 hold, reset init, banner

    // Wozmon's ESCAPE entry prints "\" then CR/LF
    EXPECT_NE(out.find("\\\r\n"), std::string::npos);

    // "0.F" dumps 16 bytes of zero page
    out.clear();
    // one-byte ACIA RX register: give the NMI handler cycles to drain between chars
    for (char c : std::string("0.F\r")) { m.type_char(uint8_t(c)); m.run_cycles(1000); }
    m.run_cycles(200000);
    EXPECT_NE(out.find("0000:"), std::string::npos);
}

} // namespace
