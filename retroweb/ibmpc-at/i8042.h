// Intel 8042 keyboard controller, AT wiring.
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
// Scope: enough controller-command coverage (self-test, interface-test,
// read/write command byte, enable/disable keyboard, read/write output
// port, pulse-output-line-0) for BIOS POST to complete and for a keyboard
// driver to program the controller; LED state and typematic rate aren't
// modeled (any command byte the keyboard itself doesn't specifically need
// just gets ACKed). Reference: IBM 5170 Technical Reference, "Keyboard
// System"; the 8042 command set is otherwise identical across the whole
// PC/AT-compatible universe and is documented in any AT-class BIOS's
// keyboard POST routine.
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

    bool reset_requested() const { return reset_requested_; }
    void clear_reset_request() { reset_requested_ = false; }

    // Host/front-end side: deliver one Set-2 scan code from the physical or
    // virtual keyboard, as if a key event just happened. No-op while the
    // controller has the keyboard disabled (0xAD).
    void inject_scancode(uint8_t code);

private:
    enum class NextWrite { kNone, kCommandByte, kOutputPort };
    NextWrite next_write_ = NextWrite::kNone;

    mutable uint8_t output_buf_ = 0;
    mutable bool output_full_ = false;
    uint8_t command_byte_ = 0x00;
    uint8_t output_port_ = 0x00;  // bit1 (A20) starts disabled -- see file header
    bool kbd_enabled_ = true;
    bool system_flag_ = false;
    bool last_was_command_ = false;
    bool reset_requested_ = false;
    mutable bool irq1_pending_ = false;
    // A real keyboard responds to an explicit reset command (0xFF) with an
    // immediate ACK (0xFA), then performs its own self-test and reports
    // 0xAA (Basic Assurance Test passed) as a second, separate byte --
    // this flags that the next read of the data port should queue that
    // follow-up byte. See IBM_PCAT_REVIEW.md for the real BIOS source that
    // pinned this sequence down.
    mutable bool pending_bat_after_ack_ = false;

    void push_output(uint8_t v) { output_buf_ = v; output_full_ = true; }
};

}  // namespace ibmpcat

#endif  // IBMPCAT_I8042_H
