#include "i8042.h"

namespace pc486 {
namespace {

// 2:1 mouse scaling table, Chapweske "The PS/2 Mouse Interface" (2001),
// "Inputs, Resolution, and Scaling": 0,1,1,3,6,9, N>5 -> 2N. Sign passes through.
int scale_2to1(int v) {
    int m = v < 0 ? -v : v;
    int s;
    switch (m) {
        case 0: s = 0; break;
        case 1: s = 1; break;
        case 2: s = 1; break;
        case 3: s = 3; break;
        case 4: s = 6; break;
        case 5: s = 9; break;
        default: s = 2 * m; break;
    }
    return v < 0 ? -s : s;
}

}  // namespace

void I8042::reset() {
    next_write_ = NextWrite::kNone;
    output_buf_ = 0;
    output_full_ = false;
    output_is_aux_ = false;
    command_byte_ = 0x00;
    output_port_ = 0x00;
    kbd_enabled_ = true;
    system_flag_ = false;
    last_was_command_ = false;
    reset_requested_ = false;
    irq1_pending_ = false;
    irq12_pending_ = false;
    queue_head_ = 0;
    queue_count_ = 0;
    typematic_ = 0x2B;
    kbd_arg_cmd_ = 0;
    tm_active_ = false;
    pending_e0_ = false;
    e1_skip_ = 0;
    mouse_ = Mouse();
    // The keyboard sends an unsolicited 0xAA after its power-on BAT, and BIOS
    // keyboard POST hangs without it. Queued immediately (the delay isn't modeled).
    push_kbd(0xAA, true);
    // The mouse's power-on BAT (0xAA, ID 0x00; Chapweske "Reset Mode") isn't queued:
    // it would sit ahead of the keyboard's BAT and POST would read it as a keyboard
    // answer. The Bochs BIOS resets the mouse itself (INT 15h AH=C2h AL=01h).
}

void I8042::enqueue(uint8_t v, bool aux, bool irq) const {
    if (queue_count_ < kQueueSize) {
        queue_[(queue_head_ + queue_count_) % kQueueSize] = Queued{v, aux, irq};
        ++queue_count_;
    }
    pump_output();
}

void I8042::pump_output() const {
    if (output_full_ || queue_count_ == 0) return;
    Queued q = queue_[queue_head_];
    queue_head_ = (queue_head_ + 1) % kQueueSize;
    --queue_count_;
    output_buf_ = q.value;
    output_full_ = true;
    output_is_aux_ = q.aux;
    // Command byte bit 0 enables IRQ1, bit 1 IRQ12. The source port picks the line.
    if (q.irq) {
        if (q.aux) {
            if (command_byte_ & 0x02) irq12_pending_ = true;
        } else {
            if (command_byte_ & 0x01) irq1_pending_ = true;
        }
    }
}

void I8042::push_ctrl(uint8_t v) const {
    output_buf_ = v;
    output_full_ = true;
    output_is_aux_ = false;
}

void I8042::push_kbd(uint8_t v, bool irq) { enqueue(v, false, irq); }

// Every mouse byte raises IRQ12, ACKs included. The Bochs BIOS clears
// command-byte bit 1 around mouse commands (rombios.c inhibit_mouse_int_and_events).
void I8042::push_aux(uint8_t v) const { enqueue(v, true, true); }

uint8_t I8042::in(uint16_t port) const {
    if (port == 0x60) {
        output_full_ = false;
        // Interrupt lines follow OBF: a read clears both, a queued byte re-asserts as it lands
        irq1_pending_ = false;
        irq12_pending_ = false;
        uint8_t v = output_buf_;
        pump_output();
        // Line free: a stream-mode mouse with banked movement starts its next packet here
        if (queue_count_ == 0 && !output_full_) mouse_maybe_report();
        return v;
    }
    // 0x64: status register
    uint8_t status = 0;
    if (output_full_) status |= 0x01;    // OBF
    if (system_flag_) status |= 0x04;
    if (last_was_command_) status |= 0x08;
    if (!kbd_enabled_) status |= 0x10;   // inhibit/enable flag (clone convention)
    // Bit 5 is AUXB on PS/2-superset controllers (byte came from the mouse). The
    // Bochs BIOS get_mouse_data() and INT 74h spin on (inb(0x64) & 0x21) == 0x21.
    // On the original AT it meant transmit timeout.
    if (output_full_ && output_is_aux_) status |= 0x20;
    return status;
}

void I8042::out(uint16_t port, uint8_t v) {
    if (port == 0x64) {
        last_was_command_ = true;
        // A2 selects command vs data, so a write to 0x64 abandons a pending argument.
        // The Bochs set_kbd_command_byte() relies on it: 0xD4 then 0x64 write leaves
        // "write to mouse" unfinished.
        next_write_ = NextWrite::kNone;
        switch (v) {
            case 0x20: push_ctrl(command_byte_); break;                 // read command byte
            case 0x60: next_write_ = NextWrite::kCommandByte; break;   // write command byte (next byte at 0x60)
            case 0xA7: command_byte_ |= 0x20; break;                   // disable AUX interface (clock line low)
            case 0xA8: command_byte_ &= ~0x20; pump_output(); break;   // enable AUX interface
            case 0xA9: push_ctrl(0x00); break;                          // AUX interface test: 0x00 = no error
            case 0xAA: push_ctrl(0x55); system_flag_ = true; break;     // self-test: 0x55 = passed
            case 0xAB: push_ctrl(0x00); break;                          // interface test: 0x00 = no error
            case 0xAD: kbd_enabled_ = false; break;                    // disable keyboard
            case 0xAE: kbd_enabled_ = true; break;                     // enable keyboard
            case 0xD0: push_ctrl(output_port_); break;                  // read output port
            case 0xD1: next_write_ = NextWrite::kOutputPort; break;    // write output port (next byte at 0x60)
            case 0xD2: next_write_ = NextWrite::kKbdOutputBuf; break;  // next byte -> output buffer, as keyboard data
            case 0xD3: next_write_ = NextWrite::kAuxOutputBuf; break;  // next byte -> output buffer, as mouse data
            case 0xD4: next_write_ = NextWrite::kAuxDevice; break;     // next byte -> the mouse itself
            case 0xFE: reset_requested_ = true; break;                 // pulse output line 0 -> CPU reset
            default: break;  // other controller commands: no-op, see PC486_REVIEW.md
        }
        return;
    }
    // 0x60
    last_was_command_ = false;
    NextWrite target = next_write_;
    next_write_ = NextWrite::kNone;
    switch (target) {
        case NextWrite::kCommandByte:
            command_byte_ = v;
            // Bits 0/1 enable IRQ1/IRQ12 and bit 5 releases the AUX clock; enabling any
            // flushes whatever is banked
            pump_output();
            mouse_maybe_report();
            break;
        case NextWrite::kOutputPort:
            output_port_ = v;
            if (!(v & 0x01)) reset_requested_ = true;  // bit0 driven low -> CPU reset
            break;
        case NextWrite::kKbdOutputBuf:
            push_kbd(v, true);  // "act as if this was keyboard data", IRQ1 included
            break;
        case NextWrite::kAuxOutputBuf:
            push_aux(v);
            break;
        case NextWrite::kAuxDevice:
            aux_write(v);
            break;
        default:
            // Keyboard-bound byte: every accepted command is ACKed (LEDs/typematic aren't
            // modeled). 0xFF (RESET) adds a second 0xAA BAT byte that keyboard POST checks.
            // 0xF2 (Read ID) adds 0xAB, 0x83 (Chapweske "The AT-PS/2 Keyboard Interface").
            // These are device output so irq=true. With false, MS-DOS 6.22 SETUP sent 0xF2,
            // saw no IRQ1, retried, and the unread ACKs wedged the output register.
            push_kbd(0xFA, true);
            if (kbd_arg_cmd_ != 0) {
                if (kbd_arg_cmd_ == 0xF3) typematic_ = uint8_t(v & 0x7F);
                kbd_arg_cmd_ = 0;
                break;
            }
            if (v == 0xED || v == 0xF0 || v == 0xF3) kbd_arg_cmd_ = v;
            if (v == 0xF6 || v == 0xFF) { typematic_ = 0x2B; tm_active_ = false; }
            if (v == 0xFF) push_kbd(0xAA, true);
            if (v == 0xF2) { push_kbd(0xAB, true); push_kbd(0x83, true); }
            break;
    }
}

void I8042::inject_scancode(uint8_t code) {
    if (kbd_enabled_) push_kbd(code, true);
    // Key state is tracked even while the keyboard is held off, so a release still ends repeat
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
        tm_next_ = now_s_ + typematic_delay();
    } else if (tm_active_ && base == tm_code_) {
        // Matched on the code alone so a break with a detached E0 still ends the repeat
        tm_active_ = false;
    }
}

