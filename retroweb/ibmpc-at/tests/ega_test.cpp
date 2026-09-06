// GoogleTest suite for the EGA device: the real planar-memory read/write
// engine (latch, Set/Reset, Data Rotate/ALU, all 4 write modes, both read
// modes, Map Mask and odd/even plane gating, the legacy-window Memory
// Mapping select), CRTC cursor/start-address register programming, the
// Attribute Controller's address/data flip-flop and its reset-on-reading-
// 0x3DA quirk, and the Input Status 1 retrace toggle.

#include <gtest/gtest.h>

#include "ega.h"

namespace {

using ibmpcat::Ega;

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

}  // namespace
