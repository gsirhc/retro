// Ties the CPU, bus, VIA, ACIA, and ROM together into the whole board --
// the cg-oac-6502 analog of i8080::Cpu's role in a full Altair Machine.
//
// Clocking: X1 is a canned 1 MHz oscillator driving Phi2 directly (see
// bus.h) -- real 1 MHz, no turbo, per retro/CLAUDE.md. run_cycles() is
// meant to be called with a budget derived from wall-clock time at
// 1,000,000 Hz, the same shape as the Altair's wasm_machine.cpp
// runCycles().
//
// Reset supervisor: DS1813 (U9) holds reset for its documented power-on
// delay (Dallas/Maxim DS1813 datasheet: ~150-200ms, modeled here as a
// cycle count at 1MHz) before the CPU's first fetch -- SW1 and J4 both
// just short the same open-drain RST node, so a manual reset re-arms the
// same hold.
//
// LED wiring, confirmed from pcb6502full.net: despite the netlist's
// confusingly-similar-looking net names (RNPA0/1/2 vs. VIA's own PA0/1/2),
// these are two entirely separate, unconnected nets -- an early read of
// this schematic mistook them for the same node and concluded the LEDs
// were VIA-driven; they are not. All three LEDs are simple, always-passive
// circuits with no CPU/VIA involvement whatsoever:
//   D1 "Power": +5V -> LED -> R8 -> GND. Genuinely just a power indicator,
//   lit whenever the board has power. (VIA PA0 itself only reaches J3's
//   header and J8's RS232-CTS jumper -- unrelated to any LED.)
//   D4 "Rx" / D7 "Tx": sit directly across the RS232-*level* RX/TX lines
//   (pre-MAX232 for RX, post-MAX232 for TX) through a resistor to GND --
//   they light on real line activity, independent of the VIA entirely.

#ifndef CG_OAC_6502_MACHINE_H
#define CG_OAC_6502_MACHINE_H

#include <cstdint>
#include <functional>

#include "bus.h"
#include "cpu65c02.h"
#include "hd44780.h"

namespace machine {

constexpr int kClockHz = 1'000'000;                 // X1, real -- never sped up
constexpr int kResetHoldCycles = kClockHz * 175 / 1000;   // DS1813, ~175ms mid-range of datasheet's 150-200ms

class Machine {
public:
    cpu65c02::Cpu cpu;
    bus::Bus bus;

    // Optional J3 accessory (see hd44780.h) -- attached by default, since
    // the stock ROM's reset_via_irq never gets past its LCD busy-poll
    // without one (confirmed by running the real ROM; see the review doc).
    // set_lcd_attached(false) reproduces the genuine bare-FullBoard hang.
    hd44780::Lcd lcd;
    void set_lcd_attached(bool attached);
    bool lcd_attached() const { return lcd_attached_; }

    // Live line state for the front-panel LED UI -- see the wiring note
    // above. D1 is simply "board powered", asserted once at construction.
    // D4/D7 pulse for each received/transmitted byte: a real diode sitting
    // directly on the RS232 line would flicker per bit transition, not per
    // byte, but per-byte is what's observable without modeling literal
    // line voltage -- a labelled simplification, not a claim of exactness.
    std::function<void(bool)> on_power_led;
    std::function<void(bool)> on_rx_led;
    std::function<void(bool)> on_tx_led;

    // Terminal <-> ACIA bridge. Host calls type_char() as the user types;
    // Machine wires acia.on_tx to relay bytes out to the host's terminal.
    std::function<void(uint8_t)> on_serial_out;
    void type_char(uint8_t c) {
        bus.acia.rx_push(c);
        if (on_rx_led) on_rx_led(true);
    }

    Machine();

    void power_on_reset();     // starts the DS1813 hold; press_reset() re-arms it mid-run too
    void press_reset() { power_on_reset(); }

    // Runs whole instructions until at least `cycles` have elapsed (may
    // slightly overshoot by the last instruction's length, same contract
    // as the Altair's Machine::runCycles()).
    void run_cycles(int cycles);

    // Wall-clock-derived, monotonic across resets -- unlike cpu.cycles,
    // which cpu65c02::Cpu::reset() zeroes (a real 65C02 doesn't remember
    // T-states across a reset either; that's correct for the CPU's own
    // bookkeeping). Machine tracks its own so a reset landing mid-budget
    // inside run_cycles() can't make its target regress -- see machine.cpp.
    uint64_t cycles() const { return total_cycles_; }

private:
    uint64_t total_cycles_ = 0;
    int reset_cycles_left_ = 0;
    bool lcd_attached_ = true;
    bool prev_e_ = false, prev_rs_ = false, prev_rw_ = false;

    void step_one(int budget);
};

} // namespace machine

#endif // CG_OAC_6502_MACHINE_H
