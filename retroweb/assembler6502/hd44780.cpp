#include "hd44780.h"

#include <cstring>

namespace hd44780 {

void Lcd::reset() {
    for (auto &r : text) std::memset(r, ' ', sizeof(text[0]));
    row_ = col_ = 0;
}

void Lcd::write_char(uint8_t c) {
    if (row_ < 2 && col_ < 16) text[row_][col_] = char(c);
    if (col_ < 16) col_++;
    if (on_change) on_change();
}

void Lcd::exec_instruction(uint8_t v) {
    if (v & 0x80) {
        uint8_t addr = v & 0x7F;
        if (addr >= 0x40) { row_ = 1; col_ = addr - 0x40; }
        else { row_ = 0; col_ = addr; }
    } else if (v & 0x02) {                    // Return Home
        row_ = col_ = 0;
    } else if (v & 0x01) {                    // Clear Display
        reset();
    }
    // function set, display on/off and entry mode are accepted, no buffer change
    if (on_change) on_change();
}

void Lcd::strobe(bool rs, bool rw, uint8_t data) {
    if (rw) return;
    if (rs) write_char(data);
    else exec_instruction(data);
}

} // namespace hd44780
