// The whole board: CPU, bus, VIA, ACIA and ROM.
//
// X1 is a 1 MHz oscillator driving Phi2. run_cycles() takes a budget derived
// from wall-clock time at 1,000,000 Hz.
//
// Reset: the DS1813 (U9) holds reset ~150-200ms (Dallas/Maxim datasheet)
// before the first fetch. SW1 and J4 short the same open-drain RST node, so
// a manual reset re-arms the hold.
//
// LEDs (pcb6502full.net): the RNPA0/1/2 nets are not VIA PA0/1/2, so no LED
// is VIA-driven.
//   D1 "Power": +5V -> LED -> R8 -> GND, lit whenever the board has power.
//   D4 "Rx" / D7 "Tx": across the RS232-level RX (pre-MAX232) and TX
//   (post-MAX232) lines, lighting on line activity.

#ifndef CG_OAC_6502_MACHINE_H
#define CG_OAC_6502_MACHINE_H

#include <cstdint>
#include <functional>

#include "bus.h"
#include "cpu65c02.h"
#include "hd44780.h"

namespace machine {

constexpr int kClockHz = 1'000'000;
constexpr int kResetHoldCycles = kClockHz * 175 / 1000;   // DS1813, ~175ms (datasheet 150-200ms)

class Machine {
public:
    cpu65c02::Cpu cpu;
    bus::Bus bus;

    // Optional J3 LCD, attached by default because reset_via_irq hangs on its
    // busy-poll without one. set_lcd_attached(false) reproduces the hang.
    hd44780::Lcd lcd;
    void set_lcd_attached(bool attached);
    bool lcd_attached() const { return lcd_attached_; }

    // LED state for the front panel. D1 asserts once at construction. D4/D7
    // pulse per byte, not per bit as the real line would.
    std::function<void(bool)> on_power_led;
    std::function<void(bool)> on_rx_led;
    std::function<void(bool)> on_tx_led;

    // Terminal <-> ACIA bridge
    std::function<void(uint8_t)> on_serial_out;
    void type_char(uint8_t c) {
        bus.acia.rx_push(c);
        if (on_rx_led) on_rx_led(true);
    }

    Machine();

    // The `this`-capturing lambdas set up in wire() would dangle after a
    // default move, so move re-wires them. Copy is deleted.
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;
    Machine(Machine&& other) noexcept;
    Machine& operator=(Machine&& other) noexcept;

    void power_on_reset();
    void press_reset() { power_on_reset(); }

    // Runs whole instructions until at least `cycles` have elapsed (may overshoot)
    void run_cycles(int cycles);

    // Monotonic across resets, unlike cpu.cycles, so a mid-budget reset can't regress run_cycles()'s target
    uint64_t cycles() const { return total_cycles_; }

private:
    uint64_t total_cycles_ = 0;
    int reset_cycles_left_ = 0;
    bool lcd_attached_ = true;
    bool prev_e_ = false, prev_rs_ = false, prev_rw_ = false;

    void step_one(int budget);

    // (Re)binds every `this`-capturing callback; called from the constructor and moves
    void wire();
};

} // namespace machine

#endif // CG_OAC_6502_MACHINE_H
