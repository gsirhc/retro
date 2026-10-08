// Intel 8042 keyboard controller, AT wiring, and the Enhanced Keyboard behind
// it. Ports 0x60 (data) and 0x64 (status read / command write). IBM 5170
// Technical Reference, "Keyboard System"; IBM Keyboard Technical Reference.
//
//   - Output Port bit 1 gates A20. The port comes out of reset high, so A20
//     starts open; BIOS closes it via command 0xD1. chipset.h reads a20_enabled().
//   - Output Port bit 0 driven low, or command 0xFE, pulses CPU RESET (the 286's
//     only way back to real mode). reset_requested() surfaces it to Machine.
//   - The keyboard holds up to 16 bytes until the controller takes them, one
//     11-bit frame at a time, and runs its own typematic clock.
//
// Controller commands: self-test, interface test, read/write command byte,
// enable/disable keyboard, read input port, read test inputs, read/write
// output port, pulse line 0. LED state is held but not shown.
#ifndef IBMPCAT_I8042_H
#define IBMPCAT_I8042_H

#include <cstdint>
#include <deque>

namespace ibmpcat {

class I8042 {
public:
    void reset();

    bool owns(uint16_t port) const { return port == 0x60 || port == 0x64; }
    uint8_t in(uint16_t port);
    void out(uint16_t port, uint8_t v);

    bool irq1_pending() const { return irq1_pending_; }
    void clear_irq1() { irq1_pending_ = false; }

    bool a20_enabled() const { return (output_port_ & 0x02) != 0; }
    void set_a20(bool on) { output_port_ = uint8_t(on ? (output_port_ | 0x02) : (output_port_ & ~0x02)); }

    bool reset_requested() const { return reset_requested_; }
    void clear_reset_request() { reset_requested_ = false; }

    // One Set 1 byte from the keyboard matrix. A real Enhanced Keyboard is Set 2
    // and the 8042 translates to Set 1, so callers supply Set 1 codes directly.
    // Dropped while the keyboard has scanning off (0xF5).
    void inject_scancode(uint8_t code);

    // Advances the keyboard link and typematic clock against the CPU's absolute
    // cycle count (like Pit8253::tick).
    void tick(uint64_t cpu_cycles, double cpu_hz);

    uint8_t typematic_byte() const { return typematic_; }
    bool typematic_active() const { return tm_active_; }
    uint8_t leds() const { return leds_; }
    bool scanning() const { return scanning_; }
    int buffered() const { return int(fifo_.size()); }

    static constexpr int kBufferSize = 16;
    // 11-bit frame at the midpoint of the 10-16.7 kHz keyboard clock (Chapweske, AT-PS/2 interface)
    static constexpr double kFrameSeconds = 11 * 75e-6;

private:
    enum class NextWrite { kNone, kCommandByte, kOutputPort };
    NextWrite next_write_ = NextWrite::kNone;

    uint8_t output_buf_ = 0;
    bool output_full_ = false;
    uint8_t command_byte_ = 0x00;
    uint8_t output_port_ = 0xFF;
    bool system_flag_ = false;
    bool last_was_command_ = false;
    bool reset_requested_ = false;
    bool irq1_pending_ = false;

    // A response to a command goes out ahead of buffered keys and isn't held off
    // by command-byte bit 4, since the controller is waiting for it.
    struct Byte { uint8_t value; bool response; };
    std::deque<Byte> fifo_;
    bool xfer_active_ = false;
    uint64_t xfer_done_ = 0;
    uint64_t now_ = 0;
    double hz_ = 8000000.0;

    bool scanning_ = true;
    uint8_t leds_ = 0;
    uint8_t last_sent_ = 0xAA;
    uint8_t kbd_arg_cmd_ = 0;

    uint8_t typematic_ = 0x2B;   // 500 ms delay, 10.9 cps: the power-on default
    bool tm_active_ = false;
    uint8_t tm_prefix_ = 0;      // 0xE0 for a grey key, else 0
    uint8_t tm_code_ = 0;
    bool pending_e0_ = false;
    int e1_skip_ = 0;            // Pause's E1 sequences never repeat
    uint64_t tm_next_ = 0;

    bool inhibited() const { return (command_byte_ & 0x10) != 0; }
    uint64_t cycles(double seconds) const { return uint64_t(seconds * hz_); }
    double typematic_delay() const { return 0.25 * double(1 + ((typematic_ >> 5) & 3)); }
    double typematic_period() const {
        // (8 + A) * 2^B * 4.17 ms, A = bits 0-2, B = bits 3-4.
        return double(8 + (typematic_ & 7)) * double(1 << ((typematic_ >> 3) & 3)) * 0.00417;
    }

    void push_ctrl(uint8_t v) { output_buf_ = v; output_full_ = true; }
    void send_key_byte(uint8_t v);
    void respond(uint8_t v);
    void clear_keyboard_buffer();
    void keyboard_defaults();
    void keyboard_write(uint8_t v);
    void start_transfer();
    void typematic_fire();
};

}  // namespace ibmpcat

#endif  // IBMPCAT_I8042_H
