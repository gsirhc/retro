#include <gtest/gtest.h>

#include "machine.h"
#include "hwtest_roms.h"

#include <algorithm>
#include <iterator>
#include <vector>

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

// The self-test leaves the WSG disabled. A low voice-0 frequency reads as
// a tick, so the help screens stay quiet.
TEST(Machine, HwtestStaysSilent) {
    pacman::Machine m;
    m.load_roms(test_set());
    m.reset();
    m.audio.clear();
    m.run_cycles(pacman::kCpuHz / 2);
    ASSERT_FALSE(m.audio.empty());
    bool any = false;
    for (float s : m.audio) if (s != 0.0f) { any = true; break; }
    EXPECT_FALSE(any);
}

TEST(Machine, JoystickEchoesToRam) {
    pacman::Machine m;
    m.load_roms(test_set());
    m.reset();
    m.inputs.in0 = 0xFE;  // up pressed (active low)
    m.run_cycles(pacman::kCpuHz / 20);
    EXPECT_EQ(m.ram[0x4C10 - 0x4C00], 0xFE);
}

// The ROM reprograms the IM 2 vector latch (OUT (0),A) at runtime, so a
// hardcoded vector byte derails the CPU on a real ROM set.
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
    s.program[0x0102] = 0x32;  // LD ($4C00),A
    s.program[0x0103] = 0x00;
    s.program[0x0104] = 0x4C;
    s.program[0x0105] = 0x76;  // HALT

    pacman::Machine m;
    m.load_roms(s);
    m.reset();
    m.run_cycles(pacman::kCpuPerFrame * 2);

    EXPECT_EQ(m.ram[0], 0x42) << "interrupt should vector through the OUT-latched byte, not a fixed address";
}

int vram_off(int col, int row) {
    int c = col - 2;
    int r = row + 2;
    if (c & 0x20) return r + ((c & 0x1F) << 5);
    return c + (r << 5);
}

// After the two test patterns the ROM shows a help screen titled
// "PAC-MAN ARCADE" (gen_hwtest.py HELP_TITLE). Upright (col, row) is native (row, 27-col).
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

TEST(Machine, MsPacmanHwtestHelpScreenShowsMsPacManTitle) {
    pacman::RomSet s = test_set();
    std::copy(pacman::hwtest::mspacman_program.begin(),
              pacman::hwtest::mspacman_program.end(), s.program.begin());
    pacman::Machine m;
    m.load_roms(s);
    m.reset();
    m.run_cycles(pacman::kCpuPerFrame * 200);

    const char* title = "MS PAC-MAN ARCADE";
    const int urow = 4;
    const int ucol0 = (28 - 17) / 2;
    for (int i = 0; title[i]; i++) {
        int ncol = urow;
        int nrow = 27 - (ucol0 + i);
        int offs = vram_off(ncol, nrow) & 0x3FF;
        EXPECT_EQ(m.video.videoram[unsigned(offs)], static_cast<uint8_t>(title[i]))
            << "Ms. Pac-Man help-screen title char " << i;
    }
    const char* line = "MS PAC-MAN ROMS";
    const int crow = 15;
    const int ccol0 = (28 - 15) / 2;
    for (int i = 0; line[i]; i++) {
        int ncol = crow;
        int nrow = 27 - (ccol0 + i);
        int offs = vram_off(ncol, nrow) & 0x3FF;
        EXPECT_EQ(m.video.videoram[unsigned(offs)], static_cast<uint8_t>(line[i]))
            << "Ms. Pac-Man ROM line char " << i;
    }
}

TEST(Machine, Dsw1FactoryDefaultIsThreeLivesBonus10k) {
    pacman::Machine m;
    EXPECT_EQ(m.inputs.dsw1, 0xC9);
    EXPECT_EQ(m.mem_read(0x5080), 0xC9);
    EXPECT_EQ(m.mem_read(0x50C0), 0xFF);
}

TEST(Machine, Dsw1PortFollowsInputsIncludingMirrors) {
    pacman::Machine m;
    m.inputs.dsw1 = 0xCD;  // 5 lives, otherwise factory
    EXPECT_EQ(m.mem_read(0x5080), 0xCD);
    EXPECT_EQ(m.mem_read(0x50BF), 0xCD);  // $5080 mirrored through $FFC0
    m.inputs.dsw2 = 0x00;
    EXPECT_EQ(m.mem_read(0x50C0), 0x00);
}

