// Intel 8042 keyboard controller, AT wiring, with the PS/2 auxiliary
// (mouse) port every 486-class board carries.
//
// Ports 0x60 (data) / 0x64 (status on read, command on write). Beyond
// talking to the keyboard itself, the AT's 8042 firmware carries two
// motherboard-level jobs software depends on constantly:
//
//   - Output Port bit 1 gates the A20 address line. It defaults DISABLED
//     at power-on/reset (an AT starts out wrapping at 1MB exactly like an
//     8086, for backward compatibility with real-mode software that
//     depends on the wraparound) -- BIOS enables it early in POST via the
//     "write output port" command (0xD1 then a data byte with bit1 set)
//     before doing anything above 1MB. `chipset.h`'s memory decode reads
//     a20_enabled() to mask/pass address bit 20 accordingly.
//   - Output Port bit 0, driven low (or the 0xFE "pulse output line 0"
//     command), pulses the CPU's RESET line. Early 286es have no
//     instruction to leave protected mode other than a full reset, so
//     real-mode-return sequences of the era use exactly this trick.
//     reset_requested() surfaces that to the embedding Machine, which
//     calls cpu.reset() -- this device has no direct reach into the CPU
//     object, matching every other device in this codebase.
//
// The second device port is genuinely a PS/2-line feature, not an original
// PC/AT one: the 5170's own 8042 firmware ignores 0xA7/0xA8/0xD2/0xD3/0xD4
// outright (OS/2 Museum's disassembly of the AT KBC ROM lists that whole
// range as "ignored"). Every 486 board with a round mouse DIN is running a
// PS/2-superset KBC instead, which is what this models -- see
// PC486_REVIEW.md §10.
//
// Controller-command scope: self-test, interface test, read/write command
// byte, enable/disable keyboard, enable/disable/test the AUX interface,
// write-to-AUX-device, write-to-either-output-buffer, read/write output
// port, pulse-output-line-0. LED state and typematic rate aren't modeled
// (any command byte the keyboard itself doesn't specifically need just
// gets ACKed). Reference: IBM 5170 Technical Reference, "Keyboard System";
// the 8042 command set is otherwise identical across the whole
// PC/AT-compatible universe and is documented in any AT-class BIOS's
// keyboard POST routine.
#ifndef PC486_I8042_H
#define PC486_I8042_H

#include <cstdint>

namespace pc486 {

class I8042 {
public:
    void reset();

    bool owns(uint16_t port) const { return port == 0x60 || port == 0x64; }
    uint8_t in(uint16_t port) const;
    void out(uint16_t port, uint8_t v);

    bool irq1_pending() const { return irq1_pending_; }
    void clear_irq1() { irq1_pending_ = false; }

    // IRQ12, the AUX port's own interrupt line. Driven by the same output
    // buffer as IRQ1, but only while the byte sitting in it came from the
    // mouse (status bit 5, AUXB) and command-byte bit 1 enables it.
    bool irq12_pending() const { return irq12_pending_; }
    void clear_irq12() { irq12_pending_ = false; }

    bool a20_enabled() const { return (output_port_ & 0x02) != 0; }

    bool reset_requested() const { return reset_requested_; }
    void clear_reset_request() { reset_requested_ = false; }

    // Host/front-end side: deliver one Set 1 scan code, as if a key event
    // just happened -- pushed straight to port 0x60 with no translation.
    // No-op while the controller has the keyboard disabled (0xAD).
    //
    // Real hardware: an AT keyboard is physically Set-2-native, and the
    // 8042's own firmware translates Set 2 -> Set 1 before software ever
    // sees a code at port 0x60 (translation is the default/overwhelmingly
    // common mode; genuine Set-2 passthrough exists but essentially no
    // real software selects it). This device models the visible result of
    // that translation directly rather than a Set-2 stage nothing above
    // the 8042 can ever observe in that default mode -- callers (a browser
    // front end's own physical-key -> Set-1 table, disks/build_freedos_hdd.cpp's
    // Set1MakeCode()) supply Set 1 codes already, matching what a real
    // BIOS/DOS keyboard driver actually reads.
    void inject_scancode(uint8_t code);

    // Button bits, in the order the PS/2 movement packet's first byte
    // carries them (Chapweske, "The PS/2 Mouse Interface", 2001).
    enum MouseButton : uint8_t {
        kMouseLeft = 0x01,
        kMouseRight = 0x02,
        kMouseMiddle = 0x04,
    };

    // Host/front-end side: one sample from the mouse's own optics/switches.
    //
    // dx/dy are movement *counts*, in the mouse's axis convention: +X is
    // right, +Y is AWAY from the user (up the screen). A browser front end
    // whose mousemove deltas grow downward must negate dy -- the real part
    // reports "move up one" as 08 00 01 and "move down one" as 28 00 FF.
    // `buttons` is the full current button state (kMouseLeft etc.), not a
    // change: a real mouse latches the switch states on every sample.
    //
    // Counts accumulate in the mouse's own 9-bit movement counters exactly
    // as they do on real hardware -- sampling continues while the host has
    // the line inhibited or the previous packet still in flight, and the
    // counters clear only when a packet is actually sent. Whether that
    // produces a packet depends on the device's mode and on whether
    // reporting is enabled; see mouse_reporting_enabled().
    void inject_mouse_event(int dx, int dy, uint8_t buttons);

