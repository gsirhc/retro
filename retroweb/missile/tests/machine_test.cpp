#include <gtest/gtest.h>

#include "machine.h"

#include <array>
#include <initializer_list>

namespace {

using missile::Machine;

// Program at $5000, RESET → $5000, IRQ → $6000.
missile::RomSet rom(std::initializer_list<uint8_t> code,
                    std::initializer_list<uint8_t> irq = {0x40}) {
    missile::RomSet s;
    size_t i = 0;
    for (uint8_t b : code) s.program[i++] = b;
    i = 0x1000;
    for (uint8_t b : irq) s.program[i++] = b;
    s.program[0x2FFC] = 0x00;
    s.program[0x2FFD] = 0x50;
    s.program[0x2FFE] = 0x00;
    s.program[0x2FFF] = 0x60;
    return s;
}

// Ack the power-on V=0 IRQ so /IRQH is clear by the third SYNC.
void ack(Machine& m) { m.mem_write(0x4D00, 0); }

void run_to_line(Machine& m, int line) {
    while (m.line() != line) m.run_cycles(1);
}

}  // namespace

TEST(Timing, FrameIs19200CyclesAt61Hz) {
    EXPECT_EQ(missile::kCyclesPerFrame, 19200);
    EXPECT_NEAR(missile::kFrameHz, 61.035, 0.001);
    EXPECT_EQ(missile::kAvgCpuHz, int(missile::kCyclesPerFrame * missile::kFrameHz));
}

TEST(Timing, SigmaXHalvesClockFromV224) {
    Machine m;
    m.load_roms(rom({0x4C, 0x00, 0x50}));
    int t = 0;
    while (m.line() < 224) t += m.run_cycles(1);
    EXPECT_NEAR(t, 224 * 80, 3);
    int t2 = 0;
    while (m.line() == 224) t2 += m.run_cycles(1);
    EXPECT_NEAR(t2, 40, 3);
    while (m.frames == 0) t += m.run_cycles(1);
    EXPECT_NEAR(t + t2, 19200, 6);
}

TEST(Irq, ThirtyTwoVEdgesAssertAtV0_64_128_192) {
    Machine m;
    m.load_roms(rom({0x4C, 0x00, 0x50}));  // I stays set: pin follows F7
    run_to_line(m, 10);
    m.run_cycles(4);
    EXPECT_TRUE(m.irq_pin());
    run_to_line(m, 40);
    m.run_cycles(4);
    EXPECT_FALSE(m.irq_pin());
    run_to_line(m, 70);
    m.run_cycles(4);
    EXPECT_TRUE(m.irq_pin());
    run_to_line(m, 200);
    m.run_cycles(4);
    EXPECT_TRUE(m.irq_pin());
    run_to_line(m, 230);
    m.run_cycles(4);
    EXPECT_FALSE(m.irq_pin());
}

TEST(Irq, FourPerFrameWithAck) {
    // main: CLI; JMP *. irq: INC $80; STA $4D00; STA $4C00; RTI
    Machine m;
    m.load_roms(rom({0x58, 0x4C, 0x01, 0x50},
                    {0xE6, 0x80, 0x8D, 0x00, 0x4D, 0x8D, 0x00, 0x4C, 0x40}));
    for (int f = 0; f < 10; f++) m.run_cycles(missile::kCyclesPerFrame);
    EXPECT_NEAR(m.mem_read(0x80), 40, 1);
    EXPECT_FALSE(m.watchdog_reset);
}

TEST(Irq, AckClearsPendingState) {
    Machine m;
    m.load_roms(rom({0x4C, 0x00, 0x50}));
    run_to_line(m, 10);
    m.run_cycles(4);
    ASSERT_TRUE(m.irq_pin());
    m.mem_write(0x4D00, 0);
    m.run_cycles(8);
    EXPECT_FALSE(m.irq_pin());
}

