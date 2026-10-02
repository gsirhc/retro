// Roland MPU-401 MIDI interface, as carried on the Sound Blaster 16.
//
// This interface physically lives on the SB16 card (it is not a separate
// board), but it is decoded at its own, independently jumper-selected base
// I/O address -- 300h or 330h, factory default 330h -- not inside the
// card's own 0x220-0x22F block. Everything here is programmed exactly as
// Creative's "Sound Blaster Series Hardware Programming Guide" (SBPG)
// chapter 5 ("MIDI Port I/O Programming") and Appendix A Table A-16
// describe it:
//
//   base+0h  Data port    read/write
//   base+1h  Status port  read only; Command port write only
//
// Status bits are active-high for NOT-ready, per SBPG chapter 5's own
// example polling code:
//   bit 6 (40h)  set = interface not ready for output   ("test al,40h / jnz")
//   bit 7 (80h)  set = no input data available           ("test al,80h / jnz")
//
// This card supports UART mode only -- no Intelligent mode, no MPU-401
// command set beyond the two below -- so only two command bytes are
// recognized:
//   0FFh  Reset        (also exits UART mode)
//   03Fh  Enter UART mode
// Either one makes a Command Acknowledge byte (0FEh) available at the data
// port; any other command byte is ignored and produces no acknowledge.
// Reset doubling as the documented detection probe (write 0FFh, expect to
// read back 0FEh; anything else means no MPU-401 at that address) is SBPG
// chapter 5's own recommended detection sequence.
//
// In UART mode, bytes written to the data port go out the card's MIDI OUT
// jack. Nothing is attached here (no Wave Blaster daughterboard, no
// external synth), so this model accepts and discards every written byte,
// and no MIDI input ever arrives -- the status port's input-not-available
// bit stays set forever. That is exactly what a real SB16 with nothing
// plugged into its MIDI port does, and it's the whole point of modeling
// this at all: a game set to "General MIDI / MPU-401" now completes its
// detection handshake and plays to silence instead of hanging on a
// detection timeout.
//
// The MPU-401's interrupt shares the SB16's one IRQ line and has its own
// mixer status bit (register 82h bit 2, SBPG chapter 4). With no MIDI
// input ever pending there is never anything to raise it for, so
// irq_pending() always reads false -- kept for symmetry with the other
// device classes rather than left unmodeled.
#ifndef PC486_MPU401_H
#define PC486_MPU401_H

#include <cstdint>

namespace pc486 {

class Mpu401 {
public:
    // Jumper-selectable per SBPG Appendix A Table A-16: 300h or 330h,
    // factory default 330h.
    static constexpr uint16_t kDefaultBase = 0x330;

    explicit Mpu401(uint16_t base = kDefaultBase) : base_(base) { reset(); }

    // Power-on/reset: no acknowledge pending, UART mode off.
    void reset();

    bool owns(uint16_t port) const { return port == base_ || port == uint16_t(base_ + 1); }
    uint8_t in(uint16_t port);
    void out(uint16_t port, uint8_t v);

    bool uart_mode() const { return uart_mode_; }

    // Never fires -- see the file header.
    bool irq_pending() const { return false; }

private:
    uint16_t base_;
    bool uart_mode_ = false;
    bool ack_pending_ = false;
};

}  // namespace pc486

#endif  // PC486_MPU401_H
