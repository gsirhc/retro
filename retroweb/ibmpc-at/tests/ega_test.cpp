// GoogleTest suite for the EGA text-mode subset: video memory read/write
// at the color text window (0xB8000), CRTC cursor/start-address register
// programming, the Attribute Controller's address/data flip-flop and its
// reset-on-reading-0x3DA quirk, and the Input Status 1 retrace toggle.

#include <gtest/gtest.h>

#include "ega.h"

namespace {

using ibmpcat::Ega;

TEST(EgaTest, TextModeMemoryReadWriteRoundTrip) {
    Ega ega;
    ega.reset();
    ega.mem_write(0xB8000, 'H');
    ega.mem_write(0xB8001, 0x07);  // white-on-black attribute
    EXPECT_EQ(ega.mem_read(0xB8000), 'H');
    EXPECT_EQ(ega.mem_read(0xB8001), 0x07);
}

TEST(EgaTest, GraphicsAndMonoWindowsAreDistinctFromColorTextWindow) {
    Ega ega;
    ega.reset();
    ega.mem_write(0xA0000, 0x11);
    ega.mem_write(0xB0000, 0x22);
    ega.mem_write(0xB8000, 0x33);
    EXPECT_EQ(ega.mem_read(0xA0000), 0x11);
    EXPECT_EQ(ega.mem_read(0xB0000), 0x22);
    EXPECT_EQ(ega.mem_read(0xB8000), 0x33);
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
