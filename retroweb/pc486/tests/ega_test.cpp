// GoogleTest suite for the EGA/VGA device: planar memory engine, CRTC/AC registers,
// retrace timing, Chain 4, the DAC and the SVGA extension registers. See PC486_REVIEW.md §7.

#include <gtest/gtest.h>

#include <cstddef>
#include <ios>
#include <vector>

#include "ega.h"

namespace {

using pc486::Ega;

// A freshly reset card has no planes enabled and a zero Bit Mask, so writes are no-ops
// until a mode set. These helpers program standard IBM EGA/VGA values (as the Bochs vgabios does).

// Mode 3 (80x25 text): odd/even on, Map Mask planes 0+1, 32K color-text window @ B8000.
void SetupTextMode80x25(Ega &ega) {
    ega.out(0x3C4, 0x02); ega.out(0x3C5, 0x03);  // Sequencer Map Mask: planes 0+1
    ega.out(0x3C4, 0x04); ega.out(0x3C5, 0x02);  // Sequencer Memory Mode: odd/even enabled
    ega.out(0x3CE, 0x05); ega.out(0x3CF, 0x10);  // Graphics Mode: odd/even enabled, write mode 0
    ega.out(0x3CE, 0x06); ega.out(0x3CF, 0x0E);  // Misc: alpha mode, chain o/e, B8000 32K window
    ega.out(0x3CE, 0x08); ega.out(0x3CF, 0xFF);  // Bit Mask: all bits pass through
}

// 16-color graphics shape (mode 0x10): all planes, odd/even off, 64K@A0000, write mode 0, no Set/Reset.
void SetupLinearGraphics(Ega &ega) {
    ega.out(0x3C4, 0x02); ega.out(0x3C5, 0x0F);  // Map Mask: all 4 planes
    ega.out(0x3C4, 0x04); ega.out(0x3C5, 0x06);  // Memory Mode: odd/even disabled (linear)
    ega.out(0x3CE, 0x01); ega.out(0x3CF, 0x00);  // Enable Set/Reset: none
    ega.out(0x3CE, 0x05); ega.out(0x3CF, 0x00);  // Graphics Mode: write mode 0, odd/even off
    ega.out(0x3CE, 0x06); ega.out(0x3CF, 0x05);  // Misc: graphics mode, 64K @ A0000
    ega.out(0x3CE, 0x08); ega.out(0x3CF, 0xFF);  // Bit Mask: all bits pass through
}

// Programs only the registers recompute_timing_() reads: Horizontal/Vertical Total (+ Overflow),
// Vertical Retrace Start/End, Clocking Mode and the dot-clock select.
void ProgramCrtcTiming(Ega &ega, uint8_t htotal, uint8_t vtotal, uint8_t overflow,
                        uint8_t vrs, uint8_t vre_low4, uint8_t seq_clocking_mode,
                        uint8_t misc_output) {
    ega.out(0x3D4, 0x00); ega.out(0x3D5, htotal);
    ega.out(0x3D4, 0x06); ega.out(0x3D5, vtotal);
    ega.out(0x3D4, 0x07); ega.out(0x3D5, overflow);
    ega.out(0x3D4, 0x10); ega.out(0x3D5, vrs);
    ega.out(0x3D4, 0x11); ega.out(0x3D5, uint8_t(vre_low4 & 0x0F));
    ega.out(0x3C4, 0x01); ega.out(0x3C5, seq_clocking_mode);
    ega.out(0x3C2, misc_output);
}

// Ticks ega for `span` cycles from `*cursor` (tick() needs a non-decreasing count; the cursor is
// left just past the last cycle) and returns the cycle of every 0x3DA bit 3 rising edge.
std::vector<uint64_t> RetraceOnsets(Ega &ega, uint64_t &cursor, uint64_t span) {
    std::vector<uint64_t> onsets;
    bool prev = (ega.in(0x3DA) & 0x08) != 0;
    uint64_t end = cursor + span;
    for (uint64_t c = cursor; c <= end; ++c) {
        ega.tick(c);
        bool now = (ega.in(0x3DA) & 0x08) != 0;
        if (now && !prev) onsets.push_back(c);
        prev = now;
    }
    cursor = end + 1;
    return onsets;
}

// Length of the first retrace pulse within `span` cycles from `*cursor`, onset to falling edge.
uint64_t MeasureRetraceWindowCycles(Ega &ega, uint64_t &cursor, uint64_t span) {
    bool prev = (ega.in(0x3DA) & 0x08) != 0;
    uint64_t rising = 0;
    bool have_rising = false;
    uint64_t end = cursor + span;
    uint64_t result = 0;
    for (uint64_t c = cursor; c <= end; ++c) {
        ega.tick(c);
        bool now = (ega.in(0x3DA) & 0x08) != 0;
        if (!have_rising) {
            if (now && !prev) { rising = c; have_rising = true; }
        } else if (prev && !now && result == 0) {
            result = c - rising;
        }
        prev = now;
    }
    cursor = end + 1;
    return result;  // 0 means no full pulse was seen within span -- caller asserts on that
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
    // Only one of the three legacy windows (64K@A0000, 32K@B0000, 32K@B8000) is decoded, per Memory Mapping.
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
    // A0000's storage is intact but no longer decoded from this address.
    EXPECT_EQ(ega.mem_read(0xA0000), 0xFF);
}

TEST(EgaTest, LegacyPlanarStrideStaysFixedRegardlessOfTheEnlargedVram) {
    // The plane interleave is the literal (plane_off << 2) + plane, not derived from vram.size(), so
    // growing vram must not move legacy bytes. The top of the A0000 window lands at 0xFFFF*4 = 262,140.
    Ega ega;
    ega.reset();
    SetupLinearGraphics(ega);
    ega.mem_write(0xAFFFF, 0x42);
    for (int p = 0; p < 4; ++p) EXPECT_EQ(ega.vram[std::size_t(262140 + p)], 0x42) << "plane " << p;
    EXPECT_EQ(ega.vram[262144], 0x00);  // one byte into the "new" megabyte: untouched
}

TEST(EgaTest, WriteMode0AppliesDataRotateAluFunction) {
    Ega ega;
    ega.reset();
    SetupLinearGraphics(ega);
    ega.mem_write(0xA0000, 0xF0);  // seed all 4 planes with 0xF0

    // Data Rotate: rotate 0, function 2 = OR the CPU byte with the latch.
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
    // Planes 2 and 3 are outside the mask and keep 0xFF.
    ega.out(0x3CE, 0x04); ega.out(0x3CF, 0x02); EXPECT_EQ(ega.mem_read(0xA0000), 0xFF);
    ega.out(0x3CE, 0x04); ega.out(0x3CF, 0x03); EXPECT_EQ(ega.mem_read(0xA0000), 0xFF);
}

TEST(EgaTest, ReadMode1ColorCompareMatchesOnlyCaredAboutPlanes) {
    Ega ega;
    ega.reset();
    SetupLinearGraphics(ega);
    // Plane 0 = 0xFF, plane 1 = 0x00, set up with a mode-2 write.
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
    // The BIOS font-load routine reaches plane 2 through the 64K@A0000 window with odd/even on
    // and Map Mask on plane 2. An even address reaches plane 2, an odd one plane 3.
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
    // Planes 0 and 1 are untouched by a write routed to plane 2.
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
    // Untouched registers read back 0.
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

// --- VGA DAC (ports 0x3C6-0x3C9) -----------------------------------------

TEST(EgaTest, DacWritesAutoAdvanceThroughRgbAndOnToTheNextEntry) {
    // One PEL Address Write index, then every third PEL Data write rolls to the next color register.
    Ega ega;
    ega.reset();
    ega.out(0x3C8, 5);
    ega.out(0x3C9, 63); ega.out(0x3C9, 21); ega.out(0x3C9, 0);   // entry 5
    ega.out(0x3C9, 1);  ega.out(0x3C9, 2);  ega.out(0x3C9, 3);   // entry 6, no new index

    uint8_t r, g, b;
    ega.dac_entry(5, r, g, b);
    EXPECT_EQ(r, 63); EXPECT_EQ(g, 21); EXPECT_EQ(b, 0);
    ega.dac_entry(6, r, g, b);
    EXPECT_EQ(r, 1); EXPECT_EQ(g, 2); EXPECT_EQ(b, 3);
    // The write index moved on rather than wrapping.
    EXPECT_EQ(ega.in(0x3C8), 7);
}

TEST(EgaTest, DacKeepsOnlyTheSixBitsTheHardwareWired) {
    // The VGA DAC has 6 significant bits per channel; the top two aren't connected.
    Ega ega;
    ega.reset();
    ega.out(0x3C8, 0);
    ega.out(0x3C9, 0xFF); ega.out(0x3C9, 0xC0); ega.out(0x3C9, 0x7F);
    uint8_t r, g, b;
    ega.dac_entry(0, r, g, b);
    EXPECT_EQ(r, 0x3F);
    EXPECT_EQ(g, 0x00);
    EXPECT_EQ(b, 0x3F);
}

TEST(EgaTest, DacReadPathHasItsOwnIndexAndReportsReadModeInTheStateRegister) {
    // Read and write indices are separate. The DAC State register (0x3C7) reports the last addressed side: 3 = read, 0 = write.
    Ega ega;
    ega.reset();
    ega.out(0x3C8, 10);
    ega.out(0x3C9, 60); ega.out(0x3C9, 50); ega.out(0x3C9, 40);
    EXPECT_EQ(ega.in(0x3C7) & 0x03, 0x00);  // last index write was the write register

    ega.out(0x3C7, 10);
    EXPECT_EQ(ega.in(0x3C7) & 0x03, 0x03);
    EXPECT_EQ(ega.in(0x3C9), 60);
    EXPECT_EQ(ega.in(0x3C9), 50);
    EXPECT_EQ(ega.in(0x3C9), 40);
    // Reading advanced only the READ index.
    EXPECT_EQ(ega.in(0x3C8), 11);
}

TEST(EgaTest, PelMaskDefaultsToAllOnesAndRoundTrips) {
    Ega ega;
    ega.reset();
    EXPECT_EQ(ega.in(0x3C6), 0xFF);  // power-on: every pixel bit reaches the DAC
    EXPECT_EQ(ega.dac_mask(), 0xFF);
    ega.out(0x3C6, 0x0F);
    EXPECT_EQ(ega.in(0x3C6), 0x0F);
    EXPECT_EQ(ega.dac_mask(), 0x0F);
}

// --- Chain 4 (Sequencer Memory Mode bit 3), VGA mode 13h's addressing ----

// Mode 13h setup: Chain 4 on, odd/even off, all planes writable.
void SetupChain4(Ega &ega) {
    ega.out(0x3C4, 0x02); ega.out(0x3C5, 0x0F);  // Map Mask: all 4 planes
    ega.out(0x3C4, 0x04); ega.out(0x3C5, 0x0E);  // Memory Mode: Chain 4, odd/even disabled
    ega.out(0x3CE, 0x01); ega.out(0x3CF, 0x00);  // Enable Set/Reset: none
    ega.out(0x3CE, 0x05); ega.out(0x3CF, 0x40);  // GC Mode: write mode 0, 256-color shift
    ega.out(0x3CE, 0x06); ega.out(0x3CF, 0x05);  // Misc: graphics, 64K @ A0000
    ega.out(0x3CE, 0x08); ega.out(0x3CF, 0xFF);  // Bit Mask: all bits pass through
}

// The CPU may touch the aperture as plain linear bytes only while every planar stage is
// pass-through (mode 13h). Each guard has its own assertion and must also move mapping_epoch().
TEST(EgaTest, LinearPageOnlyWhileEveryPlanarStageIsPassThrough) {
    Ega ega;
    ega.reset();
    // A freshly reset (text-mode) card is planar: no pointer.
    EXPECT_EQ(ega.linear_page(0xA0000, true), nullptr);

    SetupChain4(ega);
    uint8_t *p = ega.linear_page(0xA0000, true);
    ASSERT_NE(p, nullptr) << "mode 13h's configuration is linear";

    // A pointer write and the decode path must see the same byte.
    p[0x123] = 0x5A;
    EXPECT_EQ(ega.mem_read(0xA0123), 0x5A);
    ega.mem_write(0xA0456, 0xC3);
    EXPECT_EQ(p[0x456], 0xC3);

    struct Guard { const char *what; uint16_t port_idx, port_dat; uint8_t index, value; };
    const Guard guards[] = {
        {"narrowed Map Mask drops planes",      0x3C4, 0x3C5, 0x02, 0x03},
        {"write mode 2 reinterprets the byte",  0x3CE, 0x3CF, 0x05, 0x42},
        {"a Bit Mask masks bits",               0x3CE, 0x3CF, 0x08, 0x0F},
        {"Enable Set/Reset substitutes it",     0x3CE, 0x3CF, 0x01, 0x0F},
        {"a rotate count alters it",            0x3CE, 0x3CF, 0x03, 0x03},
    };
    for (const Guard &g : guards) {
        Ega e;
        e.reset();
        SetupChain4(e);
        ASSERT_NE(e.linear_page(0xA0000, true), nullptr) << g.what;
        const uint32_t before = e.mapping_epoch();
        e.out(g.port_idx, g.index); e.out(g.port_dat, g.value);
        EXPECT_EQ(e.linear_page(0xA0000, true), nullptr) << g.what;
        EXPECT_NE(e.mapping_epoch(), before) << g.what << ": epoch must move";
    }

    // Read mode 1 returns a colour-compare result, not stored data.
    Ega r;
    r.reset();
    SetupChain4(r);
    ASSERT_NE(r.linear_page(0xA0000, false), nullptr);
    r.out(0x3CE, 0x05); r.out(0x3CF, 0x48);
    EXPECT_EQ(r.linear_page(0xA0000, false), nullptr);

    // Without Chain 4 the aperture is planar.
    Ega planar;
    planar.reset();
    SetupChain4(planar);
    planar.out(0x3C4, 0x04); planar.out(0x3C5, 0x06);  // Chain 4 off
    EXPECT_EQ(planar.linear_page(0xA0000, true), nullptr);
}

// A page must lie wholly inside the active window.
TEST(EgaTest, LinearPageRefusesPagesOutsideTheActiveWindow) {
    Ega ega;
    ega.reset();
    SetupChain4(ega);   // 64K @ A0000
    EXPECT_NE(ega.linear_page(0xAF000, true), nullptr) << "last page of the window";
    EXPECT_EQ(ega.linear_page(0xB0000, true), nullptr) << "past the 64K window";
    EXPECT_EQ(ega.linear_page(0xB8000, true), nullptr);
}

TEST(EgaTest, Chain4SendsFourConsecutiveCpuBytesToFourDifferentPlanes) {
    // Address bits 0-1 become the plane select, which with byte-interleaved storage collapses to vram[offset].
    Ega ega;
    ega.reset();
    SetupChain4(ega);
    for (int i = 0; i < 8; ++i) ega.mem_write(0xA0000 + uint32_t(i), uint8_t(0x10 + i));

    for (int i = 0; i < 8; ++i) {
        int plane_off = i >> 2, plane = i & 3;
        EXPECT_EQ(ega.vram[(std::size_t(plane_off) << 2) + std::size_t(plane)], 0x10 + i)
            << "byte " << i;
        EXPECT_EQ(ega.mem_read(0xA0000 + uint32_t(i)), 0x10 + i) << "read back byte " << i;
    }
}

TEST(EgaTest, Chain4StillHonorsTheSequencerMapMask) {
    // Chain 4 doesn't bypass the Sequencer plane enables; narrowing Map Mask stops some pixels (mode X tricks).
    Ega ega;
    ega.reset();
    SetupChain4(ega);
    ega.out(0x3C4, 0x02); ega.out(0x3C5, 0x05);  // Map Mask: planes 0 and 2 only
    for (int i = 0; i < 4; ++i) ega.mem_write(0xA0000 + uint32_t(i), 0xAA);

    EXPECT_EQ(ega.vram[0], 0xAA);  // plane 0 -- enabled
    EXPECT_EQ(ega.vram[1], 0x00);  // plane 1 -- masked off
    EXPECT_EQ(ega.vram[2], 0xAA);  // plane 2 -- enabled
    EXPECT_EQ(ega.vram[3], 0x00);  // plane 3 -- masked off
}

TEST(EgaTest, Chain4TakesPriorityOverOddEvenChaining) {
    // With Chain 4 and odd/even both set, Chain 4's 2-bit plane select wins.
    Ega ega;
    ega.reset();
    SetupChain4(ega);
    ega.out(0x3C4, 0x04); ega.out(0x3C5, 0x0A);  // Chain 4 set, odd/even ALSO enabled
    ega.mem_write(0xA0002, 0x5A);
    EXPECT_EQ(ega.vram[2], 0x5A);          // chain-4 decode: plane 2, offset 0
    EXPECT_EQ(ega.mem_read(0xA0002), 0x5A);
}

// --- SVGA extension registers (0x1CE/0x1CF) and the VESA path ------------

TEST(EgaTest, SvgaRegistersAreSixteenBitAtOneAddress) {
    // The ROM uses `out dx, ax` / `in ax, dx`, so a non-zero high byte must survive.
    // See Ega::owns_port16 and chipset.cpp io_out16.
    Ega ega;
    ega.reset();
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegXres);
    ega.out16(Ega::kVbeDataPort, 1024);
    EXPECT_EQ(ega.in16(Ega::kVbeIndexPort), Ega::kVbeRegXres);
    EXPECT_EQ(ega.in16(Ega::kVbeDataPort), 1024);
    EXPECT_EQ(ega.vbe_reg(Ega::kVbeRegXres), 1024);
}

TEST(EgaTest, SvgaIdRegisterAcceptsOnlyRevisionsThisCardImplements) {
    // The ROM probes by writing a revision and reading it back; an unimplemented one must not stick.
    Ega ega;
    ega.reset();
    EXPECT_EQ(ega.vbe_reg(Ega::kVbeRegId), Ega::kVbeIdLowest);

    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegId);
    ega.out16(Ega::kVbeDataPort, Ega::kVbeIdHighest);
    EXPECT_EQ(ega.in16(Ega::kVbeDataPort), Ega::kVbeIdHighest);

