// Optional HD44780 16x2 LCD on the J3 VIA header, wired as rom/via.s drives
// it: E/RW/RS on VIA PA7/PA6/PA5, data on PB0-7. Datasheet:
// https://eater.net/datasheets/HD44780.pdf
//
// A bare board hangs: bios.s RESET calls reset_via_irq, which busy-polls
// the LCD status on PB7 (CGOAC6502_REVIEW.md). The machine attaches the LCD
// by default, with a UI control to detach it.
//
// Simplification: the busy flag always reads ready instead of the real
// ~37us-1.52ms per instruction. Only the instructions the firmware issues
// are modeled (clear, home, line-2 DDRAM address, character write).

#ifndef CG_OAC_6502_HD44780_H
#define CG_OAC_6502_HD44780_H

#include <cstdint>
#include <functional>

namespace hd44780 {

class Lcd {
public:
    char text[2][16];

    // Fires when the visible buffer changes
    std::function<void()> on_change;

    Lcd() { reset(); }
    void reset();

    // Called on E's rising edge with RS/RW and the byte on PB
    void strobe(bool rs, bool rw, uint8_t data);

    // busy flag polled via PB7 (RW=1, RS=0)
    bool busy() const { return false; }

private:
    int row_ = 0, col_ = 0;
    void write_char(uint8_t c);
    void exec_instruction(uint8_t v);
};

} // namespace hd44780

#endif // CG_OAC_6502_HD44780_H
