#include <gtest/gtest.h>

#include "machine.h"
#include "hwtest_roms.h"

#include <array>

namespace {

pacman::RomSet test_set() {
    pacman::RomSet s;
    std::copy(pacman::hwtest::program.begin(), pacman::hwtest::program.end(), s.program.begin());
    std::copy(pacman::hwtest::tiles.begin(), pacman::hwtest::tiles.end(), s.tiles.begin());
    std::copy(pacman::hwtest::sprites.begin(), pacman::hwtest::sprites.end(), s.sprites.begin());
    std::copy(pacman::hwtest::color_prom.begin(), pacman::hwtest::color_prom.end(), s.color_prom.begin());
    std::copy(pacman::hwtest::lookup_prom.begin(), pacman::hwtest::lookup_prom.end(), s.lookup_prom.begin());
    std::copy(pacman::hwtest::wave_prom.begin(), pacman::hwtest::wave_prom.end(), s.wave_prom.begin());
    return s;
}

TEST(Machine, HwtestWritesSignatureAndKicksWatchdog) {
    pacman::Machine m;
    m.load_roms(test_set());
    m.reset();
    m.run_cycles(pacman::kCpuHz / 10);  // 100 ms
    EXPECT_EQ(m.ram[0x4C00 - 0x4800], 'T');
    EXPECT_EQ(m.ram[0x4C01 - 0x4800], 'S');
    EXPECT_EQ(m.ram[0x4C02 - 0x4800], 'T');
    EXPECT_EQ(m.ram[0x4C03 - 0x4800], '1');
    EXPECT_FALSE(m.watchdog_reset);
    EXPECT_GT(m.frames, 0);
}

// The self-test ROM programs voice 0's real hardware registers ($5050/
// $5051 frequency, $5055 volume, $5045 waveform, $5001 sound enable -- see
// gen_hwtest.py) and expects it to be audible. This is an end-to-end check
// that the ROM's register offsets and Wsg's decode of them actually agree
// -- catches, e.g., the ROM and the decoder both silently assuming a
// different (wrong) map, which no single-sided unit test would surface.
TEST(Machine, HwtestVoiceIsAudible) {
    pacman::Machine m;
    m.load_roms(test_set());
    m.reset();
    m.run_cycles(pacman::kCpuHz / 10);  // let the ROM finish programming the WSG
    m.audio.clear();
    m.run_cycles(pacman::kCpuHz / 20);  // 50 ms of audio
    ASSERT_FALSE(m.audio.empty());
    bool any_nonzero = false;
    for (float s : m.audio) {
        if (s != 0.0f) { any_nonzero = true; break; }
    }
    EXPECT_TRUE(any_nonzero) << "self-test ROM's voice 0 should be audible, not silent";
}

TEST(Machine, JoystickEchoesToRam) {
    pacman::Machine m;
    m.load_roms(test_set());
    m.reset();
    m.inputs.in0 = 0xFE;  // up pressed (active low)
    m.run_cycles(pacman::kCpuHz / 20);
    EXPECT_EQ(m.ram[0x4C10 - 0x4800], 0xFE);
}

// Real Pac-Man latches its Z80 IM 2 interrupt-vector byte via `OUT (0),A` --
// a discrete hardware latch, not a fixed value -- and the ROM reprograms it
// at runtime (0xFA during the self-test's per-vblank checksum passes, 0xFC
// once the main game's vblank ISR takes over; see $233F/$3183 in the Midway
// pacman disassembly). A machine that ignores the OUT and hands the CPU a
// hardcoded vector byte sends every interrupt to whatever ROM bytes happen
// to sit at that one fixed address, which is exactly the bug this guards:
// on the real ROM it derailed the CPU into unmapped memory within a few
// frames of loading a genuine ROM set.
TEST(Machine, OutPort0LatchesInterruptVector) {
    pacman::RomSet s;
    // I=0x00, IM 2, OUT (0),A with A=0x40 -> vector $0040 -> handler at $0100.
    const uint8_t code[] = {
        0x3E, 0x00,        // LD A,$00
        0xED, 0x47,        // LD I,A
        0xED, 0x5E,        // IM 2
        0x3E, 0x40,        // LD A,$40
        0xD3, 0x00,        // OUT ($00),A
        0x3E, 0x01,        // LD A,1
        0x32, 0x00, 0x50,  // LD ($5000),A -- machine-level irq_enable
        0xFB,              // EI
        0x76,              // HALT
    };
    std::copy(std::begin(code), std::end(code), s.program.begin());
    s.program[0x0040] = 0x00;  // vector table entry $0040 -> $0100
    s.program[0x0041] = 0x01;
    s.program[0x0100] = 0x3E;  // LD A,$42
    s.program[0x0101] = 0x42;
    s.program[0x0102] = 0x32;  // LD ($4800),A
    s.program[0x0103] = 0x00;
    s.program[0x0104] = 0x48;
    s.program[0x0105] = 0x76;  // HALT

    pacman::Machine m;
    m.load_roms(s);
    m.reset();
    m.run_cycles(pacman::kCpuPerFrame * 2);

    EXPECT_EQ(m.ram[0x4800 - 0x4800], 0x42) << "interrupt should vector through the OUT-latched byte, not a fixed address";
}

TEST(Machine, RendersNonBlackFrame) {
    pacman::Machine m;
    m.load_roms(test_set());
    m.reset();
    m.run_cycles(pacman::kCpuPerFrame * 3);
    std::array<uint32_t, pacman::kUprightW * pacman::kUprightH> fb{};
    m.render(fb.data());
    int lit = 0;
    for (uint32_t p : fb)
        if (p != 0) lit++;
    EXPECT_GT(lit, 1000);
}

int vram_off(int col, int row) {
    int c = col - 2;
    int r = row + 2;
    if (c & 0x20) return r + ((c & 0x1F) << 5);
    return c + (r << 5);
}

// After the two ~1.5 s test patterns (crosshatch, color bars) the ROM
// holds a help screen whose first line is "PAC-MAN ARCADE" in the
// generated 8×8 font — see gen_hwtest.py HELP_TITLE / HELP_TITLE_ROW.
// Upright (col, row) → native (row, 27-col), same ROT90 as Video::render.
TEST(Machine, HwtestHelpScreenShowsCopyrightPrompt) {
    pacman::Machine m;
    m.load_roms(test_set());
    m.reset();
    m.run_cycles(pacman::kCpuPerFrame * 200);  // past both 90-frame waits

    const char* title = "PAC-MAN ARCADE";
    const int urow = 4;
    const int ucol0 = (28 - 14) / 2;
    for (int i = 0; title[i]; i++) {
        int ncol = urow;
        int nrow = 27 - (ucol0 + i);
        int offs = vram_off(ncol, nrow) & 0x3FF;
        EXPECT_EQ(m.video.videoram[unsigned(offs)], static_cast<uint8_t>(title[i]))
            << "help-screen title char " << i;
    }
    // A later line carries the copyright message.
    const char* line = "UNDER COPYRIGHT";
    const int crow = 20;
    const int ccol0 = (28 - 15) / 2;
    for (int i = 0; line[i]; i++) {
        int ncol = crow;
        int nrow = 27 - (ccol0 + i);
        int offs = vram_off(ncol, nrow) & 0x3FF;
        EXPECT_EQ(m.video.videoram[unsigned(offs)], static_cast<uint8_t>(line[i]))
            << "copyright line char " << i;
    }
}

}  // namespace