    ega.out16(Ega::kVbeDataPort, 0xB0CF);  // not a revision this card has
    EXPECT_EQ(ega.in16(Ega::kVbeDataPort), Ega::kVbeIdHighest);
}

TEST(EgaTest, SvgaGetCapsReportsCardMaximaNotTheCurrentGeometry) {
    // The ROM sets GETCAPS and re-reads XRES/YRES/BPP before listing VESA modes
    // (vgabios mode_info_check_mode); otherwise the list is empty. See PC486_REVIEW.md §7.
    Ega ega;
    ega.reset();
    auto put = [&](uint16_t reg, uint16_t v) {
        ega.out16(Ega::kVbeIndexPort, reg); ega.out16(Ega::kVbeDataPort, v);
    };
    auto get = [&](uint16_t reg) {
        ega.out16(Ega::kVbeIndexPort, reg); return ega.in16(Ega::kVbeDataPort);
    };
    put(Ega::kVbeRegXres, 320);
    put(Ega::kVbeRegYres, 200);
    put(Ega::kVbeRegBpp, 8);
    EXPECT_EQ(get(Ega::kVbeRegXres), 320);

    put(Ega::kVbeRegEnable, Ega::kVbeGetCaps);
    EXPECT_EQ(get(Ega::kVbeRegXres), Ega::kVbeMaxXres);
    EXPECT_EQ(get(Ega::kVbeRegYres), Ega::kVbeMaxYres);
    EXPECT_EQ(get(Ega::kVbeRegBpp), Ega::kVbeMaxBpp);
    // Other registers are unaffected by the query mode.
    EXPECT_EQ(get(Ega::kVbeRegVideoMemory64K), ega.vram.size() / 65536);

    put(Ega::kVbeRegEnable, 0);
    EXPECT_EQ(get(Ega::kVbeRegXres), 320);  // the programmed value was never lost
}

