// See machine.h for the citations and wiring notes.

#include "machine.h"

#include <algorithm>

namespace machine {

Machine::Machine() : cpu(cpu65c02::Bus{}) {
    wire();
    power_on_reset();
    if (on_power_led) on_power_led(true);   // D1: hardwired to +5V, no software involvement -- see header
}

Machine::Machine(Machine&& other) noexcept
    : cpu(std::move(other.cpu)), bus(std::move(other.bus)), lcd(std::move(other.lcd)),
      on_power_led(std::move(other.on_power_led)), on_rx_led(std::move(other.on_rx_led)),
      on_tx_led(std::move(other.on_tx_led)), on_serial_out(std::move(other.on_serial_out)),
      total_cycles_(other.total_cycles_), reset_cycles_left_(other.reset_cycles_left_),
      lcd_attached_(other.lcd_attached_), prev_e_(other.prev_e_), prev_rs_(other.prev_rs_), prev_rw_(other.prev_rw_) {
    wire();   // cpu/bus register + memory state moved above; this only re-points the `this`-capturing callbacks
}

Machine& Machine::operator=(Machine&& other) noexcept {
    if (this == &other) return *this;
    cpu = std::move(other.cpu);
    bus = std::move(other.bus);
    lcd = std::move(other.lcd);
    on_power_led = std::move(other.on_power_led);
    on_rx_led = std::move(other.on_rx_led);
    on_tx_led = std::move(other.on_tx_led);
    on_serial_out = std::move(other.on_serial_out);
    total_cycles_ = other.total_cycles_;
    reset_cycles_left_ = other.reset_cycles_left_;
    lcd_attached_ = other.lcd_attached_;
    prev_e_ = other.prev_e_; prev_rs_ = other.prev_rs_; prev_rw_ = other.prev_rw_;
    wire();
    return *this;
}

void Machine::wire() {
    cpu65c02::Bus cb;
    cb.read = [this](uint16_t addr) { return bus.read(addr); };
    cb.write = [this](uint16_t addr, uint8_t v) { bus.write(addr, v); };
    cpu.rebind_bus(cb);   // leaves a/x/y/sp/pc/p/cycles alone -- only the callbacks move

    bus.acia.on_tx = [this](uint8_t byte) {
        if (on_serial_out) on_serial_out(byte);
        if (on_tx_led) on_tx_led(true);
    };

    // J3 LCD accessory (see hd44780.h): PA7/PA6/PA5 = E/RW/RS, PB0-7 =
    // data. via.s strobes E high-then-low after setting up RS/RW and (for
    // writes) PORTB; we act on the rising edge, sampling PB at that
    // instant via peek_pb().
    bus.via.on_pa_change = [this](uint8_t pa) {
        bool e = (pa & 0x80) != 0, rw = (pa & 0x40) != 0, rs = (pa & 0x20) != 0;
        if (lcd_attached_ && e && !prev_e_) lcd.strobe(rs, rw, bus.via.peek_pb());
        prev_e_ = e; prev_rs_ = rs; prev_rw_ = rw;
    };
    bus.via.read_pb = [this]() -> uint8_t {
        // Busy flag (bit7) polled while Port B is an input: 0 = ready.
        // Detached, nothing pulls the line -- floating input reads as the
        // conventional all-high default used throughout this core (see
        // via65c22.h's input-latching note), which is why detaching hangs
        // reset_via_irq exactly like a genuinely bare board.
        return lcd_attached_ ? uint8_t(0x00) : uint8_t(0xFF);
    };
}

void Machine::set_lcd_attached(bool attached) {
    lcd_attached_ = attached;
    if (attached) lcd.reset();
}

void Machine::power_on_reset() {
    // DS1813 holds RST asserted for its documented power-on delay; the CPU
    // doesn't fetch its first instruction (or see a valid reset vector
    // read) until the hold releases. Modeled by delaying cpu.reset() itself
    // rather than the more usual "reset now, then idle" -- so a probe of
    // cpu.pc during the hold reads 0, matching a real 65C02 held in RESET.
    bus.reset();
    cpu.pc = 0;
    cpu.waiting = cpu.stopped = false;
    reset_cycles_left_ = kResetHoldCycles;
}

void Machine::step_one(int budget) {
    if (reset_cycles_left_ > 0) {
        // Only consume up to this call's remaining budget, not the whole
        // hold at once -- the ~175ms DS1813 delay must actually take that
        // long in wall-clock terms across successive run_cycles() calls
        // (a host pacing at ~60fps calls run_cycles() with a ~16.7ms
        // budget each frame; bursting all 175,000 cycles into the first
        // frame would make the hold invisible instead of a real wait).
        int n = std::min(reset_cycles_left_, budget);
        reset_cycles_left_ -= n;
        bus.tick(n);
        total_cycles_ += uint64_t(n);
        if (reset_cycles_left_ == 0) cpu.reset();   // now safe to read the real reset vector -- zeroes cpu.cycles, not total_cycles_
        return;
    }
    if (bus.nmi_pending()) { bus.ack_nmi(); cpu.nmi(); }
    cpu.irq_line = bus.irq_line();
    int c = cpu.step();
    bus.tick(c);
    total_cycles_ += uint64_t(c);
}

void Machine::run_cycles(int cycles) {
    uint64_t target = total_cycles_ + uint64_t(cycles);
    while (total_cycles_ < target) step_one(int(target - total_cycles_));
}

} // namespace machine
