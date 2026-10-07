// Roland MPU-401 MIDI interface on the Sound Blaster 16. Decoded at its own
// jumper-selected base (300h or 330h, default 330h; SBPG Appendix A Table A-16),
// not inside the card's 0x220 block. Programmed per Creative's SBPG ch.5.
//   base+0h  Data port    read/write
//   base+1h  Status port  read only; Command port write only
// Status bits are active-high for not-ready:
//   bit 6 (40h)  set = not ready for output
//   bit 7 (80h)  set = no input data available
// UART mode only. Two commands: 0FFh Reset (also exits UART mode) and 03Fh
// Enter UART mode. Each makes a Command Acknowledge (0FEh) readable at the
// data port; others are ignored. Write 0FFh and expect 0FEh is SBPG's detection probe.
// Nothing is attached to MIDI OUT, so written bytes are discarded and no
// input ever arrives, which lets a game's MPU-401 detection finish. The IRQ
// shares the SB16's line (mixer reg 82h bit 2) and never fires.
#ifndef PC486_MPU401_H
#define PC486_MPU401_H

#include <cstdint>

namespace pc486 {

class Mpu401 {
public:
    // Jumper-selectable: 300h or 330h (SBPG Appendix A Table A-16)
    static constexpr uint16_t kDefaultBase = 0x330;

    explicit Mpu401(uint16_t base = kDefaultBase) : base_(base) { reset(); }

    void reset();

    bool owns(uint16_t port) const { return port == base_ || port == uint16_t(base_ + 1); }
    uint8_t in(uint16_t port);
    void out(uint16_t port, uint8_t v);

    bool uart_mode() const { return uart_mode_; }

    bool irq_pending() const { return false; }

private:
    uint16_t base_;
    bool uart_mode_ = false;
    bool ack_pending_ = false;
};

}  // namespace pc486

#endif  // PC486_MPU401_H