TEST(EgaTest, SvgaMaximaAndVramMatchTheOneMegabyteCard) {
    // 1024x768x8 = 786,432 bytes is the largest 8bpp frame in 1MB, reported as 16 64KB units (ega.h).
    Ega ega;
    ega.reset();
    EXPECT_EQ(ega.vram.size(), std::size_t(1024 * 1024));
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegEnable);
    ega.out16(Ega::kVbeDataPort, Ega::kVbeGetCaps);
    auto get = [&](uint16_t reg) {
        ega.out16(Ega::kVbeIndexPort, reg); return ega.in16(Ega::kVbeDataPort);
    };
    EXPECT_EQ(get(Ega::kVbeRegXres), 1024);
    EXPECT_EQ(get(Ega::kVbeRegYres), 768);
    EXPECT_EQ(get(Ega::kVbeRegBpp), 8);
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegEnable);
    ega.out16(Ega::kVbeDataPort, 0);
    EXPECT_EQ(get(Ega::kVbeRegVideoMemory64K), 16);
}

TEST(EgaTest, SvgaVideoMemoryRegisterReportsInstalledVramAndIsReadOnly) {
    Ega ega;
    ega.reset();
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegVideoMemory64K);
    EXPECT_EQ(ega.in16(Ega::kVbeDataPort), ega.vram.size() / 65536);  // 1MB -> 16
    ega.out16(Ega::kVbeDataPort, 64);  // software cannot solder on more RAM
    EXPECT_EQ(ega.in16(Ega::kVbeDataPort), ega.vram.size() / 65536);
}