TEST(Machine, NoRamAt4800TheFloatingBusReadsBF) {
    pacman::Machine m;
    m.mem_write(0x4800, 0x12);
    m.mem_write(0x4BFF, 0x34);
    EXPECT_EQ(m.mem_read(0x4800), 0xBF);
    EXPECT_EQ(m.mem_read(0x4BFF), 0xBF);
    m.mem_write(0x4C00, 0x56);
    EXPECT_EQ(m.mem_read(0x4C00), 0x56);
    EXPECT_EQ(m.ram[0], 0x56);
}

TEST(Machine, A13AndA15MirrorTheRamBlocks) {
    pacman::Machine m;
    m.mem_write(0x6000, 0x11);
    EXPECT_EQ(m.video.videoram[0], 0x11);
    m.mem_write(0xE400, 0x22);
    EXPECT_EQ(m.video.colorram[0], 0x22);
    m.mem_write(0x6C05, 0x33);
    EXPECT_EQ(m.mem_read(0x4C05), 0x33);
    EXPECT_EQ(m.mem_read(0xEC05), 0x33);
    m.mem_write(0x6FF2, 0x44);
    EXPECT_EQ(m.video.spriteram[2], 0x44);
    EXPECT_EQ(m.mem_read(0x6800), 0xBF);
}

TEST(Machine, IoPageIgnoresA8ToA11) {
    pacman::Machine m;
    m.inputs.in0 = 0x10;
    m.inputs.in1 = 0x20;
    m.inputs.dsw2 = 0x30;
    EXPECT_EQ(m.mem_read(0x5100), 0x10);
    EXPECT_EQ(m.mem_read(0x5F7F), 0x20);
    EXPECT_EQ(m.mem_read(0x7040), 0x20);
    EXPECT_EQ(m.mem_read(0x5AFF), 0x30);
    m.mem_write(0x5009, 1);
    EXPECT_TRUE(m.wsg.enabled) << "the main latch repeats every 8 bytes";
    m.mem_write(0x513B, 1);
    EXPECT_TRUE(m.video.flip_screen);
    m.mem_write(0x5545, 0x07);
    EXPECT_EQ(m.wsg.regs[5], 0x07);
    m.mem_write(0x5F6A, 0x99);
    EXPECT_EQ(m.video.sprite_xy[0xA], 0x99);
}

TEST(Machine, LatchBitsWithNoHardwareDoNothing) {
    pacman::Machine m;
    for (uint16_t a : {0x5002, 0x5004, 0x5005, 0x5006}) m.mem_write(a, 1);
    EXPECT_FALSE(m.irq_enable);
    EXPECT_FALSE(m.wsg.enabled);
    EXPECT_FALSE(m.video.flip_screen);
    EXPECT_EQ(m.coin_counter[0], 0);
}

TEST(Machine, SoundChipSitsAt5040AndIsEnabledBy5001) {
    pacman::Machine m;
    m.mem_write(0x5001, 1);
    EXPECT_TRUE(m.wsg.enabled);
    m.mem_write(0x5001, 0);
    EXPECT_FALSE(m.wsg.enabled);
    m.mem_write(0x5055, 0x0F);
    m.mem_write(0x5045, 0x03);
    EXPECT_EQ(m.wsg.regs[0x15], 0x0F);
    EXPECT_EQ(m.wsg.regs[0x05], 0x03);
}

TEST(Machine, WatchdogKicksAnywhereIn50C0To50FF) {
    pacman::Machine m;
    m.program[0] = 0x18;
    m.program[1] = 0xFE;
    m.reset();
    for (int f = 0; f < 40; f++) {
        m.run_cycles(pacman::kCpuPerFrame);
        m.mem_write(f & 1 ? 0x50FF : 0x5FC5, 0);
    }
    EXPECT_FALSE(m.watchdog_reset);
}

TEST(Machine, WatchdogExpiresAfterSixteenVblanksWithoutKick) {
    pacman::Machine m;
    m.program[0] = 0x18;
    m.program[1] = 0xFE;  // JR $
    m.reset();
    m.watchdog_reset = false;
    m.run_cycles(pacman::kCpuPerFrame * 15);
    EXPECT_FALSE(m.watchdog_reset);
    m.run_cycles(pacman::kCpuPerFrame);
    EXPECT_TRUE(m.watchdog_reset);
}