    // True once a driver has put the mouse in stream mode with reporting
    // enabled (0xF4) and the controller is not holding the AUX clock low.
    // A front end can use this to tell whether anything is listening
    // before it starts capturing the pointer.
    bool mouse_reporting_enabled() const;

private:
    enum class NextWrite {
        kNone,
        kCommandByte,     // 0x60: controller command byte
        kOutputPort,      // 0xD1: output port (A20, CPU reset)
        kKbdOutputBuf,    // 0xD2: fake a byte from the keyboard
        kAuxOutputBuf,    // 0xD3: fake a byte from the mouse
        kAuxDevice,       // 0xD4: send a byte to the mouse itself
    };
    NextWrite next_write_ = NextWrite::kNone;

    mutable uint8_t output_buf_ = 0;
    mutable bool output_full_ = false;
    mutable bool output_is_aux_ = false;  // status bit 5 (AUXB) for the byte in output_buf_
    uint8_t command_byte_ = 0x00;
    uint8_t output_port_ = 0x00;  // bit1 (A20) starts disabled -- see file header
    bool kbd_enabled_ = true;
    bool system_flag_ = false;
    bool last_was_command_ = false;
    bool reset_requested_ = false;
    mutable bool irq1_pending_ = false;
    mutable bool irq12_pending_ = false;

    // The output buffer is one byte wide on the real part, but both devices
    // behind it answer in multi-byte bursts (a keyboard RESET's ACK + BAT,
    // a mouse RESET's ACK + BAT + device ID, every 3-byte movement packet),
    // and the keyboard can produce a scan code part-way through one of the
    // mouse's. That queues on real hardware in the *devices*, not the
    // controller: the 8042 holds a device's clock line low while OBF is
    // set, and the device keeps its bytes until the line is released -- no
    // byte is ever lost, only delayed. This FIFO stands in for both
    // devices' holding buffers, each byte tagged with the source that
    // decides status bit 5 and which interrupt it drives.
    //
    // Sized generously (not the real AT keyboard's own 16-byte depth)
    // because `enqueue()` below drops a byte outright if the queue is
    // full, which the real device-side holdoff this stands in for never
    // does -- a keyboard break code landing at exactly the wrong moment
    // during a burst of mouse-movement packets found this the hard way
    // (a real, reported "key stuck forever" bug: the guest's IRQ1/IRQ12
    // handlers fell behind a rapid mouse-look burst, the queue filled, and
    // the one byte that would have released the key never arrived). This
    // is a size increase, not real backpressure -- a sustained enough
    // mismatch between production and draining could still overflow it --
    // but comfortably covers any realistic gameplay burst.
    //
    // Answers to controller commands do not go through here -- see
    // push_ctrl().
    //
    // `irq` records whether this byte should assert an interrupt as it
    // reaches the buffer. IRQ1 and IRQ12 are not the OBF flag itself on
    // the real part -- they are output-port lines (P24, P25) the 8042's
    // own firmware drives. Answers to *controller* commands (self-test,
    // read command byte, AUX interface test) are polled for by whoever
    // issued them and raise nothing here; scan codes raise IRQ1, and every
    // byte from the mouse raises IRQ12 -- the firmware this machine ships
    // depends on that second one, which is why it disables IRQ12 around
    // its own mouse commands (PC486_REVIEW.md §10).
    struct Queued { uint8_t value; bool aux; bool irq; };
    static constexpr int kQueueSize = 1024;
    mutable Queued queue_[kQueueSize] = {};
    mutable int queue_head_ = 0;
    mutable int queue_count_ = 0;

    // The PS/2 mouse itself. Every field here is the device's state, not
    // the controller's; defaults are the ones its BAT loads (sample rate
    // 100/sec, resolution 4 counts/mm, 1:1 scaling, reporting disabled,
    // stream mode) per Chapweske's "Reset Mode".
    enum class MouseMode { kStream, kRemote, kWrap };
    struct Mouse {
        MouseMode mode = MouseMode::kStream;
        MouseMode mode_before_wrap = MouseMode::kStream;
        bool reporting = false;
        bool scaling_2to1 = false;
        uint8_t resolution = 0x02;   // 0..3 => 1/2/4/8 counts/mm
        uint8_t sample_rate = 100;
        uint8_t buttons = 0;
        int dx = 0;
        int dy = 0;
        bool x_overflow = false;
        bool y_overflow = false;
        bool sample_pending = false;  // something to report since the last packet
        uint8_t expect_param = 0;     // 0, or the command (0xF3/0xE8) awaiting its argument
        uint8_t last_packet[4] = {};  // what 0xFE (Resend) re-sends
        uint8_t last_packet_len = 0;
    };
    mutable Mouse mouse_;

    // The 8042 answering for itself (self-test, read command byte, read
    // output port, interface tests): the firmware writes its answer
    // straight into the one-byte output buffer, over whatever was sitting
    // there unread, and raises no interrupt. It has no queue of its own,
    // and every caller of these commands polls for the answer.
    void push_ctrl(uint8_t v) const;
    void push_kbd(uint8_t v, bool irq = false);
    void push_aux(uint8_t v) const;
    void enqueue(uint8_t v, bool aux, bool irq) const;
    void pump_output() const;

    void aux_write(uint8_t v);
    void aux_respond(const uint8_t* bytes, int n) const;
    void mouse_set_defaults();
    void mouse_clear_counters() const;
    void mouse_queue_packet(bool apply_scaling) const;
    void mouse_maybe_report() const;
};

}  // namespace pc486

#endif  // PC486_I8042_H