TEST(EgaTest, EnablingAnSvgaModeClearsVramUnlessAskedNotTo) {
    // A mode set clears the previous pixels unless NoClearMem is set.
    Ega ega;
    ega.reset();
    ega.vram[0] = 0x99;
    auto enable = [&](uint16_t bits) {
        ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegBpp); ega.out16(Ega::kVbeDataPort, 8);
        ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegXres); ega.out16(Ega::kVbeDataPort, 320);
        ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegYres); ega.out16(Ega::kVbeDataPort, 200);
        ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegEnable); ega.out16(Ega::kVbeDataPort, bits);
    };
    enable(Ega::kVbeEnabled);
    EXPECT_EQ(ega.vram[0], 0x00);

    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegEnable); ega.out16(Ega::kVbeDataPort, 0);
    ega.vram[0] = 0x99;
    enable(uint16_t(Ega::kVbeEnabled | Ega::kVbeNoClearMem));
    EXPECT_EQ(ega.vram[0], 0x99);
}

TEST(EgaTest, SvgaWindowIsFlatLinearMemorySlidByTheBankRegister) {
    // In an SVGA mode the planar engine is out; the Bank register slides the 64KB aperture
    // over the frame buffer. Map Mask is narrowed to prove it gates nothing.
    Ega ega;
    ega.reset();
    SetupChain4(ega);
    ega.out(0x3C4, 0x02); ega.out(0x3C5, 0x01);  // Map Mask: plane 0 only -- must not matter
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegBpp); ega.out16(Ega::kVbeDataPort, 8);
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegXres); ega.out16(Ega::kVbeDataPort, 640);
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegYres); ega.out16(Ega::kVbeDataPort, 400);
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegEnable); ega.out16(Ega::kVbeDataPort, Ega::kVbeEnabled);

    for (int i = 0; i < 4; ++i) ega.mem_write(0xA0000 + uint32_t(i), uint8_t(0x20 + i));
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(ega.vram[std::size_t(i)], 0x20 + i);
        EXPECT_EQ(ega.mem_read(0xA0000 + uint32_t(i)), 0x20 + i);
    }

    // Bank 2 puts the same aperture over linear offset 2*64K.
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegBank); ega.out16(Ega::kVbeDataPort, 2);
    ega.mem_write(0xA0000, 0x77);
    EXPECT_EQ(ega.vram[2 * Ega::kVbeBankSize], 0x77);
    EXPECT_EQ(ega.mem_read(0xA0000), 0x77);
    EXPECT_EQ(ega.vram[0], 0x20);  // bank 0's byte is untouched

    // Past the card's 1MB the write is rejected and the window stays put, as in Bochs vga.cc.
    uint16_t past_end_bank = uint16_t(ega.vram.size() / Ega::kVbeBankSize + 1);
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegBank); ega.out16(Ega::kVbeDataPort, past_end_bank);
    EXPECT_EQ(ega.vbe_reg(Ega::kVbeRegBank), 2);
    EXPECT_EQ(ega.mem_read(0xA0000), 0x77);
}

