#include "i8042.h"

namespace ibmpcat {

void I8042::reset() {
    next_write_ = NextWrite::kNone;
    output_buf_ = 0;
    output_full_ = false;
    command_byte_ = 0x00;
    output_port_ = 0x00;
    kbd_enabled_ = true;
    system_flag_ = false;
    last_was_command_ = false;
    reset_requested_ = false;
    irq1_pending_ = false;
    // The keyboard sends 0xAA unsolicited after its power-on self-test. BIOS POST
    // hangs at the keyboard check without it. The real delay is not modeled.
    push_output(0xAA);
}

uint8_t I8042::in(uint16_t port) const {
    if (port == 0x60) {
        output_full_ = false;
        // IRQ1 follows output-buffer-full; reading clears both.
        irq1_pending_ = false;
        uint8_t v = output_buf_;
        if (pending_bat_after_ack_) {
            // ACK of a keyboard RESET was read: queue the 0xAA self-test byte.
            pending_bat_after_ack_ = false;
            output_buf_ = 0xAA;
            output_full_ = true;
        }
        return v;
    }
    // 0x64: status register
    uint8_t status = 0;
    if (output_full_) status |= 0x01;    // OBF
    if (system_flag_) status |= 0x04;
    if (last_was_command_) status |= 0x08;
    if (!kbd_enabled_) status |= 0x10;   // inhibit/enable flag (clone convention)
    return status;
}

void I8042::out(uint16_t port, uint8_t v) {
    if (port == 0x64) {
        last_was_command_ = true;
        switch (v) {
            case 0x20: push_output(command_byte_); break;             // read command byte
            case 0x60: next_write_ = NextWrite::kCommandByte; break;   // write command byte (next byte at 0x60)
            case 0xAA: push_output(0x55); system_flag_ = true; break; // self-test: 0x55 = passed
            case 0xAB: push_output(0x00); break;                       // interface test: 0x00 = no error
            case 0xAD: kbd_enabled_ = false; break;                     // disable keyboard
            case 0xAE: kbd_enabled_ = true; break;                      // enable keyboard
            case 0xD0: push_output(output_port_); break;                // read output port
            case 0xD1: next_write_ = NextWrite::kOutputPort; break;     // write output port (next byte at 0x60)
            case 0xFE: reset_requested_ = true; break;                  // pulse output line 0 -> CPU reset
            default: break;  // other controller commands: no-op
        }
        return;
    }
    // 0x60
    last_was_command_ = false;
    switch (next_write_) {
        case NextWrite::kCommandByte:
            command_byte_ = v;
            next_write_ = NextWrite::kNone;
            break;
        case NextWrite::kOutputPort:
            output_port_ = v;
            if (!(v & 0x01)) reset_requested_ = true;  // bit0 driven low -> CPU reset
            next_write_ = NextWrite::kNone;
            break;
        default:
            // Byte for the keyboard itself: ACK it. RESET (0xFF) also queues a 0xAA
            // self-test byte, which BIOS POST checks for.
            push_output(0xFA);
            if (v == 0xFF) pending_bat_after_ack_ = true;
            break;
    }
}

void I8042::inject_scancode(uint8_t code) {
    if (!kbd_enabled_) return;
    push_output(code);
    if (command_byte_ & 0x01) irq1_pending_ = true;  // IRQ1 enabled in the command byte
}

}  // namespace ibmpcat