TEST(Watchdog, BitesAfterEightFramesUnfed) {
    Machine m;
    m.load_roms(rom({0x4C, 0x00, 0x50}));
    m.vram[0x0100] = 0x5A;
    for (int f = 0; f < 7; f++) m.run_cycles(missile::kCyclesPerFrame);
    EXPECT_FALSE(m.watchdog_reset);
    m.run_cycles(2 * missile::kCyclesPerFrame);
    EXPECT_TRUE(m.watchdog_reset);
    EXPECT_EQ(m.vram[0x0100], 0x5A);  // DRAM survives /RESET
}

TEST(Madsel, StaIndexedIndirectWritesOnePixel) {
    // LDX #0; LDA #$C0; STA ($10,X); JMP *
    Machine m;
    m.load_roms(rom({0xA2, 0x00, 0xA9, 0xC0, 0x81, 0x10, 0x4C, 0x06, 0x50}));
    ack(m);
    m.vram[0x10] = 0x80;
    m.vram[0x11] = 0x80;
    m.vram[0x2020] = 0x00;
    m.run_cycles(2 + 2 + 6);
    EXPECT_EQ(m.vram[0x2020], 0x11);
    EXPECT_EQ(m.vram[0x0080], 0x00);  // not decoded as a plain store
}

TEST(Madsel, KeepsNeighbourPixels) {
    Machine m;
    m.load_roms(rom({0xA2, 0x00, 0xA9, 0x40, 0x81, 0x10, 0x4C, 0x06, 0x50}));
    ack(m);
    m.vram[0x10] = 0x81;  // pixel 1 of byte $2020
    m.vram[0x11] = 0x80;
    m.vram[0x2020] = 0xFF;
    m.run_cycles(10);
    // $40 → colour %01: clear pixel 1's high bit, set its low bit.
    EXPECT_EQ(m.vram[0x2020], 0xDF);
}

TEST(Madsel, LdaIndexedIndirectReadsPixelBits) {
    // LDX #0; LDA ($10,X); STA $90; LDA ($12,X); STA $91; JMP *
    Machine m;
    m.load_roms(rom({0xA2, 0x00, 0xA1, 0x10, 0x85, 0x90, 0xA1, 0x12, 0x85, 0x91,
                     0x4C, 0x0A, 0x50}));
    ack(m);
    m.vram[0x10] = 0x80; m.vram[0x11] = 0x80;
    m.vram[0x12] = 0x81; m.vram[0x13] = 0x80;
    m.vram[0x2020] = 0x01;  // pixel 0: low bit only
    m.run_cycles(60);
    EXPECT_EQ(m.mem_read(0x90), 0x7F);
    EXPECT_EQ(m.mem_read(0x91), 0x1F);
}

TEST(Madsel, ThirdBitPlaneCostsOneCycle) {
    // LDX #0; LDA #$E0; STA ($10,X) at $E0xx
    Machine m;
    m.load_roms(rom({0xA2, 0x00, 0xA9, 0xE0, 0x81, 0x10, 0x4C, 0x06, 0x50}));
    ack(m);
    m.vram[0x10] = 0x05;
    m.vram[0x11] = 0xE8;
    int c = m.run_cycles(1);
    c += m.run_cycles(1);
    c += m.run_cycles(1);
    EXPECT_EQ(c, 2 + 2 + 7);
    EXPECT_EQ(m.madsel_hits, 1);
    EXPECT_EQ(m.vram[0xE805 >> 2] & (0x11 << 1), 0x11 << 1);
    EXPECT_EQ(m.vram[0x0400] & (1 << 5), 1 << 5);
}

TEST(Madsel, OnlyOpcodesWithLow5Bits01) {
    // LDX #0; LDA #$C0; STA $8080 (absolute) → plain store at $0080
    Machine m;
    m.load_roms(rom({0xA2, 0x00, 0xA9, 0xC0, 0x8D, 0x80, 0x80, 0x4C, 0x07, 0x50}));
    ack(m);
    m.run_cycles(8);
    EXPECT_EQ(m.vram[0x0080], 0xC0);
    EXPECT_EQ(m.vram[0x2020], 0x00);
}