namespace {

// IM 1 ISR counting at $4C00; `rearm` writes 0 then 1 to $5000 before RETI.
void vblank_counter(pacman::Machine& m, bool rearm) {
    const uint8_t main[] = {0x31, 0xF0, 0x4F, 0xED, 0x56, 0x3E, 0x01, 0x32, 0x00, 0x50, 0xFB,
                            0x32, 0xC0, 0x50, 0x18, 0xFB};
    std::copy(std::begin(main), std::end(main), m.program.begin());
    std::vector<uint8_t> isr = {0xF5, 0x3A, 0x00, 0x4C, 0x3C, 0x32, 0x00, 0x4C};
    if (rearm) isr.insert(isr.end(), {0xAF, 0x32, 0x00, 0x50, 0x3C, 0x32, 0x00, 0x50});
    isr.insert(isr.end(), {0xF1, 0xFB, 0xED, 0x4D});
    std::copy(isr.begin(), isr.end(), m.program.begin() + 0x38);
}

}  // namespace

TEST(Machine, VblankIrqIsHeldUntil5000IsCleared) {
    pacman::Machine m;
    m.reset();
    vblank_counter(m, false);
    m.run_cycles(pacman::kCpuPerFrame * 2);
    EXPECT_TRUE(m.cpu.int_line);
    EXPECT_GT(m.ram[0], 100) << "a held /INT re-enters the ISR after every RETI";
}

TEST(Machine, VblankIrqFiresOncePerFrameWhenAcknowledged) {
    pacman::Machine m;
    m.reset();
    vblank_counter(m, true);
    m.run_cycles(pacman::kCpuPerFrame * 5);
    EXPECT_EQ(m.ram[0], 5);
}

TEST(Machine, VblankIrqWaitsOutADisabledCpu) {
    pacman::Machine m;
    m.reset();
    vblank_counter(m, true);
    m.program[10] = 0x00;  // no EI
    m.run_cycles(pacman::kCpuPerFrame * 2);
    EXPECT_EQ(m.ram[0], 0);
    EXPECT_TRUE(m.cpu.int_line);
    m.cpu.iff1 = m.cpu.iff2 = true;
    m.run_cycles(100);
    EXPECT_EQ(m.ram[0], 1);
}

TEST(Machine, WatchdogResetKeepsRam) {
    pacman::Machine m;
    m.program[0] = 0x18;
    m.program[1] = 0xFE;
    m.reset();
    m.ram[0x10] = 0xA5;
    m.video.videoram[5] = 0x5A;
    m.run_cycles(pacman::kCpuPerFrame * pacman::kWatchdogFrames);
    ASSERT_TRUE(m.watchdog_reset);
    EXPECT_EQ(m.ram[0x10], 0xA5);
    EXPECT_EQ(m.video.videoram[5], 0x5A);
    EXPECT_FALSE(m.irq_enable);
}

TEST(Machine, WatchdogKickAt50C0PreventsExpiry) {
    pacman::Machine m;
    m.program[0] = 0x18;
    m.program[1] = 0xFE;
    m.reset();
    m.watchdog_reset = false;
    for (int i = 0; i < pacman::kWatchdogFrames * 2; i++) {
        m.mem_write(0x50C0, 0);
        m.run_cycles(pacman::kCpuPerFrame);
        EXPECT_FALSE(m.watchdog_reset) << "frame " << i;
    }
}

TEST(Machine, Write5003SetsFlipScreen) {
    pacman::Machine m;
    EXPECT_FALSE(m.video.flip_screen);
    m.mem_write(0x5003, 1);
    EXPECT_TRUE(m.video.flip_screen);
    m.mem_write(0x5003, 0);
    EXPECT_FALSE(m.video.flip_screen);
}

// A15 is unconnected on the stock PCB, so $8000-$BFFF mirror $0000-$3FFF.
TEST(Machine, PacManA15MirrorsProgramRom) {
    pacman::RomSet s;
    s.program[0] = 0x3C;
    s.program[0x1234] = 0xA5;
    pacman::Machine m;
    m.load_roms(s);
    EXPECT_EQ(m.mem_read(0x0000), 0x3C);
    EXPECT_EQ(m.mem_read(0x8000), 0x3C);
    EXPECT_EQ(m.mem_read(0x1234), 0xA5);
    EXPECT_EQ(m.mem_read(0x9234), 0xA5);
}

