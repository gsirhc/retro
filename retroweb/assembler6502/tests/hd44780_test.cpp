// GoogleTest suite for hd44780::Lcd -- the subset of the HD44780 protocol
// rom/via.s actually drives (see hd44780.h).

#include <gtest/gtest.h>

#include "../hd44780.h"

using namespace hd44780;

namespace {

TEST(Hd44780, WritesCharactersAtCursorAndAutoIncrements) {
    Lcd l;
    l.strobe(/*rs=*/true, /*rw=*/false, 'H');
    l.strobe(true, false, 'I');
    EXPECT_EQ(l.text[0][0], 'H');
    EXPECT_EQ(l.text[0][1], 'I');
}

TEST(Hd44780, ClearDisplayResetsBufferAndCursor) {
    Lcd l;
    l.strobe(true, false, 'X');
    l.strobe(false, false, 0x01);   // clear display
    EXPECT_EQ(l.text[0][0], ' ');
    l.strobe(true, false, 'Y');
    EXPECT_EQ(l.text[0][0], 'Y');   // cursor really did go back to (0,0)
}

TEST(Hd44780, SetDdramLine2MovesCursorToRow1) {
    Lcd l;
    l.strobe(false, false, 0xC0);   // cursorLine2_lcd's instruction
    l.strobe(true, false, 'Z');
    EXPECT_EQ(l.text[1][0], 'Z');
    EXPECT_EQ(l.text[0][0], ' ');
}

TEST(Hd44780, ReturnHomeMovesCursorWithoutClearing) {
    Lcd l;
    l.strobe(true, false, 'A');
    l.strobe(false, false, 0x02);   // cursorLine1_lcd's instruction (return home)
    l.strobe(true, false, 'B');
    EXPECT_EQ(l.text[0][0], 'B');   // overwritten, not cleared first
}

TEST(Hd44780, ReadStrobeNeverChangesTheBuffer) {
    Lcd l;
    l.strobe(true, false, 'Q');
    l.strobe(false, true, 0x00);   // RW=1: a read (busy-flag poll), not a write
    EXPECT_EQ(l.text[0][0], 'Q');
    EXPECT_EQ(l.text[0][1], ' ');
}

} // namespace
