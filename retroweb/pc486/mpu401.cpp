#include "mpu401.h"

namespace pc486 {

void Mpu401::reset() {
    uart_mode_ = false;
    ack_pending_ = false;
}

uint8_t Mpu401::in(uint16_t port) {
    if (port == uint16_t(base_ + 1)) {
        // Bit 6 (output-not-ready) stays clear: commands complete when written.
        // Bit 7 (input-not-available) clears only while an ack waits (SBPG ch.5).
        return ack_pending_ ? 0x00 : 0x80;
    }
    if (ack_pending_) {
        ack_pending_ = false;
        return 0xFE;  // Command Acknowledge, SBPG ch.5
    }
    return 0x00;  // no MIDI input ever arrives
}

void Mpu401::out(uint16_t port, uint8_t v) {
    if (port == uint16_t(base_ + 1)) {
        if (v == 0xFF) {       // Reset
            uart_mode_ = false;
            ack_pending_ = true;
        } else if (v == 0x3F) {  // Enter UART mode
            uart_mode_ = true;
            ack_pending_ = true;
        }
        // Other commands are Intelligent-mode (SBPG ch.5): ignored, no ack
        return;
    }
    // Data port: a written byte goes out MIDI OUT; nothing is attached, so it's discarded
}

}  // namespace pc486