// U5/U6/U7 data-line scramble (patent 4,525,599): input bit 0 becomes bit 7.
uint16_t bitswap_t(uint16_t val, const int* bits, int n) {
    uint16_t out = 0;
    for (int i = 0; i < n; i++)
        out |= uint16_t((val >> bits[i]) & 1) << (n - 1 - i);
    return out;
}
uint8_t aux_data_t(uint8_t v) {
    const int b[] = {0, 4, 5, 7, 6, 3, 2, 1};
    return uint8_t(bitswap_t(v, b, 8));
}
uint8_t aux_data_inv_t(uint8_t v) {
    const int b[] = {4, 3, 5, 6, 2, 1, 0, 7};
    return uint8_t(bitswap_t(v, b, 8));
}

TEST(Machine, AuxDataScrambleRoundTripAndGoldenVector) {
    EXPECT_EQ(aux_data_t(0x01), 0x80);
    for (int i = 0; i < 256; i++)
        EXPECT_EQ(int(aux_data_inv_t(aux_data_t(uint8_t(i)))), i);
}

void plant_u5(pacman::RomSet& s, uint16_t cpu, uint8_t decoded) {
    const int a[] = {8, 7, 5, 9, 10, 6, 3, 4, 2, 1, 0};
    s.aux_u5[bitswap_t(uint16_t(cpu - 0x8000), a, 11)] = aux_data_inv_t(decoded);
}
void plant_u6(pacman::RomSet& s, uint16_t cpu, uint8_t decoded) {
    const int a[] = {3, 7, 9, 10, 8, 6, 5, 4, 2, 1, 0};
    if (cpu >= 0x8800 && cpu < 0x9000) {
        uint16_t i = uint16_t(cpu - 0x8800);
        s.aux_u6[0x800 + bitswap_t(i, a, 11)] = aux_data_inv_t(decoded);
    } else {
        uint16_t i = uint16_t(cpu - 0x9000);
        s.aux_u6[bitswap_t(i, a, 11)] = aux_data_inv_t(decoded);
    }
}
void plant_u7(pacman::RomSet& s, uint16_t cpu, uint8_t decoded) {
    const int a[] = {11, 3, 7, 9, 10, 8, 6, 5, 4, 2, 1, 0};
    s.aux_u7[bitswap_t(uint16_t(cpu - 0x3000), a, 12)] = aux_data_inv_t(decoded);
}

pacman::RomSet aux_set() {
    pacman::RomSet s;
    s.program.fill(0xE5);  // distinctive original Pac-Man fill
    s.program[0x0410] = 0xAA;
    s.program[0x3000] = 0xBB;
    s.aux_board = true;
    plant_u5(s, 0x8008, 0x11);
    plant_u5(s, 0x8010, 0x22);
    plant_u6(s, 0x9000, 0x33);
    plant_u6(s, 0x8800, 0x44);
    plant_u7(s, 0x3000, 0x55);
    plant_u7(s, 0x3FFC, 0x00);
    plant_u7(s, 0x3FFD, 0x01);
    return s;
}

TEST(Machine, AuxDecodeStartsDisabledUntil3FF8) {
    pacman::Machine m;
    m.load_roms(aux_set());
    EXPECT_TRUE(m.aux_board);
    EXPECT_FALSE(m.aux_decode);
    EXPECT_EQ(m.mem_read(0x0410), 0xAA) << "patch window should still be Pac-Man";
    EXPECT_EQ(m.mem_read(0x3000), 0xBB) << "$3000 is still 6j";
    (void)m.mem_read(0x3FF8);  // latch set
    EXPECT_TRUE(m.aux_decode);
    EXPECT_EQ(m.mem_read(0x3000), 0x55) << "U7 overlay";
    EXPECT_EQ(m.mem_read(0x8008), 0x11);
    EXPECT_EQ(m.mem_read(0x8010), 0x22);
    EXPECT_EQ(m.mem_read(0x9000), 0x33);
    EXPECT_EQ(m.mem_read(0x8800), 0x44);
    EXPECT_EQ(m.mem_read(0x0410), 0x11) << "$0410 is the $8008 patch window";
    EXPECT_EQ(m.mem_read(0x1008), 0x22) << "$1008 is the $8010 patch window";
}

