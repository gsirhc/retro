#include "i8042.h"

namespace pc486 {
namespace {

// 2:1 scaling, applied by the mouse to its movement counters before it
// reports them. Table verbatim from Chapweske, "The PS/2 Mouse Interface"
// (2001), "Inputs, Resolution, and Scaling": 0->0, 1->1, 2->1, 3->3, 4->6,
// 5->9, N>5 -> 2N. The table is stated for magnitudes; the sign rides
// through unchanged.
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
    mouse_ = Mouse();
    // A real AT keyboard runs its own power-on Basic Assurance Test and
    // reports success by sending 0xAA *unsolicited* -- no command needed --
    // as soon as it finishes, independent of the controller's own 0xAA
    // self-test command. BIOS's keyboard POST waits for exactly this byte;
    // without it, POST hangs forever at the keyboard-presence check. Real
    // hardware has a short delay before this arrives; modeled here as
    // already sitting in the output buffer immediately after reset, since
    // nothing currently depends on the delay itself.
    push_kbd(0xAA, true);
    // The mouse runs the same power-on BAT (~500ms on real hardware) and
    // answers with 0xAA then its device ID 0x00 -- Chapweske, "Reset Mode".
    // Those bytes are not queued here: with the AUX clock in its power-on
    // state and no driver yet, a byte pair sitting in the shared buffer
    // ahead of the keyboard's own BAT would be read by BIOS's keyboard
    // POST as a keyboard answer. Real firmware never sees them either --
    // the Bochs BIOS this machine ships resets the mouse explicitly (INT
    // 15h AH=C2h AL=01h) and reads the pair from there.
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
    // The command byte's own enables gate the two lines: bit 0 for IRQ1,
    // bit 1 for IRQ12. Which line a device byte drives is decided by the
    // port it came in on, not by its value.
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

// Every byte the mouse itself sends raises IRQ12 -- command ACKs included,
// which is exactly why the Bochs BIOS clears command-byte bit 1 before it
// talks to the device and restores it afterwards (rombios.c,
// inhibit_mouse_int_and_events / enable_mouse_int_and_events).
void I8042::push_aux(uint8_t v) const { enqueue(v, true, true); }

uint8_t I8042::in(uint16_t port) const {
    if (port == 0x60) {
        output_full_ = false;
        // Real hardware: the interrupt lines follow "output buffer full" --
        // reading the buffer clears both simultaneously. If another byte is
        // waiting behind it, the line re-asserts as that byte lands.
        irq1_pending_ = false;
        irq12_pending_ = false;
        uint8_t v = output_buf_;
        pump_output();
        // The line is free again: a stream-mode mouse with movement banked
        // in its counters starts its next packet right here.
        if (queue_count_ == 0 && !output_full_) mouse_maybe_report();
        return v;
    }
    // 0x64: status register
    uint8_t status = 0;
    if (output_full_) status |= 0x01;    // OBF
    if (system_flag_) status |= 0x04;
    if (last_was_command_) status |= 0x08;
    if (!kbd_enabled_) status |= 0x10;   // inhibit/enable flag (clone convention)
    // Bit 5 is AUXB on a PS/2-superset controller: the byte in the output
    // buffer came from the mouse, not the keyboard. Not optional -- the
    // Bochs BIOS's get_mouse_data() and its INT 74h handler both spin on
    // `(inb(0x64) & 0x21) == 0x21`, so a mouse byte without this bit is a
    // byte they never read. (On the original AT this bit meant a
    // transmit timeout instead; that machine had no AUX port.)
    if (output_full_ && output_is_aux_) status |= 0x20;
    return status;
}

void I8042::out(uint16_t port, uint8_t v) {
    if (port == 0x64) {
        last_was_command_ = true;
        // The 8042 tells commands from data by the A2 address line, so a
        // write to 0x64 always starts a new command and abandons any
        // argument byte an earlier one was still waiting for. The Bochs
        // BIOS depends on this: set_kbd_command_byte() writes 0xD4 to 0x64
        // and then immediately 0x60 to 0x64, leaving that "write to mouse"
        // permanently unfinished.
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
            // Bits 0/1 are the IRQ1/IRQ12 enables and bit 5 releases the
            // AUX clock; a driver turning any of them on wants whatever is
            // already banked to come out.
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
            // A byte meant for the keyboard itself (set-LEDs, set typematic
            // rate, enable scanning, reset, ...). LED/typematic state isn't
            // modeled; every accepted command just gets ACKed like a real
            // keyboard would -- except 0xFF (RESET), where a real keyboard
            // follows its ACK with a second, separate self-test-passed
            // byte (0xAA), which real BIOS keyboard POST explicitly checks
            // for (see PC486_REVIEW.md), and 0xF2 (Read ID), whose ACK is
            // followed by a genuine two-byte device ID, 0xAB then 0x83 --
            // "the keyboard responds by sending a two-byte device ID of
            // 0xAB, 0x83" (Chapweske, "The AT-PS/2 Keyboard Interface",
            // command 0xF2). Every one of these is real keyboard-device
            // output, not the controller answering for itself (push_ctrl,
            // above), so it takes irq=true, not the false this file used to
            // pass here -- see push_kbd's own comment in i8042.h for the
            // citation that caught it. That was a real, live bug: MS-DOS
            // 6.22's SETUP.EXE sends 0xF2 during its own keyboard probe,
            // gets an ACK it correctly interprets as "arrived" via the
            // (missing) IRQ1 rather than by polling, and -- seeing no
            // interrupt -- retries 0xF2 three times over. Every one of
            // those four unacknowledged ACKs (and, before this fix, the
            // missing ID bytes too) piled up unread behind the single-byte
            // output register (see chipset.cpp's IRQ1 comment on that
            // register), wedging it full forever and silently dropping
            // every keystroke typed afterward, Setup included.
            push_kbd(0xFA, true);
            if (v == 0xFF) push_kbd(0xAA, true);
            if (v == 0xF2) { push_kbd(0xAB, true); push_kbd(0x83, true); }
            break;
    }
}

void I8042::inject_scancode(uint8_t code) {
    if (!kbd_enabled_) return;
    push_kbd(code, true);
}

// ---------------------------------------------------------------------------
// The PS/2 mouse on the AUX port.
//
// Command set, responses, defaults, packet layout and scaling: Adam
// Chapweske, "The PS/2 Mouse Interface" (2001) -- the reference every later
// description of this protocol descends from, itself derived from IBM's
// PS/2 technical reference. Cross-checked against what this machine's own
// firmware actually does: the Bochs BIOS's INT 15h AH=C2h implementation
// (`rombios.c`, int15_function_mouse) and its INT 74h packet handler.
//
// This is a plain 3-button PS/2 mouse: device ID 0x00, 3-byte packets. The
// Microsoft IntelliMouse's 4-byte wheel packet is deliberately absent --
// that part shipped in 1996, two years after this machine's build date,
// and no DOS software of the era asks for it. Its "knock" (set sample rate
// 200, 100, 80, then read device ID) is answered the way a standard mouse
// answers it: the rates are accepted and the ID stays 0x00, which is
// exactly how a driver probing for a wheel learns there isn't one.
// ---------------------------------------------------------------------------

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
    // "2:1 scaling only applies to the automatic data reporting in Stream
    // mode. It does not effect the reported data sent in response to the
    // Read Data (0xEB) command." -- Chapweske, footnote 1.
    if (apply_scaling && mouse_.scaling_2to1) {
        dx = scale_2to1(dx);
        dy = scale_2to1(dy);
    }
    bool x_over = mouse_.x_overflow;
    bool y_over = mouse_.y_overflow;
    // The reported field is 9 bits wide whatever the counters hold, so a
    // doubled scaled value that no longer fits reports as an overflow too.
    if (dx > 255) { dx = 255; x_over = true; }
    if (dx < -255) { dx = -255; x_over = true; }
    if (dy > 255) { dy = 255; y_over = true; }
    if (dy < -255) { dy = -255; y_over = true; }

