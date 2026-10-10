#include "machine.h"

#include <algorithm>

namespace machine {

Machine::Machine() : cpu(cpu65c02::Bus{}) {
    wire();
    power_on_reset();
    if (on_power_led) on_power_led(true);
}

Machine::Machine(Machine&& other) noexcept
    : cpu(std::move(other.cpu)), bus(std::move(other.bus)), lcd(std::move(other.lcd)),
      on_power_led(std::move(other.on_power_led)), on_rx_led(std::move(other.on_rx_led)),
      on_tx_led(std::move(other.on_tx_led)), on_serial_out(std::move(other.on_serial_out)),
      total_cycles_(other.total_cycles_), reset_cycles_left_(other.reset_cycles_left_),
      lcd_attached_(other.lcd_attached_), prev_e_(other.prev_e_), prev_rs_(other.prev_rs_), prev_rw_(other.prev_rw_) {
    wire();
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
    cpu.rebind_bus(cb);

    bus.acia.on_tx = [this](uint8_t byte) {
        if (on_serial_out) on_serial_out(byte);
        if (on_tx_led) on_tx_led(true);
    };

    // J3 LCD: PA7/PA6/PA5 = E/RW/RS. Act on E's rising edge, sampling PB.
    bus.via.on_pa_change = [this](uint8_t pa) {
        bool e = (pa & 0x80) != 0, rw = (pa & 0x40) != 0, rs = (pa & 0x20) != 0;
        if (lcd_attached_ && e && !prev_e_) lcd.strobe(rs, rw, bus.via.peek_pb());
        prev_e_ = e; prev_rs_ = rs; prev_rw_ = rw;
    };
    bus.via.read_pb = [this]() -> uint8_t {
        // busy flag (bit7): 0 = ready. Detached, the floating input reads high,
        // so reset_via_irq hangs like a bare board.
        return lcd_attached_ ? uint8_t(0x00) : uint8_t(0xFF);
    };
}

void Machine::set_lcd_attached(bool attached) {
    lcd_attached_ = attached;
    if (attached) lcd.reset();
}

void Machine::power_on_reset() {
    // cpu.reset() is delayed until the hold ends, so pc reads 0 during it
    bus.reset();
    cpu.pc = 0;
    cpu.waiting = cpu.stopped = false;
    reset_cycles_left_ = kResetHoldCycles;
}

void Machine::step_one(int budget) {
    if (reset_cycles_left_ > 0) {
        // spend the hold only up to this call's budget so it lasts ~175ms of wall clock
        int n = std::min(reset_cycles_left_, budget);
        reset_cycles_left_ -= n;
        bus.tick(n);
        total_cycles_ += uint64_t(n);
        if (reset_cycles_left_ == 0) cpu.reset();
        return;
    }
    if (bus.nmi_pending()) { bus.ack_nmi(); cpu.nmi(); }
    cpu.irq_line = bus.irq_line();
    int c = cpu.step();
    bus.tick(c);
    total_cycles_ += uint64_t(c);
}

void Machine::run_cycles(int cycles) {
    if (cycles <= 0) return;
    uint64_t target = total_cycles_ + uint64_t(cycles);
    while (total_cycles_ < target) step_one(int(target - total_cycles_));
}

} // namespace machine