TEST(EgaTest, GetCapsOnBankAdvertisesThirtyTwoKGranularity) {
    // dispi_support_bank_granularity_32k reads BANK under GETCAPS and expects bit 0x10 in the high byte.
    Ega ega;
    ega.reset();
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegEnable);
    ega.out16(Ega::kVbeDataPort, Ega::kVbeGetCaps);
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegBank);
    EXPECT_EQ(ega.in16(Ega::kVbeDataPort), uint16_t(Ega::kVbeBankGranularity32K << 8));
}

TEST(EgaTest, ThirtyTwoKBankGranularityMakesFourFZeroFiveLandCorrectly) {
    // With the 32KB Enable bit on, hardware bank N is N*32KB. The firmware's 4F05 writes
    // guest_bank*2, so guest bank 1 -> offset 64KB.
    Ega ega;
    ega.reset();
    SetupChain4(ega);
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegBpp); ega.out16(Ega::kVbeDataPort, 8);
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegXres); ega.out16(Ega::kVbeDataPort, 640);
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegYres); ega.out16(Ega::kVbeDataPort, 480);
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegEnable);
    ega.out16(Ega::kVbeDataPort, uint16_t(Ega::kVbeEnabled | Ega::kVbeBankGranularity32K));
    EXPECT_EQ(ega.vbe_bank_bytes(), 32768u);

    // Firmware-style 4F05 for guest bank 1: shl 1, OR RW flags.
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegBank);
    ega.out16(Ega::kVbeDataPort, uint16_t(0xC000 | (1u << 1)));
    EXPECT_EQ(ega.vbe_reg(Ega::kVbeRegBank), 2);
    ega.mem_write(0xA0000, 0x5A);
    EXPECT_EQ(ega.vram[Ega::kVbeBankSize], 0x5A);  // 2 * 32KB = 64KB
    EXPECT_EQ(ega.vram[2 * Ega::kVbeBankSize], 0x00);  // not the old doubled landing
}

