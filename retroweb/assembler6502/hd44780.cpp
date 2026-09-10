// HD44780 accessory implementation. See hd44780.h for citation and scope.

#include "hd44780.h"

#include <cstring>

namespace hd44780 {

void Lcd::reset() {
    for (auto &r : text) std::memset(r, ' ', sizeof(r));
    row_ = col_ = 0;
}

void Lcd::write_char(uint8_t c) {
    if (row_ < 2 && col_ < 16) text[row_][col_] = char(c);
    if (col_ < 16) col_++;
    if (on_change) on_change();
}

void Lcd::exec_instruction(uint8_t v) {
    if (v & 0x80) {                          // Set DDRAM Address
        uint8_t addr = v & 0x7F;
        if (addr >= 0x40) { row_ = 1; col_ = addr - 0x40; }
        else { row_ = 0; col_ = addr; }
    } else if (v & 0x02) {                    // Return Home (0x02 or 0x03)
        row_ = col_ = 0;
    } else if (v & 0x01) {                    // Clear Display
        reset();
    }
    // Function set (0x2x-0x3x), display on/off (0x0x), entry mode (0x0x-
    // 0x07) are accepted (this firmware's exact bytes never conflict with
    // the cases above) but don't change the text buffer.
    if (on_change) on_change();
}

void Lcd::strobe(bool rs, bool rw, uint8_t data) {
    if (rw) return;                            // a read strobe -- only the busy flag, no buffer change
    if (rs) write_char(data);
    else exec_instruction(data);
}

} // namespace hd44780
