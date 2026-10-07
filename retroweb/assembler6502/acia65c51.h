// WDC W65C51N ACIA, U7. Registers at $5000-$5003 (mirrored through $5FFF):
// DATA, STATUS, CMD, CTRL. Datasheet:
// https://www.westerndesigncenter.com/wdc/documentation/w65c51n.pdf
//
// CMOS part, so the baud generator is accurate (no NMOS 6551 erratum). The
// 1.8432 MHz crystal (Y1) feeds it directly, so CTRL's baud bits give a real
// rate that baud() exposes for the terminal to meter against.
//
// ~DTR, ~DCD, ~DSR and ~CTS are tied to GND per the netlist: DCD/DSR read
// asserted and the chip is always clear-to-send.

#ifndef CG_OAC_6502_ACIA65C51_H
#define CG_OAC_6502_ACIA65C51_H

#include <cstdint>
#include <functional>

namespace acia65c51 {

class Acia {
public:
    // Fires when a byte finishes shifting out at the configured baud rate.
    // TDRE stays busy for the real bit time, which rom/bios.s:207's delay
    // loop works around.
    std::function<void(uint8_t)> on_tx;

    bool irq() const { return irq_; }

    uint8_t read(uint8_t reg);
    void write(uint8_t reg, uint8_t v);

    // Single receive holding register, no FIFO: if RDRF is still set the
    // new byte is dropped and OVRN is raised.
    void rx_push(uint8_t byte);

    // Advance `n` Phi2 cycles; paces the transmit shift.
    void tick(int n);

    // bps selected by CTRL's baud bits (datasheet Table 4). 0 = external
    // receiver clock, undriven on this board, so no shift delay.
    int baud() const;

    void reset();

private:
    uint8_t status_ = 0x10;   // TDRE set (ready) at reset
    uint8_t cmd_ = 0, ctrl_ = 0;
    bool irq_ = false;

    uint8_t rx_byte_ = 0;
    int tx_cycles_left_ = 0;
    uint8_t tx_byte_ = 0;
    bool tx_pending_ = false;

    void update_irq();
    int cycles_per_byte() const;
};

} // namespace acia65c51

#endif // CG_OAC_6502_ACIA65C51_H
