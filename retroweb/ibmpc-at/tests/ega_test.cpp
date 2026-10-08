#include <gtest/gtest.h>

#include "ega.h"

#include <cmath>
#include <utility>

namespace {

using ibmpcat::Ega;

// A fresh card ignores writes until a mode set. These helpers program standard EGA/VGA values.

// Mode 3 (80x25 text): odd/even on, Map Mask planes 0+1, B8000 32K window, Bit Mask pass-through.
void SetupTextMode80x25(Ega &ega) {
    ega.out(0x3C4, 0x02); ega.out(0x3C5, 0x03);  // Sequencer Map Mask: planes 0+1
    ega.out(0x3C4, 0x04); ega.out(0x3C5, 0x02);  // Sequencer Memory Mode: odd/even enabled
    ega.out(0x3CE, 0x05); ega.out(0x3CF, 0x10);  // Graphics Mode: odd/even enabled, write mode 0
    ega.out(0x3CE, 0x06); ega.out(0x3CF, 0x0E);  // Misc: alpha mode, chain o/e, B8000 32K window
    ega.out(0x3CE, 0x08); ega.out(0x3CF, 0xFF);  // Bit Mask: all bits pass through
}

// Linear 16-color graphics (e.g. mode 0x10): all planes, odd/even off, 64K@A0000, Set/Reset off.
void SetupLinearGraphics(Ega &ega) {
    ega.out(0x3C4, 0x02); ega.out(0x3C5, 0x0F);  // Map Mask: all 4 planes
    ega.out(0x3C4, 0x04); ega.out(0x3C5, 0x06);  // Memory Mode: odd/even disabled (linear)
    ega.out(0x3CE, 0x01); ega.out(0x3CF, 0x00);  // Enable Set/Reset: none
    ega.out(0x3CE, 0x05); ega.out(0x3CF, 0x00);  // Graphics Mode: write mode 0, odd/even off
    ega.out(0x3CE, 0x06); ega.out(0x3CF, 0x05);  // Misc: graphics mode, 64K @ A0000
    ega.out(0x3CE, 0x08); ega.out(0x3CF, 0xFF);  // Bit Mask: all bits pass through
}

TEST(EgaTest, TextModeMemoryReadWriteRoundTrip) {
    Ega ega;
    ega.reset();
    SetupTextMode80x25(ega);
    ega.mem_write(0xB8000, 'H');
    ega.mem_write(0xB8001, 0x07);  // white-on-black attribute
    EXPECT_EQ(ega.mem_read(0xB8000), 'H');
    EXPECT_EQ(ega.mem_read(0xB8001), 0x07);
}

TEST(EgaTest, MemoryMappingSelectsWhichLegacyWindowIsDecoded) {
    // Only one legacy window is decoded at a time (Memory Mapping field).
    Ega ega;
    ega.reset();
    SetupLinearGraphics(ega);  // starts mapped 64K @ A0000

    ega.mem_write(0xA0000, 0x11);
    EXPECT_EQ(ega.mem_read(0xA0000), 0x11);
    // The color-text window isn't decoded while 64K@A0000 is selected.
    EXPECT_EQ(ega.mem_read(0xB8000), 0xFF);

    ega.out(0x3CE, 0x06); ega.out(0x3CF, 0x0F);  // Misc: graphics mode, 32K @ B8000 color
    ega.mem_write(0xB8000, 0x33);
    EXPECT_EQ(ega.mem_read(0xB8000), 0x33);
    // A0000 keeps its storage but is no longer decoded here.
    EXPECT_EQ(ega.mem_read(0xA0000), 0xFF);
}

TEST(EgaTest, WriteMode0AppliesDataRotateAluFunction) {
    Ega ega;
    ega.reset();
    SetupLinearGraphics(ega);
    ega.mem_write(0xA0000, 0xF0);  // seed all 4 planes with 0xF0

    // Data Rotate function 2 ORs the CPU byte with the latch.
    ega.out(0x3CE, 0x03); ega.out(0x3CF, 0x10);  // rotate=0, function=OR
    ega.mem_read(0xA0000);                       // load the latch from the seeded byte
    ega.mem_write(0xA0000, 0x0F);
    EXPECT_EQ(ega.mem_read(0xA0000), 0xFF);       // 0xF0 | 0x0F
}

TEST(EgaTest, WriteMode1CopiesLatchVerbatimIgnoringCpuByte) {
    Ega ega;
    ega.reset();
    SetupLinearGraphics(ega);
    ega.mem_write(0xA0000, 0xAB);  // source byte, all planes
    ega.mem_read(0xA0000);         // load the latch from the source address

    ega.out(0x3CE, 0x05); ega.out(0x3CF, 0x01);  // Graphics Mode: write mode 1
    ega.mem_write(0xA0004, 0x00);                // CPU byte is irrelevant in mode 1
    EXPECT_EQ(ega.mem_read(0xA0004), 0xAB);
}

TEST(EgaTest, WriteMode2SelectsPlaneValueFromEachCpuBit) {
    Ega ega;
    ega.reset();
    SetupLinearGraphics(ega);
    ega.out(0x3CE, 0x05); ega.out(0x3CF, 0x02);  // Graphics Mode: write mode 2
    // CPU byte 0b0101: planes 0 and 2 get 0xFF, planes 1 and 3 get 0x00.
    ega.mem_write(0xA0000, 0x05);

    ega.out(0x3CE, 0x04); ega.out(0x3CF, 0x00); EXPECT_EQ(ega.mem_read(0xA0000), 0xFF);
    ega.out(0x3CE, 0x04); ega.out(0x3CF, 0x01); EXPECT_EQ(ega.mem_read(0xA0000), 0x00);
    ega.out(0x3CE, 0x04); ega.out(0x3CF, 0x02); EXPECT_EQ(ega.mem_read(0xA0000), 0xFF);
    ega.out(0x3CE, 0x04); ega.out(0x3CF, 0x03); EXPECT_EQ(ega.mem_read(0xA0000), 0x00);
}

TEST(EgaTest, EnableSetResetOverridesCpuByteWithSetResetValue) {
    Ega ega;
    ega.reset();
    SetupLinearGraphics(ega);
    ega.out(0x3CE, 0x00); ega.out(0x3CF, 0x0A);  // Set/Reset: planes 1 and 3 = 1
    ega.out(0x3CE, 0x01); ega.out(0x3CF, 0x0F);  // Enable Set/Reset: all planes
    ega.mem_write(0xA0000, 0x00);                // CPU byte ignored entirely

    ega.out(0x3CE, 0x04); ega.out(0x3CF, 0x00); EXPECT_EQ(ega.mem_read(0xA0000), 0x00);
    ega.out(0x3CE, 0x04); ega.out(0x3CF, 0x01); EXPECT_EQ(ega.mem_read(0xA0000), 0xFF);
    ega.out(0x3CE, 0x04); ega.out(0x3CF, 0x02); EXPECT_EQ(ega.mem_read(0xA0000), 0x00);
    ega.out(0x3CE, 0x04); ega.out(0x3CF, 0x03); EXPECT_EQ(ega.mem_read(0xA0000), 0xFF);
}

TEST(EgaTest, BitMaskProtectsUntouchedBitsOfExistingByte) {
    Ega ega;
    ega.reset();
    SetupLinearGraphics(ega);
    ega.mem_write(0xA0000, 0xFF);                // seed every plane fully set
    ega.out(0x3CE, 0x08); ega.out(0x3CF, 0x0F);  // Bit Mask: only the low nibble is writable
    ega.mem_write(0xA0000, 0x00);
    EXPECT_EQ(ega.mem_read(0xA0000), 0xF0);       // low nibble cleared, high nibble untouched
}

TEST(EgaTest, MapMaskGatesWhichPlanesActuallyStore) {
    Ega ega;
    ega.reset();
    SetupLinearGraphics(ega);
    ega.mem_write(0xA0000, 0xFF);                 // all planes = 0xFF
    ega.out(0x3C4, 0x02); ega.out(0x3C5, 0x03);   // Map Mask: only planes 0+1 writable
    ega.mem_write(0xA0000, 0x00);

    ega.out(0x3CE, 0x04); ega.out(0x3CF, 0x00); EXPECT_EQ(ega.mem_read(0xA0000), 0x00);
    ega.out(0x3CE, 0x04); ega.out(0x3CF, 0x01); EXPECT_EQ(ega.mem_read(0xA0000), 0x00);
    // Planes 2 and 3 weren't in the mask -- their earlier 0xFF survives.
    ega.out(0x3CE, 0x04); ega.out(0x3CF, 0x02); EXPECT_EQ(ega.mem_read(0xA0000), 0xFF);
    ega.out(0x3CE, 0x04); ega.out(0x3CF, 0x03); EXPECT_EQ(ega.mem_read(0xA0000), 0xFF);
}

TEST(EgaTest, ReadMode1ColorCompareMatchesOnlyCaredAboutPlanes) {
    Ega ega;
    ega.reset();
    SetupLinearGraphics(ega);
    // Plane 0 = 0xFF, planes 1-3 = 0x00, set up via write mode 2.
    ega.out(0x3CE, 0x05); ega.out(0x3CF, 0x02);  // write mode 2
    ega.mem_write(0xA0000, 0x01);                 // plane0=0xFF, planes1-3=0x00

    ega.out(0x3CE, 0x02); ega.out(0x3CF, 0x01);  // Color Compare: plane0 wants 1
    ega.out(0x3CE, 0x07); ega.out(0x3CF, 0x01);  // Color Don't Care: only plane0 cared about
    ega.out(0x3CE, 0x05); ega.out(0x3CF, 0x08);  // Graphics Mode: read mode 1
    EXPECT_EQ(ega.mem_read(0xA0000), 0xFF);       // every bit matches on the one cared-about plane

    ega.out(0x3CE, 0x02); ega.out(0x3CF, 0x00);  // now want plane0 = 0 -- no longer matches
    EXPECT_EQ(ega.mem_read(0xA0000), 0x00);
}

TEST(EgaTest, OddEvenChainingRoutesCharacterGeneratorWritesToPlanes2And3) {
    // Font loads reach plane 2 through the A0000 window with odd/even on and Map Mask on plane 2. An even address hits plane 2.
    Ega ega;
    ega.reset();
    ega.out(0x3C4, 0x02); ega.out(0x3C5, 0x04);  // Map Mask: plane 2 only
    ega.out(0x3C4, 0x04); ega.out(0x3C5, 0x02);  // Memory Mode: odd/even enabled
    ega.out(0x3CE, 0x05); ega.out(0x3CF, 0x00);  // Graphics Mode: write mode 0
    ega.out(0x3CE, 0x06); ega.out(0x3CF, 0x04);  // Misc: 64K @ A0000 window
    ega.out(0x3CE, 0x08); ega.out(0x3CF, 0xFF);  // Bit Mask: all bits pass through
    ega.mem_write(0xA0000, 0x7E);                 // even address -- reaches plane 2

    ega.out(0x3CE, 0x04); ega.out(0x3CF, 0x02);  // Read Map Select: plane 2
    EXPECT_EQ(ega.mem_read(0xA0000), 0x7E);
    // Planes 0/1 are untouched.
    ega.out(0x3CE, 0x04); ega.out(0x3CF, 0x00);
    EXPECT_EQ(ega.mem_read(0xA0000), 0x00);
}

TEST(EgaTest, CrtcCursorAndStartAddressRegisters) {
    Ega ega;
    ega.reset();
    ega.out(0x3D4, 0x0E); ega.out(0x3D5, 0x01);  // cursor location high
    ega.out(0x3D4, 0x0F); ega.out(0x3D5, 0x40);  // cursor location low
    EXPECT_EQ(ega.cursor_offset(), 0x0140);

    ega.out(0x3D4, 0x0C); ega.out(0x3D5, 0x00);  // start address high
    ega.out(0x3D4, 0x0D); ega.out(0x3D5, 0x00);  // start address low
    EXPECT_EQ(ega.start_offset(), 0x0000);
}

TEST(EgaTest, CursorShapeRegistersReportStartEndAndDisableBit) {
    Ega ega;
    ega.reset();
    ega.out(0x3D4, 0x0A); ega.out(0x3D5, 0x0D);  // Cursor Start: scanline 13, not disabled
    ega.out(0x3D4, 0x0B); ega.out(0x3D5, 0x0E);  // Cursor End: scanline 14
    EXPECT_FALSE(ega.cursor_disabled());
    EXPECT_EQ(ega.cursor_start_scanline(), 13);
    EXPECT_EQ(ega.cursor_end_scanline(), 14);

    ega.out(0x3D4, 0x0A); ega.out(0x3D5, 0x20);  // bit 5 set -- cursor off
    EXPECT_TRUE(ega.cursor_disabled());
}

TEST(EgaTest, AttrPaletteReportsTheLiveRegisterMaskedToSixBits) {
    Ega ega;
    ega.reset();
    ega.out(0x3C0, 0x05);  // index = palette register 5
    ega.out(0x3C0, 0xFF);  // data -- only the low 6 bits are a real EGA color
    EXPECT_EQ(ega.attr_palette(5), 0x3F);
    // Untouched registers still read back their reset value (0).
    EXPECT_EQ(ega.attr_palette(6), 0x00);
}

TEST(EgaTest, AttributeControllerFlipFlopAlternatesIndexAndData) {
    Ega ega;
    ega.reset();
    ega.out(0x3C0, 0x00);  // first write after reset = index (palette register 0)
    ega.out(0x3C0, 0x3F);  // second write = data
    ega.out(0x3C0, 0x00);  // back to index again
    EXPECT_EQ(ega.in(0x3C1), 0x3F);
}

TEST(EgaTest, ReadingInputStatusOneResetsAttributeFlipFlop) {
    Ega ega;
    ega.reset();
    ega.out(0x3C0, 0x00);  // consumes the "index" half of the flip-flop
    ega.in(0x3DA);         // real hardware: this forces the next 0x3C0 write back to "index"
    ega.out(0x3C0, 0x01);  // index = 1 (not data for register 0)
    ega.out(0x3C0, 0x2A);  // now this is data for register 1
    EXPECT_EQ(ega.in(0x3C1), 0x2A);
}

TEST(EgaTest, RetraceBitToggledByTick) {
    Ega ega;
    ega.reset();
    bool saw_true = false, saw_false = false;
    for (uint64_t c = 0; c < 2'000'000; c += 500) {
        ega.tick(c);
        if (ega.in(0x3DA) & 0x08) saw_true = true; else saw_false = true;
    }
    EXPECT_TRUE(saw_true);
    EXPECT_TRUE(saw_false);
}

// IBM EGA BIOS mode 3 at 350 lines: 21.85 kHz, 60 Hz.
void ProgramEga350LineText(Ega &ega) {
    ega.out(0x3C2, 0xA7);
    ega.out(0x3C4, 0x01); ega.out(0x3C5, 0x01);
    const std::pair<uint8_t, uint8_t> crtc[] = {
        {0x00, 0x5B}, {0x01, 0x4F}, {0x06, 0x6C}, {0x07, 0x1F},
        {0x10, 0x5E}, {0x11, 0x2B}, {0x12, 0x5D},
    };
    for (auto [i, v] : crtc) { ega.out(0x3D4, i); ega.out(0x3D5, v); }
}

TEST(EgaTest, RasterTimingComesFromTheCrtcAndClock) {
    Ega ega;
    ega.reset();
    ProgramEga350LineText(ega);
    int retraces = 0, lines = 0;
    bool was_retrace = true, was_blank = true;
    uint64_t c = 0;
    for (; c < 8000000; c += 4) {
        ega.tick(c);
        uint8_t st = ega.in(0x3DA);
        bool retrace = st & 0x08, blank = st & 0x01;
        if (retrace && !was_retrace) ++retraces;
        if (!blank && was_blank) ++lines;
        was_retrace = retrace; was_blank = blank;
    }
    EXPECT_EQ(retraces, 60) << "one second of 60.04 Hz frames";
    EXPECT_NEAR(lines, 350 * 60, 60) << "display enable comes on once per displayed line";
}

TEST(EgaTest, VerticalRetraceSpansTheProgrammedLines) {
    Ega ega;
    ega.reset();
    ProgramEga350LineText(ega);
    double line = 93.0 * 8 / 16257000.0 * 8000000.0;
    auto status_at_line = [&](double l) { Ega e = ega; e.tick(uint64_t(l * line)); return e.in(0x3DA); };
    EXPECT_FALSE(status_at_line(349.5) & 0x08);
    EXPECT_TRUE(status_at_line(350.5) & 0x08);
    EXPECT_TRUE(status_at_line(362.5) & 0x08);
    EXPECT_FALSE(status_at_line(363.5) & 0x08);
    EXPECT_TRUE(status_at_line(355.5) & 0x01) << "no display during retrace";
    EXPECT_FALSE(status_at_line(100.1) & 0x01) << "early in a displayed line";
    EXPECT_TRUE(status_at_line(100.95) & 0x01) << "horizontal blanking at the end of the line";
}

TEST(EgaTest, ClockSelectAndDotClockHalvingChangeTheRate) {
    Ega ega;
    ega.reset();
    ProgramEga350LineText(ega);
    ega.out(0x3C4, 0x01); ega.out(0x3C5, 0x09);  // dot clock / 2
    int retraces = 0;
    bool was = true;
    for (uint64_t c = 0; c < 8000000; c += 4) {
        ega.tick(c);
        bool r = ega.in(0x3DA) & 0x08;
        if (r && !was) ++retraces;
        was = r;
    }
    EXPECT_EQ(retraces, 30);
}

// One processor cycle in five, five cycles to 32 dots: 1.97 us at 16.257 MHz (IBM_PCAT_REVIEW.md §47.3).
TEST(EgaTest, BackToBackCpuAccessesTakeOneSlotPerFiveMemoryCycles) {
    Ega ega;
    ega.reset();
    ProgramEga350LineText(ega);
    uint64_t now = 0;
    for (int i = 0; i < 1000; ++i) now += ega.cpu_access_clocks(0xB8000, now);
    EXPECT_NEAR(double(now) / 1000, 32 / 16.257e6 * 8e6, 0.05);
}

TEST(EgaTest, MediumResolutionBandwidthGivesTheCpuThreeSlotsInFive) {
    Ega ega;
    ega.reset();
    ProgramEga350LineText(ega);
    ega.out(0x3C4, 0x01); ega.out(0x3C5, 0x0B);  // Clocking Mode bit 1, dot clock / 2
    uint64_t now = 0;
    for (int i = 0; i < 999; ++i) now += ega.cpu_access_clocks(0xA0000, now);
    EXPECT_LT(double(now) / 999, 32 / 16.257e6 * 8e6 / 2);
}

TEST(EgaTest, AnAccessAfterAnIdleGapWaitsOnlyForTheNextSlot) {
    Ega ega;
    ega.reset();
    ProgramEga350LineText(ega);
    double cycle = 32 / 16.257e6 * 8e6 / 5;
    EXPECT_EQ(ega.cpu_access_clocks(0xB8000, 0), int(std::ceil(5 * cycle))) << "the fifth cycle, then its own length";
    double slot = 4 * cycle;
    while (slot < 1000) slot += 5 * cycle;
    EXPECT_EQ(ega.cpu_access_clocks(0xB8000, 1000), int(std::ceil(slot + cycle - 1000)));
}

TEST(EgaTest, AccessesOutsideTheMappedWindowDoNotWait) {
    Ega ega;
    ega.reset();
    ProgramEga350LineText(ega);
    ega.out(0x3CE, 0x06); ega.out(0x3CF, 0x0C);  // 32K at B8000
    EXPECT_EQ(ega.cpu_access_clocks(0xA0000, 0), 0);
    EXPECT_GT(ega.cpu_access_clocks(0xB8000, 0), 0);
}

}  // namespace