TEST(Machine, AuxDisableTrapAt0038RestoresPacMan) {
    pacman::Machine m;
    m.load_roms(aux_set());
    (void)m.mem_read(0x3FF8);
    EXPECT_TRUE(m.aux_decode);
    EXPECT_EQ(m.mem_read(0x0410), 0x11);
    (void)m.mem_read(0x0038);
    EXPECT_FALSE(m.aux_decode);
    EXPECT_EQ(m.mem_read(0x0410), 0xAA);
    EXPECT_EQ(m.mem_read(0x3000), 0xBB);
}

TEST(Machine, AuxDisableTrapAt8000ReturnsPacManMirror) {
    pacman::RomSet s = aux_set();
    s.program[0] = 0xC3;
    pacman::Machine m;
    m.load_roms(s);
    (void)m.mem_read(0x3FF8);
    EXPECT_TRUE(m.aux_decode);
    // $8000-$8007 is a latch-clear trap, so the read returns the original-bank mirror.
    EXPECT_EQ(m.mem_read(0x8000), 0xC3);
    EXPECT_FALSE(m.aux_decode);
}

TEST(Machine, AuxWriteToEnableTrapSetsLatch) {
    pacman::Machine m;
    m.load_roms(aux_set());
    m.mem_write(0x3FFC, 0x00);
    EXPECT_TRUE(m.aux_decode);
    EXPECT_EQ(m.mem_read(0x8008), 0x11);
}

TEST(Machine, AuxU5GoldenDescrambleAt8008) {
    // i=8, U5 address scramble lands on chip offset 0x10; data 0x01 → 0x80.
    pacman::RomSet s;
    s.aux_board = true;
    s.aux_u5[0x10] = 0x01;
    pacman::Machine m;
    m.load_roms(s);
    (void)m.mem_read(0x3FF8);
    EXPECT_EQ(m.mem_read(0x8008), 0x80);
}

TEST(Machine, WatchdogResetLeavesAuxLatchAlone) {
    pacman::Machine m;
    m.load_roms(aux_set());
    (void)m.mem_read(0x3FF8);
    EXPECT_TRUE(m.aux_decode);
    m.reset();
    EXPECT_TRUE(m.aux_decode) << "PAL latch is not on Z80 RESET";
    EXPECT_EQ(m.mem_read(0x8008), 0x11);
}

// The IM 2 vector table at $3FF8-$3FFF is the latch-set trap.
TEST(Machine, Im2VectorFetchEnablesAuxDecode) {
    pacman::RomSet s = aux_set();
    const uint8_t code[] = {
        0x3E, 0x3F,        // LD A,$3F
        0xED, 0x47,        // LD I,A
        0xED, 0x5E,        // IM 2
        0x3E, 0xFC,        // LD A,$FC
        0xD3, 0x00,        // OUT ($00),A  → vector address $3FFC
        0x3E, 0x01,        // LD A,1
        0x32, 0x00, 0x50,  // LD ($5000),A
        0xFB,              // EI
        0x76,              // HALT
    };
    std::copy(std::begin(code), std::end(code), s.program.begin());
    s.program[0x0100] = 0x3E;  // LD A,$99
    s.program[0x0101] = 0x99;
    s.program[0x0102] = 0x32;  // LD ($4C00),A
    s.program[0x0103] = 0x00;
    s.program[0x0104] = 0x4C;
    s.program[0x0105] = 0x76;
    // $0100 is a patch window ($1000 is, $0100 is not). Handler stays Pac-Man.
    pacman::Machine m;
    m.load_roms(s);
    m.reset();
    EXPECT_FALSE(m.aux_decode);
    m.run_cycles(pacman::kCpuPerFrame * 2);
    EXPECT_TRUE(m.aux_decode) << "IRQ vector fetch at $3FFC should set the latch";
    EXPECT_EQ(m.ram[0], 0x99);
}

}  // namespace

TEST(Machine, CoinCounterCountsRisingEdges) {
    pacman::Machine m;
    m.reset();
    m.mem_write(0x5007, 1);
    m.mem_write(0x5007, 1);
    m.mem_write(0x5007, 0);
    m.mem_write(0x5007, 1);
    EXPECT_EQ(m.coin_counter[0], 2);
}
