#include "i8042.h"

namespace ibmpcat {

namespace {
// Bit 7 keylock open, bit 6 low = colour display, bit 5 high = no manufacturing
// jumper, bit 4 high = 512KB on the system board, bits 0-3 unconnected and
// pulled up (IBM PC/AT Technical Reference, 8042 input port).
constexpr uint8_t kInputPort = 0xBF;
// Enhanced Keyboard ID, with 83h translated to 41h by the 8042.
constexpr uint8_t kIdTranslated[2] = {0xAB, 0x41};
// Set 2's 00h overrun code, as the 8042 translates it.
constexpr uint8_t kOverrun = 0xFF;
}  // namespace

void I8042::reset() {
    next_write_ = NextWrite::kNone;
    output_buf_ = 0;
    output_full_ = false;
    command_byte_ = 0x00;
    // Port 2 pins come out of reset high (Intel UPI-41A/42 data sheet): RESET released, A20 open.
    output_port_ = 0xFF;
    system_flag_ = false;
    last_was_command_ = false;
    reset_requested_ = false;
    irq1_pending_ = false;
    fifo_.clear();
    xfer_active_ = false;
    scanning_ = true;
    leds_ = 0;
    last_sent_ = 0xAA;
    kbd_arg_cmd_ = 0;
    keyboard_defaults();
    pending_e0_ = false;
    e1_skip_ = 0;
    // The keyboard sends 0xAA unsolicited after its power-on self-test. BIOS POST
    // hangs at the keyboard check without it. The BAT's own duration is not modeled.
    send_key_byte(0xAA);
}

uint8_t I8042::in(uint16_t port) {
    if (port == 0x60) {
        // IRQ1 follows output-buffer-full; reading clears both.
        output_full_ = false;
        irq1_pending_ = false;
        start_transfer();
        return output_buf_;
    }
    // 0x64: status register. Bit 4 is the keylock, 1 = not inhibited.
    uint8_t status = 0x10;
    if (output_full_) status |= 0x01;
    if (system_flag_) status |= 0x04;
    if (last_was_command_) status |= 0x08;
    return status;
}

void I8042::out(uint16_t port, uint8_t v) {
    if (port == 0x64) {
        last_was_command_ = true;
        switch (v) {
            case 0x20: push_ctrl(command_byte_); break;
            case 0x60: next_write_ = NextWrite::kCommandByte; break;
            case 0xAA: push_ctrl(0x55); system_flag_ = true; output_port_ |= 0x02; break;  // leaves A20 open (OS/2 Museum, 8042 commands)
            case 0xAB: push_ctrl(0x00); break;
            case 0xAD: command_byte_ |= 0x10; break;
            case 0xAE: command_byte_ &= ~0x10; start_transfer(); break;
            case 0xC0: push_ctrl(kInputPort); break;
            // T0 is the keyboard clock, T1 data; both idle high, and the 8042 holds the clock low to inhibit
            case 0xE0: push_ctrl(inhibited() ? 0x02 : 0x03); break;
            case 0xD0: push_ctrl(output_port_); break;
            case 0xD1: next_write_ = NextWrite::kOutputPort; break;
            case 0xFE: reset_requested_ = true; break;
            default: break;
        }
        return;
    }
    // 0x60
    last_was_command_ = false;
    switch (next_write_) {
        case NextWrite::kCommandByte:
            command_byte_ = v;
            next_write_ = NextWrite::kNone;
            start_transfer();
            break;
        case NextWrite::kOutputPort:
            output_port_ = v;
            if (!(v & 0x01)) reset_requested_ = true;
            next_write_ = NextWrite::kNone;
            break;
        default:
            keyboard_write(v);
            break;
    }
}

void I8042::tick(uint64_t cpu_cycles, double cpu_hz) {
    now_ = cpu_cycles;
    hz_ = cpu_hz;
    if (xfer_active_ && now_ >= xfer_done_) {
        xfer_active_ = false;
        if (!output_full_ && !fifo_.empty() && (!inhibited() || fifo_.front().response)) {
            output_buf_ = fifo_.front().value;
            fifo_.pop_front();
            output_full_ = true;
            last_sent_ = output_buf_;
            if (command_byte_ & 0x01) irq1_pending_ = true;
        }
    }
    start_transfer();
    if (tm_active_ && now_ >= tm_next_) typematic_fire();
}

void I8042::start_transfer() {
    if (xfer_active_ || output_full_ || fifo_.empty()) return;
    if (inhibited() && !fifo_.front().response) return;
    xfer_active_ = true;
    xfer_done_ = now_ + cycles(kFrameSeconds);
}

void I8042::send_key_byte(uint8_t v) {
    int keys = 0;
    for (const Byte& b : fifo_) keys += b.response ? 0 : 1;
    // The overrun code replaces byte 17 and later bytes are lost (IBM Keyboard Technical Reference)
    if (keys < kBufferSize) fifo_.push_back(Byte{v, false});
    else if (keys == kBufferSize) fifo_.push_back(Byte{kOverrun, false});
    start_transfer();
}

void I8042::respond(uint8_t v) {
    auto it = fifo_.begin();
    while (it != fifo_.end() && it->response) ++it;
    fifo_.insert(it, Byte{v, true});
    // The controller took the line to send, so a key byte in flight starts over
    xfer_active_ = false;
    start_transfer();
}

void I8042::clear_keyboard_buffer() {
    fifo_.clear();
    xfer_active_ = false;
}

void I8042::keyboard_defaults() {
    typematic_ = 0x2B;
    tm_active_ = false;
}

void I8042::keyboard_write(uint8_t v) {
    // A command sent in place of an argument is run as a command
    if (kbd_arg_cmd_ != 0 && !(v & 0x80)) {
        if (kbd_arg_cmd_ == 0xED) leds_ = uint8_t(v & 0x07);
        if (kbd_arg_cmd_ == 0xF3) typematic_ = uint8_t(v & 0x7F);
        kbd_arg_cmd_ = 0;
        respond(0xFA);
        return;
    }
    kbd_arg_cmd_ = 0;
    switch (v) {
        case 0xED: case 0xF0: case 0xF3:
            kbd_arg_cmd_ = v;
            respond(0xFA);
            break;
        case 0xEE:
            respond(0xEE);
            break;
        case 0xF2:
            respond(0xFA);
            respond(kIdTranslated[0]);
            respond(kIdTranslated[1]);
            break;
        case 0xF4:
            clear_keyboard_buffer();
            scanning_ = true;
            respond(0xFA);
            break;
        case 0xF5:
            keyboard_defaults();
            clear_keyboard_buffer();
            scanning_ = false;
            respond(0xFA);
            break;
        case 0xF6:
            keyboard_defaults();
            clear_keyboard_buffer();
            respond(0xFA);
            break;
        case 0xF7: case 0xF8: case 0xF9: case 0xFA: case 0xFB: case 0xFC: case 0xFD:
            respond(0xFA);  // Set 3 key types, no effect in Set 1
            break;
        case 0xFE:
            respond(last_sent_);
            break;
        case 0xFF:
            keyboard_defaults();
            clear_keyboard_buffer();
            scanning_ = true;
            leds_ = 0;
            respond(0xFA);
            respond(0xAA);
            break;
        default:
            respond(0xFE);
            break;
    }
}

void I8042::inject_scancode(uint8_t code) {
    if (scanning_) send_key_byte(code);
    if (e1_skip_ > 0) { --e1_skip_; return; }
    if (code == 0xE1) { e1_skip_ = 2; return; }
    if (code == 0xE0) { pending_e0_ = true; return; }
    uint8_t prefix = pending_e0_ ? 0xE0 : 0x00;
    pending_e0_ = false;
    uint8_t base = uint8_t(code & 0x7F);
    if (!(code & 0x80)) {
        tm_active_ = true;
        tm_prefix_ = prefix;
        tm_code_ = base;
        tm_next_ = now_ + cycles(typematic_delay());
    } else if (tm_active_ && base == tm_code_) {
        // Matched on the code alone so a break with a detached E0 still ends the repeat
        tm_active_ = false;
    }
}

void I8042::typematic_fire() {
    // A held key stores only its first make while the keyboard is inhibited
    // (IBM Keyboard Technical Reference)
    if (scanning_ && !inhibited() && !pending_e0_ && e1_skip_ == 0) {
        if (tm_prefix_) send_key_byte(tm_prefix_);
        send_key_byte(tm_code_);
    }
    tm_next_ += cycles(typematic_period());
    if (tm_next_ <= now_) tm_next_ = now_ + cycles(typematic_period());
}

}  // namespace ibmpcat