TEST(Madsel, BlockedWhileIrqhAsserted) {
    // SEI is the reset state, so a pending IRQ holds /IRQH and MADSEL never loads.
    Machine m;
    m.load_roms(rom({0x4C, 0x00, 0x50,
                     0xA2, 0x00, 0xA9, 0xC0, 0x81, 0x10, 0x4C, 0x0A, 0x50}));
    run_to_line(m, 10);
    m.run_cycles(4);
    ASSERT_TRUE(m.irq_pin());
    m.vram[0x10] = 0x80;
    m.vram[0x11] = 0x80;
    m.cpu.pc = 0x5003;
    m.run_cycles(10);
    EXPECT_EQ(m.vram[0x2020], 0x00);
    EXPECT_EQ(m.vram[0x0080], 0xC0);
}

TEST(Io, TrackballOnCtrldElseIn0) {
    Machine m;
    m.inputs.in0 = 0xEF;
    m.inputs.track_x = 0x13;
    m.inputs.track_y = 0x05;
    EXPECT_EQ(m.mem_read(0x4800), 0xEF);
    m.mem_write(0x4800, 0x41);
    EXPECT_EQ(m.mem_read(0x48FF), 0x53);
    m.mem_write(0x4800, 0x40);
    EXPECT_EQ(m.mem_read(0x4800), 0xEF);
}

TEST(Io, FlipIsActiveLowOnOut0Bit6) {
    Machine m;
    m.mem_write(0x4800, 0x40);
    EXPECT_FALSE(m.flipscreen);
    m.mem_write(0x4800, 0x00);
    EXPECT_TRUE(m.flipscreen);
}

TEST(Io, In1VblankAndSwitches) {
    Machine m;
    m.load_roms(rom({0x4C, 0x00, 0x50}));
    m.inputs.in1 = 0x63;  // left fire held
    run_to_line(m, 5);
    EXPECT_EQ(m.mem_read(0x4900), 0x80 | 0x63);
    run_to_line(m, 30);
    EXPECT_EQ(m.mem_read(0x49AA), 0x63);
}

TEST(Io, R8ViaPokeyAllpotAndR10) {
    Machine m;
    EXPECT_EQ(m.mem_read(0x4008), missile::kR8Default);
    EXPECT_EQ(m.mem_read(0x47F8), missile::kR8Default);
    EXPECT_EQ(m.mem_read(0x4A00), missile::kR10Default);
    m.inputs.r10 = 0x02;
    EXPECT_EQ(m.mem_read(0x4A55), 0x02);
}

TEST(Io, ProgramMirrorsAboveA15) {
    Machine m;
    auto s = rom({0x4C, 0x00, 0x50});
    s.program[0x1234] = 0xA5;
    m.load_roms(s);
    EXPECT_EQ(m.mem_read(0x6234), 0xA5);
    EXPECT_EQ(m.mem_read(0xE234), 0xA5);
}

TEST(Video, PaletteBitsInvertedRgb) {
    Machine m;
    m.mem_write(0x4B06, 0x06);  // ~ → R on, G off, B off
    m.vram[128 * 64 + 32] = 0x11;
    std::array<uint32_t, missile::kFbW * missile::kFbH> fb{};
    m.render(fb.data());
    EXPECT_EQ(fb[size_t((128 - missile::kVisTop) * missile::kFbW + 128)], 0xFF0000FFu);
}

TEST(Video, BottomRowsAddThirdBit) {
    Machine m;
    m.mem_write(0x4B01, 0x08);  // pen 1: R off, G on, B on
    // Line 240 is BA = $F000, so BA12 = 1 and BA11 = 0 put its bits at $0201.
    m.vram[0x0201] = 0x01;
    std::array<uint32_t, missile::kFbW * missile::kFbH> fb{};
    m.render(fb.data());
    EXPECT_EQ(fb[size_t((240 - missile::kVisTop) * missile::kFbW + 0)], 0xFFFFFF00u);
}

