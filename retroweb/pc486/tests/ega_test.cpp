// GoogleTest suite for the EGA device: the real planar-memory read/write
// engine (latch, Set/Reset, Data Rotate/ALU, all 4 write modes, both read
// modes, Map Mask and odd/even plane gating, the legacy-window Memory
// Mapping select), CRTC cursor/start-address register programming, the
// Attribute Controller's address/data flip-flop and its reset-on-reading-
// 0x3DA quirk, and the Input Status 1 retrace toggle; plus (Milestone 3)
// the VGA parts Milestone 1 stopped short of -- Chain 4 addressing, the
// 256-entry DAC and its PEL mask, and the card's SVGA extension registers
// that carry the VESA BIOS Extensions. See PC486_REVIEW.md §7.

#include <gtest/gtest.h>

#include <cstddef>
#include <ios>

#include "ega.h"

namespace {

using pc486::Ega;

// Real hardware requires a BIOS/driver mode-set to program these registers
// before video memory behaves predictably -- a freshly reset card has no
// planes enabled and an all-zero Bit Mask, so even a trivial byte write is
// a no-op until something programs it, exactly as on genuine hardware.
// These helpers reproduce the IBM EGA/VGA standard register values for the
// two mode families this suite exercises, matching what every compatible
// BIOS (including this machine's Bochs vgabios) programs for them.

// Standard mode 3 (80x25 16-color text): odd/even chaining OFF-the-CPU's-
// mind (Sequencer Memory Mode enables it), Map Mask enables planes 0+1
// (character/attribute), Graphics Controller Mode enables odd/even on the
// read side too, Miscellaneous selects the 32K color-text window @ B8000,
// Bit Mask passes every CPU bit through untouched.
void SetupTextMode80x25(Ega &ega) {
    ega.out(0x3C4, 0x02); ega.out(0x3C5, 0x03);  // Sequencer Map Mask: planes 0+1
    ega.out(0x3C4, 0x04); ega.out(0x3C5, 0x02);  // Sequencer Memory Mode: odd/even enabled
    ega.out(0x3CE, 0x05); ega.out(0x3CF, 0x10);  // Graphics Mode: odd/even enabled, write mode 0
    ega.out(0x3CE, 0x06); ega.out(0x3CF, 0x0E);  // Misc: alpha mode, chain o/e, B8000 32K window
    ega.out(0x3CE, 0x08); ega.out(0x3CF, 0xFF);  // Bit Mask: all bits pass through
}

// Standard 16-color graphics mode shape (e.g. mode 0x10, 640x350x16): all 4
// planes enabled, odd/even chaining OFF (linear addressing), 64K@A0000,
// write mode 0, Set/Reset disabled so the CPU byte passes straight through,
// Bit Mask all-pass.
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
    // Real EGA/VGA hardware decodes only ONE of the three legacy windows
    // (64K@A0000, 32K@B0000 mono, 32K@B8000 color) at a time, per the
    // Graphics Controller's Memory Mapping field -- not all three at once.
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
    // Switching windows didn't touch A0000's storage, but it's no longer
    // decoded from this address either -- independent, not aliased.
    EXPECT_EQ(ega.mem_read(0xA0000), 0xFF);
}