TEST(EgaTest, LeavingAnSvgaModeGivesThePlanarEngineBackItsMemory) {
    // 4F02 back to a legacy mode turns the extension registers off and restores planar decoding.
    Ega ega;
    ega.reset();
    SetupLinearGraphics(ega);
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegBpp); ega.out16(Ega::kVbeDataPort, 8);
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegXres); ega.out16(Ega::kVbeDataPort, 320);
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegYres); ega.out16(Ega::kVbeDataPort, 200);
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegEnable); ega.out16(Ega::kVbeDataPort, Ega::kVbeEnabled);
    ega.mem_write(0xA0000, 0x3C);
    EXPECT_EQ(ega.vram[0], 0x3C);  // flat: one byte, one plane's slot

    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegEnable); ega.out16(Ega::kVbeDataPort, 0);
    ega.mem_write(0xA0000, 0x5C);
    // Planar again: one CPU byte lands in all four planes at once.
    for (int p = 0; p < 4; ++p) EXPECT_EQ(ega.vram[std::size_t(p)], 0x5C) << "plane " << p;
}

TEST(EgaTest, FourBppDispiModeKeepsThePlanarEngineAndSlidesItByTheBankRegister) {
    // Mode 104h (1024x768x4) needs 98,304 bytes/plane, past the 64KB aperture, so Bank slides
    // plane_off as Bochs's ext_offset does (PC486_REVIEW.md §7.5.1, VGABIOS dispi_set_mode).
    Ega ega;
    ega.reset();
    SetupLinearGraphics(ega);
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegBpp); ega.out16(Ega::kVbeDataPort, 4);
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegXres); ega.out16(Ega::kVbeDataPort, 1024);
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegYres); ega.out16(Ega::kVbeDataPort, 768);
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegEnable); ega.out16(Ega::kVbeDataPort, Ega::kVbeEnabled);
    EXPECT_TRUE(ega.vbe_planar_banked());
    EXPECT_FALSE(ega.vbe_mode_active());

    ega.mem_write(0xA0000, 0xA5);
    for (int p = 0; p < 4; ++p) EXPECT_EQ(ega.vram[std::size_t(p)], 0xA5) << "plane " << p;

    // Bank 1: plane_off 65536, interleaved index 262144.
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegBank); ega.out16(Ega::kVbeDataPort, 1);
    ega.mem_write(0xA0000, 0x5A);
    for (int p = 0; p < 4; ++p) {
        EXPECT_EQ(ega.vram[262144 + std::size_t(p)], 0x5A) << "plane " << p;
        EXPECT_EQ(ega.vram[std::size_t(p)], 0xA5) << "bank 0 plane " << p << " untouched";
    }
    EXPECT_EQ(ega.mem_read(0xA0000), 0x5A);

    // A 1MB card has four planar banks at 64KB granularity; a fifth is rejected.
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegBank); ega.out16(Ega::kVbeDataPort, 4);
    EXPECT_EQ(ega.vbe_reg(Ega::kVbeRegBank), 1);
    EXPECT_EQ(ega.mem_read(0xA0000), 0x5A);
}

