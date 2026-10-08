// Intel 8042 keyboard controller, AT wiring. Ports 0x60 (data) and 0x64
// (status read / command write). IBM 5170 Technical Reference, "Keyboard System".
//
//   - Output Port bit 1 gates A20. The port comes out of reset high, so A20
//     starts open; BIOS closes it via command 0xD1. chipset.h reads a20_enabled().
//   - Output Port bit 0 driven low, or command 0xFE, pulses CPU RESET (the 286's
//     only way back to real mode). reset_requested() surfaces it to Machine.
//
// Covered commands: self-test, interface test, read/write command byte,
// enable/disable keyboard, read/write output port, pulse line 0. LED and
// typematic state are not modeled; other keyboard bytes are just ACKed.
#ifndef IBMPCAT_I8042_H
#define IBMPCAT_I8042_H

#include <cstdint>

namespace ibmpcat {

class I8042 {
public:
    void reset();

    bool owns(uint16_t port) const { return port == 0x60 || port == 0x64; }
    uint8_t in(uint16_t port) const;
    void out(uint16_t port, uint8_t v);

    bool irq1_pending() const { return irq1_pending_; }
    void clear_irq1() { irq1_pending_ = false; }

    bool a20_enabled() const { return (output_port_ & 0x02) != 0; }
    void set_a20(bool on) { output_port_ = uint8_t(on ? (output_port_ | 0x02) : (output_port_ & ~0x02)); }

    bool reset_requested() const { return reset_requested_; }
    void clear_reset_request() { reset_requested_ = false; }

    // Delivers one Set 1 scan code straight to port 0x60. No-op while the
    // keyboard is disabled (0xAD). A real AT keyboard is Set 2 and the 8042
    // translates to Set 1, so callers supply Set 1 codes directly.
    void inject_scancode(uint8_t code);

private:
    enum class NextWrite { kNone, kCommandByte, kOutputPort };
    NextWrite next_write_ = NextWrite::kNone;

    mutable uint8_t output_buf_ = 0;
    mutable bool output_full_ = false;
    uint8_t command_byte_ = 0x00;
    uint8_t output_port_ = 0xFF;
    bool kbd_enabled_ = true;
    bool system_flag_ = false;
    bool last_was_command_ = false;
    bool reset_requested_ = false;
    mutable bool irq1_pending_ = false;
    // Keyboard RESET (0xFF) is ACKed (0xFA), then a separate 0xAA self-test byte
    // follows. This flags that the next data-port read queues it.
    mutable bool pending_bat_after_ack_ = false;

    void push_output(uint8_t v) { output_buf_ = v; output_full_ = true; }
};

}  // namespace ibmpcat

#endif  // IBMPCAT_I8042_H