TEST(Audio, PokeyRunsAtRealTimeThroughSlowLines) {
    Machine m;
    m.load_roms(rom({0x4C, 0x00, 0x50}));
    m.audio_hz = 48000;
    while (m.frames < 10) m.run_cycles(100);
    double per_frame = double(m.audio.size()) / 10.0;
    EXPECT_NEAR(per_frame, 48000.0 / missile::kFrameHz, 2.0);
}

TEST(Rom, SetWithoutPromRestoresDefaultMasks) {
    Machine m;
    auto withp = rom({0x4C, 0x00, 0x50});
    withp.has_prom = true;
    withp.prom.fill(0x00);
    m.load_roms(withp);
    EXPECT_EQ(m.prom[0x10], 0x00);
    m.load_roms(rom({0x4C, 0x00, 0x50}));
    EXPECT_EQ(m.prom[0x10], 0xEE);
    EXPECT_EQ(m.prom[0x18], 0xFE);
}

TEST(Flip, IrqEdgesOnInverted16V) {
    Machine m;
    m.load_roms(rom({0x4C, 0x00, 0x50}));
    m.mem_write(0x4800, 0x00);
    m.mem_write(0x4D00, 0);
    run_to_line(m, 20);  // V=236: deassert edge at V=240
    m.run_cycles(4);
    EXPECT_FALSE(m.irq_pin());
    run_to_line(m, 50);  // V=206: assert edge at V=208
    m.run_cycles(4);
    EXPECT_TRUE(m.irq_pin());
    run_to_line(m, 85);  // V=171: deassert edge at V=176
    m.run_cycles(4);
    EXPECT_FALSE(m.irq_pin());
}

TEST(Flip, VblankAndSigmaXFollowFlippedV) {
    Machine m;
    m.load_roms(rom({0x4C, 0x00, 0x50}));
    m.mem_write(0x4800, 0x00);
    run_to_line(m, 10);
    EXPECT_FALSE(m.vblank());
    EXPECT_EQ(m.v(), 246);
    run_to_line(m, 240);
    EXPECT_TRUE(m.vblank());
    int t = 0;
    while (m.line() == 20) t += m.run_cycles(1);
    run_to_line(m, 20);
    t = 0;
    while (m.line() == 20) t += m.run_cycles(1);
    EXPECT_NEAR(t, 40, 3);  // V=236 is in the slow band
}

TEST(Flip, RenderReadsRowsBottomUp) {
    Machine m;
    m.mem_write(0x4800, 0x00);
    m.mem_write(0x4B06, 0x06);
    m.vram[255 * 64] = 0x11;
    std::array<uint32_t, missile::kFbW * missile::kFbH> fb{};
    m.render(fb.data());
    EXPECT_EQ(fb[0], 0xFF0000FFu);
}

TEST(Flip, CocktailTrackballReadsIdle) {
    Machine m;
    m.inputs.track_x = 0x05;
    m.inputs.track_y = 0x03;
    m.mem_write(0x4800, 0x01);
    EXPECT_EQ(m.mem_read(0x4800), 0x00);
}

TEST(Madsel, ThirdColourBytesLandInTheScreenGapOfWorkRam) {
    // BA12 is MAD0, BA11 is MAD10 and inverted MAD9, BA10-BA3 are MAD8-MAD1.
    Machine m;
    m.mad_write(0xE000, 0x20);
    EXPECT_EQ(m.vram[0x0200] & 0x01, 0x01);
    m.mad_write(0xF000, 0x20);
    EXPECT_EQ(m.vram[0x0201] & 0x01, 0x01);
    m.mad_write(0xE800, 0x20);
    EXPECT_EQ(m.vram[0x0400] & 0x01, 0x01);
    m.mad_write(0xE008, 0x20);
    EXPECT_EQ(m.vram[0x0202] & 0x01, 0x01);
    m.mad_write(0xE400, 0x20);
    EXPECT_EQ(m.vram[0x0300] & 0x01, 0x01);
    m.mad_write(0xE007, 0x20);
    EXPECT_EQ(m.vram[0x0200] & 0x80, 0x80);
}

