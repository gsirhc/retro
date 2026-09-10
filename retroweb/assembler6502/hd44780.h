// Optional HD44780 16x2 character LCD accessory, wired to the board's J3
// VIA breakout header exactly as rom/via.s already drives one: E/RW/RS on
// VIA PA7/PA6/PA5, 8-bit data on PB0-7 (via.s: "E = %10000000, RW =
// %01000000, RS = %00100000"). Cite: HD44780 datasheet
// (https://eater.net/datasheets/HD44780.pdf -- the same citation via.s
// itself uses).
//
// FullBoard ships with no LCD -- J3 is a bare header -- but rom/bios.s's
// RESET unconditionally calls reset_via_irq, which busy-polls an LCD
// status bit on PB7 before doing anything else, so the shipped ROM never
// gets past reset on a genuinely bare board. Confirmed by running the real
// ROM (see CGOAC6502_REVIEW.md); the machine defaults to this accessory
// *attached* so the stock ROM actually boots, with a UI control to detach
// it and see the real bare-board hang.
//
// Simplification, labelled: the busy flag always reads "ready" rather than
// modeling the HD44780's real ~37us-1.52ms per-instruction timing -- at
// 1MHz the CPU's own polling overhead already exceeds the short
// instructions in practice, and the firmware never depends on the busy
// flag actually stalling it. Only the subset of instructions this
// firmware issues (clear, home, line-2 DDRAM address, character write
// with auto-increment) are modeled; general CGRAM/DDRAM addressing beyond
// the two rows this display has is not.

#ifndef CG_OAC_6502_HD44780_H
#define CG_OAC_6502_HD44780_H

#include <cstdint>
#include <functional>

namespace hd44780 {

class Lcd {
public:
    char text[2][16];

    // Fires whenever the visible buffer changes, for the UI to redraw.
    std::function<void()> on_change;

    Lcd() { reset(); }
    void reset();

    // Called on the E pin's rising edge (Machine watches VIA PA7 via
    // on_pa_change) with the RS/RW bits and the byte currently on PB.
    void strobe(bool rs, bool rw, uint8_t data);

    // The busy flag polled via PB7 while RW=1, RS=0 -- see simplification
    // note above.
    bool busy() const { return false; }

private:
    int row_ = 0, col_ = 0;
    void write_char(uint8_t c);
    void exec_instruction(uint8_t v);
};

} // namespace hd44780

#endif // CG_OAC_6502_HD44780_H