TEST(EgaTest, BankRegisterIgnoresTheFirmwareRdWrFlagBits) {
    // The VGABIOS 4F05 ORs VBE_DISPI_BANK_RW (bits 15:14) into the bank; only the low bits select.
    Ega ega;
    ega.reset();
    SetupChain4(ega);
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegBpp); ega.out16(Ega::kVbeDataPort, 8);
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegXres); ega.out16(Ega::kVbeDataPort, 640);
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegYres); ega.out16(Ega::kVbeDataPort, 400);
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegEnable); ega.out16(Ega::kVbeDataPort, Ega::kVbeEnabled);

    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegBank);
    ega.out16(Ega::kVbeDataPort, uint16_t(0xC000 | 2));  // RW flags + bank 2
    EXPECT_EQ(ega.vbe_reg(Ega::kVbeRegBank), 2);
    ega.mem_write(0xA0000, 0x77);
    EXPECT_EQ(ega.vram[2 * Ega::kVbeBankSize], 0x77);
}

TEST(EgaTest, OwnsTheDacAndSvgaPortsItImplements) {
    Ega ega;
    ega.reset();
    for (uint16_t p : {uint16_t(0x3C6), uint16_t(0x3C7), uint16_t(0x3C8), uint16_t(0x3C9),
                       Ega::kVbeIndexPort, Ega::kVbeDataPort}) {
        EXPECT_TRUE(ega.owns_port(p)) << "port 0x" << std::hex << p;
    }
    EXPECT_TRUE(Ega::owns_port16(Ega::kVbeIndexPort));
    EXPECT_TRUE(Ega::owns_port16(Ega::kVbeDataPort));
    EXPECT_FALSE(Ega::owns_port16(0x3C4));  // an ordinary 8-bit indexed register pair
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

// Mode 03h (720x400, 28.322 MHz, 9 dots/char, HT 0x5F, VT 0xBF + overflow = 449 lines) is ~70.1 Hz,
// i.e. 66e6/70.1 CPU cycles per frame.
TEST(EgaTest, Mode03hTimingYieldsSeventyHertzFrame) {
    Ega ega;
    ega.reset();
    ProgramCrtcTiming(ega, /*htotal=*/0x5F, /*vtotal=*/0xBF, /*overflow=*/0x05,
                       /*vrs=*/0x9C, /*vre_low4=*/0x0E, /*seq clocking=*/0x00,
                       /*misc=*/0x04);
    uint64_t cursor = 0;
    std::vector<uint64_t> onsets = RetraceOnsets(ega, cursor, 3'000'000);
    ASSERT_GE(onsets.size(), 2u);
    uint64_t period = onsets[1] - onsets[0];
    EXPECT_NEAR(double(period), 66e6 / 70.1, 66e6 / 70.1 * 0.01);
}

// 640x480 (25.175 MHz, 8 dots/char, HT 0x5F, 525 lines) is 59.94 Hz.
TEST(EgaTest, SixForty480TimingYieldsFiftyNinePointNineFourHertzFrame) {
    Ega ega;
    ega.reset();
    ProgramCrtcTiming(ega, /*htotal=*/0x5F, /*vtotal=*/0x0B, /*overflow=*/0x24,
                       /*vrs=*/0xE0, /*vre_low4=*/0x02, /*seq clocking=*/0x01,
                       /*misc=*/0x00);
    uint64_t cursor = 0;
    std::vector<uint64_t> onsets = RetraceOnsets(ega, cursor, 3'000'000);
    ASSERT_GE(onsets.size(), 2u);
    uint64_t period = onsets[1] - onsets[0];
    EXPECT_NEAR(double(period), 66e6 / 59.94, 66e6 / 59.94 * 0.01);
}

// The card runs from its own crystal, so mode 03h takes half the CPU cycles per frame at 33 MHz.
TEST(EgaTest, SetCpuHzHalvesCyclesPerFrameAtHalfTheClock) {
    Ega ega;
    ega.reset();
    ProgramCrtcTiming(ega, /*htotal=*/0x5F, /*vtotal=*/0xBF, /*overflow=*/0x05,
                       /*vrs=*/0x9C, /*vre_low4=*/0x0E, /*seq clocking=*/0x00,
                       /*misc=*/0x04);
    ega.set_cpu_hz(33e6);
    uint64_t cursor = 0;
    std::vector<uint64_t> onsets = RetraceOnsets(ega, cursor, 1'500'000);
    ASSERT_GE(onsets.size(), 2u);
    uint64_t period = onsets[1] - onsets[0];
    EXPECT_NEAR(double(period), 33e6 / 70.1, 33e6 / 70.1 * 0.01);
}

// Retrace runs from CRTC 10h to the low 4 bits of 11h (a 4-bit comparator), not a fixed fraction of the frame.
TEST(EgaTest, RetraceWindowLengthTracksVerticalRetraceEndRegister) {
    Ega ega;
    ega.reset();
    ProgramCrtcTiming(ega, /*htotal=*/0x5F, /*vtotal=*/0xBF, /*overflow=*/0x05,
                       /*vrs=*/0x9C, /*vre_low4=*/0x0E, /*seq clocking=*/0x00,
                       /*misc=*/0x04);
    uint64_t cursor = 0;
    uint64_t narrow_window = MeasureRetraceWindowCycles(ega, cursor, 1'500'000);
    ASSERT_GT(narrow_window, 0u);

    // Same start, a retrace end that wraps to a value further away: a wider pulse.
    ega.out(0x3D4, 0x11); ega.out(0x3D5, 0x08);
    uint64_t wide_window = MeasureRetraceWindowCycles(ega, cursor, 1'500'000);
    ASSERT_GT(wide_window, 0u);

    EXPECT_GT(wide_window, narrow_window * 3);
}

// Mode 13h CRTC: HDE 4Fh (80 of 100 clocks), VDE 8Fh + Overflow 1Fh (400 of 449 lines), VRS 9Ch.
void ProgramMode13hTiming(Ega &ega) {
    ProgramCrtcTiming(ega, /*htotal=*/0x5F, /*vtotal=*/0xBF, /*overflow=*/0x1F,
                       /*vrs=*/0x9C, /*vre_low4=*/0x0E, /*seq clocking=*/0x01,
                       /*misc=*/0x00);
    ega.out(0x3D4, 0x01); ega.out(0x3D5, 0x4F);
    ega.out(0x3D4, 0x12); ega.out(0x3D5, 0x8F);
}

// Input Status 1 bit 0 is high in every horizontal blank, not just vertical.
// Duke Nukem 3D's palette loader waits on it and hangs if it never moves.
TEST(EgaTest, DisplayDisabledBitPulsesOncePerScanline) {
    Ega ega;
    ega.reset();
    ProgramMode13hTiming(ega);
    uint64_t c = 0;
    std::vector<uint64_t> onsets = RetraceOnsets(ega, c, 3'000'000);
    ASSERT_GE(onsets.size(), 2u);
    uint64_t frame = onsets[1] - onsets[0];
    int edges = 0, high = 0, samples = 0;
    bool prev = true;
    for (uint64_t t = c; t < c + frame; t += 20) {
        ega.tick(t);
        bool now = (ega.in(0x3DA) & 0x01) != 0;
        if (now && !prev) ++edges;
        prev = now;
        high += now; ++samples;
    }
    EXPECT_NEAR(edges, 400, 2);
    // 20 of 100 char clocks on 400 lines plus all of the other 49 lines.
    double expected = (400.0 * 0.2 + 49.0) / 449.0;
    EXPECT_NEAR(double(high) / samples, expected, 0.02);
}

// Vertical retrace sits inside vertical blank, so bit 0 is high for every bit 3 pulse.
TEST(EgaTest, DisplayDisabledBitHighThroughoutVerticalRetrace) {
    Ega ega;
    ega.reset();
    ProgramMode13hTiming(ega);
    int retrace_samples = 0;
    for (uint64_t t = 0; t < 2'000'000; t += 50) {
        ega.tick(t);
        uint8_t v = ega.in(0x3DA);
        if (v & 0x08) {
            ++retrace_samples;
            ASSERT_TRUE(v & 0x01) << "at cycle " << t;
        }
    }
    EXPECT_GT(retrace_samples, 0);
}

// An unprogrammed CRTC still toggles bit 0 so a poll can't hang during a mode set.
TEST(EgaTest, DisplayDisabledBitTogglesWithUnprogrammedCrtc) {
    Ega ega;
    ega.reset();
    bool saw_high = false, saw_low = false;
    for (uint64_t t = 0; t < 1'000'000; t += 7) {
        ega.tick(t);
        if (ega.in(0x3DA) & 0x01) saw_high = true; else saw_low = true;
    }
    EXPECT_TRUE(saw_high);
    EXPECT_TRUE(saw_low);
}

// An all-zero CRTC would give a several-hundred-kHz frame rate; it must fall back to ~70 Hz.
TEST(EgaTest, UnprogrammedCrtcFallsBackToSeventyHertzInsteadOfNonsense) {
    Ega ega;
    ega.reset();
    uint64_t cursor = 0;
    std::vector<uint64_t> onsets = RetraceOnsets(ega, cursor, 2'000'000);
    ASSERT_GE(onsets.size(), 2u);
    uint64_t period = onsets[1] - onsets[0];
    EXPECT_NEAR(double(period), 66e6 / 70.0, 66e6 / 70.0 * 0.01);
}

// Clocking Mode bit 3 halves the dot clock. Mode 0Dh (HT 0x2D, 8 dots/char, 449 lines) still lands near 70 Hz.
TEST(EgaTest, ClockingModeDivideByTwoBitIsHonoured) {
    Ega ega;
    ega.reset();
    ProgramCrtcTiming(ega, /*htotal=*/0x2D, /*vtotal=*/0xBF, /*overflow=*/0x05,
                       /*vrs=*/0x9C, /*vre_low4=*/0x0E, /*seq clocking=*/0x09,
                       /*misc=*/0x00);
    uint64_t cursor = 0;
    std::vector<uint64_t> onsets = RetraceOnsets(ega, cursor, 3'000'000);
    ASSERT_GE(onsets.size(), 2u);
    uint64_t period = onsets[1] - onsets[0];
    EXPECT_NEAR(double(period), 66e6 / 70.1, 66e6 / 70.1 * 0.01);
}

}  // namespace