TEST(Madsel, TwoColourWriteUsesFourteenBitPixelAddress) {
    Machine m;
    m.mad_write(0x4003, 0xC0);
    EXPECT_EQ(m.vram[0x1000], 0x88);
    m.mad_write(0xC1FC, 0xC0);
    EXPECT_EQ(m.vram[0x307F], 0x11);
}

TEST(Madsel, WriteSendsD7ToHighNibbleChipsAndD6ToLow) {
    Machine m;
    m.mad_write(0x4001, 0x80);
    EXPECT_EQ(m.vram[0x1000], 0x20);
    m.mad_write(0x4001, 0x40);
    EXPECT_EQ(m.vram[0x1000], 0x02);
}

TEST(Madsel, WriteHonoursPromChipSelects) {
    Machine m;
    m.prom.fill(0xFF);
    m.prom[0x10] = 0xFE;
    m.mad_write(0x4000, 0xC0);
    EXPECT_EQ(m.vram[0x1000], 0x01);
}

TEST(Madsel, DefaultPromEnablesEveryChipForWorkRamRows) {
    Machine m;
    for (int i = 0; i < 16; i++) EXPECT_EQ(m.prom[size_t(i)], 0x00) << i;
    EXPECT_EQ(m.prom[0x1B], 0xF7);
    EXPECT_EQ(m.prom[0x1F], 0x7F);
}

TEST(Madsel, ReadReturnsHighNibbleOnD7LowOnD6AndByteBitOnD5) {
    Machine m;
    m.vram[0x1000] = 0x24;
    EXPECT_EQ(m.mad_read(0x4000), 0x1F);
    EXPECT_EQ(m.mad_read(0x4001), 0x1F | 0x80);
    EXPECT_EQ(m.mad_read(0x4002), 0x1F | 0x40 | 0x20);
    m.vram[0x1000] = 0x01;
    EXPECT_EQ(m.mad_read(0x4000), 0x1F | 0x40 | 0x20);
}

TEST(Madsel, ThirdColourRangeReadsBitFromThirdColourByteOnD5) {
    Machine m;
    m.vram[0x0200] = 0x04;
    m.vram[0xE000 >> 2] = 0x02;
    EXPECT_EQ(m.mad_read(0xE002), 0x1F | 0x20);
    EXPECT_EQ(m.mad_read(0xE001), 0x1F | 0x40);
}

TEST(Video, FourthPaletteBitIsNotAColour) {
    Machine m;
    m.mem_write(0x4B00, 0x01);
    std::array<uint32_t, missile::kFbW * missile::kFbH> fb{};
    m.render(fb.data());
    EXPECT_EQ(fb[0], 0xFFFFFFFFu);
}

TEST(Video, FirstShownLineIsRow25) {
    EXPECT_EQ(missile::kVisTop, 25);
    Machine m;
    m.mem_write(0x4B02, 0x0D);
    m.vram[25 * 64] = 0x01;
    std::array<uint32_t, missile::kFbW * missile::kFbH> fb{};
    m.render(fb.data());
    EXPECT_EQ(fb[0], 0xFFFF0000u);
}

TEST(Video, PixelBitOrderFollowsTheShiftRegisters) {
    Machine m;
    m.mem_write(0x4B02, 0x0D);
    m.mem_write(0x4B04, 0x07);
    m.vram[100 * 64] = 0x21;
    std::array<uint32_t, missile::kFbW * missile::kFbH> fb{};
    m.render(fb.data());
    size_t base = size_t((100 - missile::kVisTop) * missile::kFbW);
    EXPECT_EQ(fb[base + 0], 0xFFFF0000u);
    EXPECT_EQ(fb[base + 1], 0xFF0000FFu);
}