void I8042::typematic_fire() {
    // Never split a sequence the host is partway through sending
    if (pending_e0_ || e1_skip_ > 0) return;
    if (kbd_enabled_) {
        if (tm_prefix_) push_kbd(tm_prefix_, true);
        push_kbd(tm_code_, true);
    }
    tm_next_ += typematic_period();
    if (tm_next_ <= now_s_) tm_next_ = now_s_ + typematic_period();
}

// ---- PS/2 mouse on the AUX port ----
// Chapweske, "The PS/2 Mouse Interface" (2001), cross-checked against the Bochs
// BIOS INT 15h AH=C2h (rombios.c int15_function_mouse) and INT 74h handler.
// Plain 3-button mouse, ID 0x00, 3-byte packets. No IntelliMouse (1996); its
// knock (rates 200, 100, 80 then read ID) is accepted and the ID stays 0x00.

void I8042::aux_respond(const uint8_t* bytes, int n) const {
    for (int i = 0; i < n && i < 4; ++i) mouse_.last_packet[i] = bytes[i];
    mouse_.last_packet_len = static_cast<uint8_t>(n < 4 ? n : 4);
    for (int i = 0; i < n; ++i) push_aux(bytes[i]);
}

void I8042::mouse_set_defaults() {
    mouse_.sample_rate = 100;
    mouse_.resolution = 0x02;  // 4 counts/mm
    mouse_.scaling_2to1 = false;
    mouse_.reporting = false;
}