    // Byte 1: [Y overflow][X overflow][Y sign][X sign][always 1][middle][right][left]
    // Bit 3 is not decoration -- drivers that check it discard packets
    // without it (Chapweske, footnote 4).
    uint8_t b0 = 0x08 | (mouse_.buttons & 0x07);
    if (dx < 0) b0 |= 0x10;
    if (dy < 0) b0 |= 0x20;
    if (x_over) b0 |= 0x40;
    if (y_over) b0 |= 0x80;
    const uint8_t packet[3] = {b0, static_cast<uint8_t>(dx & 0xFF),
                               static_cast<uint8_t>(dy & 0xFF)};
    aux_respond(packet, 3);
    // "after a packet is sent to the host, the movement counters are reset"
    mouse_clear_counters();
}

bool I8042::mouse_reporting_enabled() const {
    return mouse_.reporting && mouse_.mode == MouseMode::kStream &&
           !(command_byte_ & 0x20);
}

void I8042::mouse_maybe_report() const {
    if (!mouse_reporting_enabled() || !mouse_.sample_pending) return;
    // A real mouse cannot begin a transmission while its previous packet is
    // still going out, and the controller holds its clock low the whole
    // time OBF is set. It keeps sampling into its counters meanwhile, and
    // sends as soon as the line frees up (see in(), port 0x60).
    if (queue_count_ != 0) return;
    if (output_full_ && output_is_aux_) return;
    mouse_queue_packet(true);
}

void I8042::inject_mouse_event(int dx, int dy, uint8_t buttons) {
    mouse_.buttons = buttons & 0x07;
    // "The range of values that can be expressed by the movement counters
    // is -255 to +255. If this range is exceeded, the appropriate overflow
    // bit is set and the counter is not incremented/decremented until it is
    // reset." -- Chapweske. The excess is genuinely lost on real hardware.
    mouse_.dx += dx;
    if (mouse_.dx > 255) { mouse_.dx = 255; mouse_.x_overflow = true; }
    if (mouse_.dx < -255) { mouse_.dx = -255; mouse_.x_overflow = true; }
    mouse_.dy += dy;
    if (mouse_.dy > 255) { mouse_.dy = 255; mouse_.y_overflow = true; }
    if (mouse_.dy < -255) { mouse_.dy = -255; mouse_.y_overflow = true; }
    // Stream mode reports on movement *or* a button change; the mouse
    // latches its inputs on every sample either way, so any injected event
    // is something to report.
    mouse_.sample_pending = true;
    mouse_maybe_report();
}

void I8042::aux_write(uint8_t v) {
    // Wrap mode echoes every byte back untouched, valid commands included;
    // only 0xFF (Reset) and 0xEC (Reset Wrap Mode) are still obeyed.
    if (mouse_.mode == MouseMode::kWrap && v != 0xFF && v != 0xEC) {
        push_aux(v);
        return;
    }

    if (mouse_.expect_param != 0) {
        uint8_t cmd = mouse_.expect_param;
        mouse_.expect_param = 0;
        const uint8_t ack = 0xFA;
        if (cmd == 0xF3) {
            // Valid rates are 10, 20, 40, 60, 80, 100 and 200 samples/sec.
            mouse_.sample_rate = v;
        } else if (cmd == 0xE8) {
            mouse_.resolution = v & 0x03;  // 0..3 => 1/2/4/8 counts/mm
        }
        aux_respond(&ack, 1);
        mouse_clear_counters();
        return;
    }

    const uint8_t ack = 0xFA;
    switch (v) {
        case 0xFF: {  // Reset
            // ACK, then the BAT result, then the device ID -- three
            // separate bytes, which is exactly what the Bochs BIOS's INT
            // 15h AH=C2h AL=01h reads back (rombios.c: one get_mouse_data()
            // for the 0xFA, then two more for 0xAA and 0x00).
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
                // Deliberately not through aux_respond(): a resend must not
                // become the thing a further resend repeats, and it is the
                // one command that does not clear the movement counters.
                for (int i = 0; i < mouse_.last_packet_len; ++i) push_aux(mouse_.last_packet[i]);
            }
            return;
        case 0xF6:  // Set defaults
            mouse_set_defaults();
            mouse_.mode = MouseMode::kStream;
            aux_respond(&ack, 1);
            break;
        case 0xF5:  // Disable data reporting
            mouse_.reporting = false;
            aux_respond(&ack, 1);
            break;
        case 0xF4:  // Enable data reporting
            mouse_.reporting = true;
            aux_respond(&ack, 1);
            break;
        case 0xF3:  // Set sample rate: ACK, then one argument byte
        case 0xE8:  // Set resolution: ACK, then one argument byte
            mouse_.expect_param = v;
            aux_respond(&ack, 1);
            break;
        case 0xF2: {  // Get device ID
            const uint8_t resp[2] = {0xFA, 0x00};  // 0x00: standard PS/2 mouse
            aux_respond(resp, 2);
            break;
        }
        case 0xF0:  // Set remote mode
            mouse_.mode = MouseMode::kRemote;
            aux_respond(&ack, 1);
            break;
        case 0xEE:  // Set wrap mode
            mouse_.mode_before_wrap = mouse_.mode;
            mouse_.mode = MouseMode::kWrap;
            aux_respond(&ack, 1);
            break;
        case 0xEC:  // Reset wrap mode -- back to whatever mode preceded it
            if (mouse_.mode == MouseMode::kWrap) mouse_.mode = mouse_.mode_before_wrap;
            aux_respond(&ack, 1);
            break;
        case 0xEB:  // Read data: ACK, then one packet, unscaled (footnote 1)
            aux_respond(&ack, 1);
            mouse_queue_packet(false);
            return;
        case 0xEA:  // Set stream mode
            mouse_.mode = MouseMode::kStream;
            aux_respond(&ack, 1);
            break;
        case 0xE9: {  // Status request
            // Byte 1: [0][mode][enable][scaling][0][left][middle][right] --
            // note the button order is the reverse of the movement
            // packet's. Byte 2 is the resolution *code* (0..3), byte 3 the
            // sample rate: Chapweske's own worked example answers this
            // command with FA 00 02 64 at defaults.
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
            aux_respond(&ack, 1);
            break;
        case 0xE6:  // Set scaling 1:1
            mouse_.scaling_2to1 = false;
            aux_respond(&ack, 1);
            break;
        default: {
            // A real device answers a command it does not recognise with
            // 0xFE (Resend/error) rather than a bare ACK, which is how a
            // driver probing for an extension finds out it is absent.
            const uint8_t resend = 0xFE;
            aux_respond(&resend, 1);
            return;
        }
    }
    // "the movement counters are reset ... after the mouse receives any
    // command from the host other than the Resend (0xFE) command."
    mouse_clear_counters();
}

}  // namespace pc486
