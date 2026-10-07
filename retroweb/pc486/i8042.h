// Intel 8042 keyboard controller, AT wiring, with the PS/2 auxiliary (mouse)
// port every 486-class board carries. Ports 0x60 (data), 0x64 (status/command).
// Motherboard jobs:
// - Output Port bit 1 gates A20. It starts disabled (wrap at 1MB like an 8086);
//   BIOS enables it with 0xD1 early in POST. chipset.h reads a20_enabled().
// - Output Port bit 0 driven low, or command 0xFE, pulses CPU RESET.
//   reset_requested() surfaces it to Machine, which calls cpu.reset().
// The AUX port is a PS/2 feature: the 5170's KBC firmware ignores
// 0xA7/0xA8/0xD2/0xD3/0xD4 (OS/2 Museum's AT KBC ROM disassembly), so this
// models a PS/2-superset KBC (PC486_REVIEW.md §10).
// Port 0x92 (Fast A20 Gate) is modeled here because it drives the same A20
// signal. Bit 1 is the gate, bit 0 fast-resets on a 1 write (OSDev Wiki, "A20
// Line"). HIMEM.SYS tries it first (PC486_REVIEW.md §19.6).
// Commands: self-test, interface test, read/write command byte, enable/disable
// keyboard and AUX, write to AUX or either output buffer, read/write output
// port, pulse line 0. Typematic default 500 ms / 10.9 cps (set by 0xF3). LED
// state isn't modeled. IBM 5170 Technical Reference, "Keyboard System".
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

    // Port 0x92 shares the 8042 output port's A20 bit (one physical line), so it
    // reads/writes output_port_ bit 1 directly. Bit 0 is a write-only reset trigger
    // that self-clears and is not stored.
    bool owns_fast_a20(uint16_t port) const { return port == 0x92; }
    uint8_t fast_a20_in() const { return output_port_ & 0x02; }
    void fast_a20_out(uint8_t v) {
        output_port_ = uint8_t((output_port_ & ~0x02) | (v & 0x02));
        if (v & 0x01) reset_requested_ = true;
    }

    bool irq1_pending() const { return irq1_pending_; }
    void clear_irq1() { irq1_pending_ = false; }

    // IRQ12, the AUX line. Driven by the output buffer only while the byte came
    // from the mouse (status bit 5, AUXB) and command-byte bit 1 enables it.
    bool irq12_pending() const { return irq12_pending_; }
    void clear_irq12() { irq12_pending_ = false; }

    bool a20_enabled() const { return (output_port_ & 0x02) != 0; }

    bool reset_requested() const { return reset_requested_; }
    void clear_reset_request() { reset_requested_ = false; }

    // Delivers one Set 1 scan code to port 0x60. No-op while the keyboard is
    // disabled (0xAD). A real AT keyboard is Set-2-native and the 8042 translates
    // to Set 1, so this models the translated result; callers supply Set 1 codes.
    void inject_scancode(uint8_t code);

    // Typematic repeat against the CPU's absolute cycle count (like Pit8253::tick).
    // Only the most recent key repeats, until its break (IBM PS/2 Technical Reference
    // "Keyboard"; Chapweske "The AT-PS/2 Keyboard Interface").
    void tick(uint64_t cpu_cycles, double cpu_hz) {
        uint64_t d = cpu_cycles - prev_cycles_;
        prev_cycles_ = cpu_cycles;
        if (!tm_active_) return;
        now_s_ += double(d) / cpu_hz;
        if (now_s_ >= tm_next_) typematic_fire();
    }
    uint8_t typematic_byte() const { return typematic_; }
    bool typematic_active() const { return tm_active_; }

    // Button bits in PS/2 movement packet order (Chapweske)
    enum MouseButton : uint8_t {
        kMouseLeft = 0x01,
        kMouseRight = 0x02,
        kMouseMiddle = 0x04,
    };

    // One mouse sample. dx/dy are counts, +X right, +Y away from the user, so a
    // browser front end must negate dy (up one = 08 00 01, down one = 28 00 FF).
    // `buttons` is the full current state. Counts accumulate in the 9-bit counters
    // and clear only when a packet is sent; see mouse_reporting_enabled().
    void inject_mouse_event(int dx, int dy, uint8_t buttons);

    // True once a driver has the mouse in stream mode with reporting enabled
    // (0xF4) and the AUX clock is not held low
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
    uint8_t output_port_ = 0x00;  // bit 1 (A20) starts disabled
    bool kbd_enabled_ = true;
    bool system_flag_ = false;
    bool last_was_command_ = false;
    bool reset_requested_ = false;
    mutable bool irq1_pending_ = false;
    mutable bool irq12_pending_ = false;

    uint8_t typematic_ = 0x2B;   // 500 ms delay, 10.9 cps: the power-on default
    uint8_t kbd_arg_cmd_ = 0;    // a keyboard command (0xED/0xF0/0xF3) awaiting its argument
    bool tm_active_ = false;
    uint8_t tm_prefix_ = 0;      // 0xE0 for a grey key, else 0
    uint8_t tm_code_ = 0;
    bool pending_e0_ = false;
    int e1_skip_ = 0;            // Pause's E1 sequences never repeat
    uint64_t prev_cycles_ = 0;
    double now_s_ = 0.0;
    double tm_next_ = 0.0;
    double typematic_delay() const { return 0.25 * double(1 + ((typematic_ >> 5) & 3)); }
    double typematic_period() const {
        // (8 + A) * 2^B * 4.17 ms, A = bits 0-2, B = bits 3-4.
        return double(8 + (typematic_ & 7)) * double(1 << ((typematic_ >> 3) & 3)) * 0.00417;
    }
    void typematic_fire();

    // Output queue. The real buffer is one byte, but devices answer in bursts and
    // hold bytes behind the clock line, so none are lost. This FIFO stands in for
    // those holding buffers, each byte tagged with its source (status bit 5 and
    // IRQ). Oversized because enqueue() drops on overflow: a break code lost
    // during a mouse-look burst left a key stuck. Controller command answers use
    // push_ctrl() instead.
    // `irq`: IRQ1/IRQ12 are output-port lines (P24, P25) driven by firmware, not
    // OBF. Controller answers raise nothing, scan codes raise IRQ1, mouse bytes
    // raise IRQ12 (PC486_REVIEW.md §10).
    struct Queued { uint8_t value; bool aux; bool irq; };
    static constexpr int kQueueSize = 1024;
    mutable Queued queue_[kQueueSize] = {};
    mutable int queue_head_ = 0;
    mutable int queue_count_ = 0;

    // Mouse device state. Defaults are what its BAT loads (100 samples/s,
    // 4 counts/mm, 1:1, reporting off, stream mode; Chapweske "Reset Mode").
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

    // The 8042 answering for itself: written straight into the one-byte output
    // buffer over any unread byte, no interrupt, no queue. Callers poll.
    void push_ctrl(uint8_t v) const;
    // Every byte the keyboard sends (scan codes, ACK 0xFA, BAT 0xAA, ID 0xAB/0x83)
    // sets IBF and fires IRQ1 (Chapweske "AT-PS/2 Keyboard Interface", "Writing to
    // keyboard"). `irq` has no default so each call site says so.
    void push_kbd(uint8_t v, bool irq);
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