void I8042::mouse_clear_counters() const {
    mouse_.dx = 0;
    mouse_.dy = 0;
    mouse_.x_overflow = false;
    mouse_.y_overflow = false;
    mouse_.sample_pending = false;
}

void I8042::mouse_queue_packet(bool apply_scaling) const {
    int dx = mouse_.dx;
    int dy = mouse_.dy;
    // 2:1 scaling applies only to stream mode reporting, not Read Data (0xEB)
    // (Chapweske footnote 1)
    if (apply_scaling && mouse_.scaling_2to1) {
        dx = scale_2to1(dx);
        dy = scale_2to1(dy);
    }
    bool x_over = mouse_.x_overflow;
    bool y_over = mouse_.y_overflow;
    // The reported field is 9 bits, so a scaled value that no longer fits also overflows
    if (dx > 255) { dx = 255; x_over = true; }
    if (dx < -255) { dx = -255; x_over = true; }
    if (dy > 255) { dy = 255; y_over = true; }
    if (dy < -255) { dy = -255; y_over = true; }

    // Byte 1: [Y ovf][X ovf][Y sign][X sign][1][middle][right][left]. Drivers
    // discard packets without bit 3 (Chapweske footnote 4).
    uint8_t b0 = 0x08 | (mouse_.buttons & 0x07);
    if (dx < 0) b0 |= 0x10;
    if (dy < 0) b0 |= 0x20;
    if (x_over) b0 |= 0x40;
    if (y_over) b0 |= 0x80;
    const uint8_t packet[3] = {b0, static_cast<uint8_t>(dx & 0xFF),
                               static_cast<uint8_t>(dy & 0xFF)};
    aux_respond(packet, 3);
    // Counters reset after a packet is sent (Chapweske)
    mouse_clear_counters();
}

bool I8042::mouse_reporting_enabled() const {
    return mouse_.reporting && mouse_.mode == MouseMode::kStream &&
           !(command_byte_ & 0x20);
}

void I8042::mouse_maybe_report() const {
    if (!mouse_reporting_enabled() || !mouse_.sample_pending) return;
    // The mouse can't transmit while the previous packet is going out (clock
    // held low while OBF is set). It keeps sampling and sends when the line frees.
    if (queue_count_ != 0) return;
    if (output_full_ && output_is_aux_) return;
    mouse_queue_packet(true);
}

void I8042::inject_mouse_event(int dx, int dy, uint8_t buttons) {
    mouse_.buttons = buttons & 0x07;
    // Counters range -255..+255; past that the overflow bit sets and the counter
    // stops (Chapweske). The excess is lost on real hardware.
    mouse_.dx += dx;
    if (mouse_.dx > 255) { mouse_.dx = 255; mouse_.x_overflow = true; }
    if (mouse_.dx < -255) { mouse_.dx = -255; mouse_.x_overflow = true; }
    mouse_.dy += dy;
    if (mouse_.dy > 255) { mouse_.dy = 255; mouse_.y_overflow = true; }
    if (mouse_.dy < -255) { mouse_.dy = -255; mouse_.y_overflow = true; }
    // Stream mode reports on movement or button change
    mouse_.sample_pending = true;
    mouse_maybe_report();
}