TEST(EgaTest, WriteMode0AppliesDataRotateAluFunction) {
    Ega ega;
    ega.reset();
    SetupLinearGraphics(ega);
    ega.mem_write(0xA0000, 0xF0);  // seed all 4 planes with 0xF0

    // Data Rotate: rotate count 0, function 2 = OR the CPU byte with the
    // latch (which a preceding read loads from the current VRAM content).
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
    // Plane 0 = 0xFF (all set), plane 1 = 0x00 (all clear), via two mode-2
    // writes (mode 2 already exercised above, reused here as the setup
    // mechanism -- one CPU bit per plane).
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
    // The BIOS's character-generator/font-load routine reaches plane 2 the
    // same way text mode splits character/attribute across planes 0/1:
    // odd/even chaining stays on, Map Mask just points at a different bit
    // -- but it does so through the 64K@A0000 graphics-style window (the
    // character generator RAM isn't visible through the B8000 text window),
    // a real, documented EGA/VGA BIOS technique. An even CPU address
    // reaches plane 2; an odd address would reach plane 3 (unused here).
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
    // Plane 0/1 (text mode's normal char/attr planes) are untouched by a
    // write that Map Mask routed to plane 2 only.
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

// --- VGA DAC (ports 0x3C6-0x3C9) -----------------------------------------

TEST(EgaTest, DacWritesAutoAdvanceThroughRgbAndOnToTheNextEntry) {
    // Real hardware: one index write to the PEL Address Write register,
    // then every third PEL Data write rolls on to the next color register
    // -- which is how period code loads a whole 256-entry palette with a
    // single OUT and 768 more.
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
    // The write index has genuinely moved on, not wrapped back.
    EXPECT_EQ(ega.in(0x3C8), 7);
}

TEST(EgaTest, DacKeepsOnlyTheSixBitsTheHardwareWired) {
    // A VGA DAC has 6 significant bits per channel; the top two simply
    // aren't connected, which is why 8-bit-minded code gets a washed-out
    // picture on real hardware rather than an error.
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
    // Read and write indices are separate registers on real hardware, so a
    // driver reading one entry part-way through writing another corrupts
    // neither. The DAC State register (0x3C7 read) reports which side was
    // addressed last: 3 = read mode, 0 = write mode.
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
    // Reading three bytes advanced only the READ index; the write index is
    // still where the writes above left it.
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

// The Sequencer/Graphics-Controller setup mode 13h uses: Chain 4 on,
// odd/even off, all planes writable, straight CPU-byte writes.
void SetupChain4(Ega &ega) {
    ega.out(0x3C4, 0x02); ega.out(0x3C5, 0x0F);  // Map Mask: all 4 planes
    ega.out(0x3C4, 0x04); ega.out(0x3C5, 0x0E);  // Memory Mode: Chain 4, odd/even disabled
    ega.out(0x3CE, 0x01); ega.out(0x3CF, 0x00);  // Enable Set/Reset: none
    ega.out(0x3CE, 0x05); ega.out(0x3CF, 0x40);  // GC Mode: write mode 0, 256-color shift
    ega.out(0x3CE, 0x06); ega.out(0x3CF, 0x05);  // Misc: graphics, 64K @ A0000
    ega.out(0x3CE, 0x08); ega.out(0x3CF, 0xFF);  // Bit Mask: all bits pass through
}

TEST(EgaTest, Chain4SendsFourConsecutiveCpuBytesToFourDifferentPlanes) {
    // Real hardware: address bits 0-1 become the plane select and drop out
    // of the per-plane offset. With this file's byte-interleaved plane
    // storage that collapses to vram[offset] exactly -- which is *why* mode
    // 13h looks linear to software.
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
    // Chain 4 changes which plane an address reaches; it does not bypass
    // the Sequencer's plane-enable wires. A mode-13h driver that narrows
    // Map Mask genuinely stops some pixels landing (this is exactly how
    // "mode X"-style planar tricks are built on top of the same silicon).
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
    // Both bits set is a nonsense combination software can still program;
    // the real part resolves it in favour of Chain 4's 2-bit plane select,
    // not odd/even's 1-bit one.
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
    // The card's ROM drives these with `out dx, ax` / `in ax, dx`, so a
    // value with a non-zero high byte has to survive -- splitting the
    // access into two byte cycles would write the high half into the data
    // port. See Ega::owns_port16 and chipset.cpp's io_out16.
    Ega ega;
    ega.reset();
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegXres);
    ega.out16(Ega::kVbeDataPort, 1024);
    EXPECT_EQ(ega.in16(Ega::kVbeIndexPort), Ega::kVbeRegXres);
    EXPECT_EQ(ega.in16(Ega::kVbeDataPort), 1024);
    EXPECT_EQ(ega.vbe_reg(Ega::kVbeRegXres), 1024);
}

TEST(EgaTest, SvgaIdRegisterAcceptsOnlyRevisionsThisCardImplements) {
    // The ROM probes by writing a revision number and reading it back; a
    // revision the card does not implement must NOT stick, or the probe
    // wrongly concludes the card speaks it.
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
    // Real, load-bearing behavior: the card's own ROM sets the GETCAPS bit
    // and re-reads XRES/YRES/BPP to discover what the board can do before
    // it will list a VESA mode (vgabios mode_info_check_mode). Without
    // this, every mode compares against the current geometry -- zero on a
    // freshly-reset card -- and the VBE mode list comes back empty. See
    // PC486_REVIEW.md §7.
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

TEST(EgaTest, SvgaVideoMemoryRegisterReportsInstalledVramAndIsReadOnly) {
    Ega ega;
    ega.reset();
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegVideoMemory64K);
    EXPECT_EQ(ega.in16(Ega::kVbeDataPort), ega.vram.size() / 65536);  // 256KB -> 4
    ega.out16(Ega::kVbeDataPort, 64);  // software cannot solder on more RAM
    EXPECT_EQ(ega.in16(Ega::kVbeDataPort), ega.vram.size() / 65536);
}

TEST(EgaTest, EnablingAnSvgaModeClearsVramUnlessAskedNotTo) {
    // A mode set must not leave the previous mode's pixels on screen; the
    // NoClearMem bit is the documented way for software to keep them (a
    // mode switch that preserves an already-drawn frame).
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
    // In an SVGA mode the planar engine is out of the picture entirely --
    // no latches, no Map Mask, no Graphics Controller ALU -- and the 64KB
    // aperture at 0xA0000 is a window the Bank register slides over the
    // whole frame buffer. Map Mask is deliberately narrowed here to prove
    // it no longer gates anything.
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

    // Past the end of the card's real 256KB there is nothing to answer.
    ega.out16(Ega::kVbeIndexPort, Ega::kVbeRegBank); ega.out16(Ega::kVbeDataPort, 9);
    EXPECT_EQ(ega.mem_read(0xA0000), 0xFF);
}

TEST(EgaTest, LeavingAnSvgaModeGivesThePlanarEngineBackItsMemory) {
    // 4F02 back to a legacy VGA mode turns the extension registers off, and
    // the 0xA0000 window has to return to planar decoding -- otherwise
    // every subsequent text/EGA mode reads the wrong bytes.
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

}  // namespace