void I8042::aux_write(uint8_t v) {
    // Wrap mode echoes every byte; only 0xFF and 0xEC are still obeyed
    if (mouse_.mode == MouseMode::kWrap && v != 0xFF && v != 0xEC) {
        push_aux(v);
        return;
    }

    if (mouse_.expect_param != 0) {
        uint8_t cmd = mouse_.expect_param;
        mouse_.expect_param = 0;
        const uint8_t ack[1] = {0xFA};
        if (cmd == 0xF3) {
            // Valid rates: 10, 20, 40, 60, 80, 100, 200 samples/sec
            mouse_.sample_rate = v;
        } else if (cmd == 0xE8) {
            mouse_.resolution = v & 0x03;  // 0..3 => 1/2/4/8 counts/mm
        }
        aux_respond(ack, 1);
        mouse_clear_counters();
        return;
    }

    const uint8_t ack[1] = {0xFA};
    switch (v) {
        case 0xFF: {  // Reset
            // ACK, BAT result, device ID: three bytes, as the Bochs BIOS reads them (rombios.c)
            const uint8_t resp[3] = {0xFA, 0xAA, 0x00};
            mouse_.mode = MouseMode::kStream;
            mouse_.mode_before_wrap = MouseMode::kStream;
            mouse_set_defaults();
            mouse_clear_counters();
            mouse_.buttons = 0;
            aux_respond(resp, 3);
            break;
        }
        case 0xFE:  // Resend -- repeat the last thing the mouse sent
            if (mouse_.last_packet_len > 0) {
                // Not through aux_respond(): a resend must not become the thing a further
                // resend repeats, and it doesn't clear the counters
                for (int i = 0; i < mouse_.last_packet_len; ++i) push_aux(mouse_.last_packet[i]);
            }
            return;
        case 0xF6:  // Set defaults
            mouse_set_defaults();
            mouse_.mode = MouseMode::kStream;
            aux_respond(ack, 1);
            break;
        case 0xF5:  // Disable data reporting
            mouse_.reporting = false;
            aux_respond(ack, 1);
            break;
        case 0xF4:  // Enable data reporting
            mouse_.reporting = true;
            aux_respond(ack, 1);
            break;
        case 0xF3:  // Set sample rate: ACK, then one argument byte
        case 0xE8:  // Set resolution: ACK, then one argument byte
            mouse_.expect_param = v;
            aux_respond(ack, 1);
            break;
        case 0xF2: {  // Get device ID
            const uint8_t resp[2] = {0xFA, 0x00};  // 0x00: standard PS/2 mouse
            aux_respond(resp, 2);
            break;
        }
        case 0xF0:  // Set remote mode
            mouse_.mode = MouseMode::kRemote;
            aux_respond(ack, 1);
            break;
        case 0xEE:  // Set wrap mode
            mouse_.mode_before_wrap = mouse_.mode;
            mouse_.mode = MouseMode::kWrap;
            aux_respond(ack, 1);
            break;
        case 0xEC:  // Reset wrap mode -- back to whatever mode preceded it
            if (mouse_.mode == MouseMode::kWrap) mouse_.mode = mouse_.mode_before_wrap;
            aux_respond(ack, 1);
            break;
        case 0xEB:  // Read data: ACK, then one packet, unscaled (footnote 1)
            aux_respond(ack, 1);
            mouse_queue_packet(false);
            return;
        case 0xEA:  // Set stream mode
            mouse_.mode = MouseMode::kStream;
            aux_respond(ack, 1);
            break;
        case 0xE9: {  // Status request
            // Byte 1: [0][mode][enable][scaling][0][left][middle][right], button order
            // reversed from the movement packet. Byte 2 is the resolution code, byte 3 the
            // sample rate (Chapweske's example: FA 00 02 64).
            uint8_t b0 = 0;
            if (mouse_.mode == MouseMode::kRemote) b0 |= 0x40;
            if (mouse_.reporting) b0 |= 0x20;
            if (mouse_.scaling_2to1) b0 |= 0x10;
            if (mouse_.buttons & kMouseLeft) b0 |= 0x04;
            if (mouse_.buttons & kMouseMiddle) b0 |= 0x02;
            if (mouse_.buttons & kMouseRight) b0 |= 0x01;
            const uint8_t resp[4] = {0xFA, b0, mouse_.resolution, mouse_.sample_rate};
            aux_respond(resp, 4);
            break;
        }
        case 0xE7:  // Set scaling 2:1
            mouse_.scaling_2to1 = true;
            aux_respond(ack, 1);
            break;
        case 0xE6:  // Set scaling 1:1
            mouse_.scaling_2to1 = false;
            aux_respond(ack, 1);
            break;
        default: {
            // Unknown commands answer 0xFE (Resend/error), which is how a driver
            // probing for an extension learns it's absent
            const uint8_t resend[1] = {0xFE};
            aux_respond(resend, 1);
            return;
        }
    }
    // Counters reset after any host command except Resend (0xFE) (Chapweske)
    mouse_clear_counters();
}

}  // namespace pc486
