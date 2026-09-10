// WDC W65C51 ACIA (Asynchronous Communications Interface Adapter), U7 on
// the board. Register map ($5000-$5003, mirrored across $5000-$5FFF):
// DATA, STATUS, CMD, CTRL. Cite: WDC W65C51N datasheet
// (https://www.westerndesigncenter.com/wdc/documentation/w65c51n.pdf).
//
// This board's part is confirmed (from pcb6502full.net's libsource) to be
// the CMOS W65C51N, not the NMOS MOS 6551 -- the CMOS part's baud-rate
// generator is accurate (the NMOS 6551's is a well-known erratum some
// emulators reproduce; not appropriate here). The 1.8432 MHz crystal (Y1)
// feeds the on-chip baud generator directly, so CTRL's baud-select bits
// select a *real* bit rate this core exposes via baud() for the terminal
// to meter output against live, rather than a fixed per-profile rate.
//
// Handshake pin tie-offs, confirmed from the netlist: ~DTR, ~DCD, ~DSR and
// ~CTS are all hard-wired to GND, so DCD/DSR always read "asserted" in
// STATUS and the chip is permanently clear-to-send -- there is no incoming
// hardware flow control on this board by default (see J8 in the review
// doc for the optional RTS-out jumper).

#ifndef CG_OAC_6502_ACIA65C51_H
#define CG_OAC_6502_ACIA65C51_H

#include <cstdint>
#include <functional>

namespace acia65c51 {

class Acia {
public:
    // Fires once a transmitted byte has actually finished shifting out at
    // the configured baud rate (see tick()) -- the ACIA's real behavior,
    // not an instant callback, is what rom/bios.s:207's "delay loop for
    // known ACIA bug not updating status register" is empirically working
    // around: TDRE genuinely stays busy for the real bit time of a byte,
    // it isn't a separate glitch needing a magic constant.
    std::function<void(uint8_t)> on_tx;

    bool irq() const { return irq_; }

    uint8_t read(uint8_t reg);
    void write(uint8_t reg, uint8_t v);

    // Host delivers a received byte (e.g. a keypress from the terminal).
    // The W65C51 has a single receive holding register, no FIFO: if RDRF
    // is still set (the CPU hasn't read the last byte), the new byte is
    // dropped and OVRN is raised -- the real chip's behavior, not a
    // simplification. Combined with ~CTS being hard-tied low on this
    // board (see file header), there is no hardware back-pressure, so a
    // fast paste really would lose bytes on the real hardware too.
    void rx_push(uint8_t byte);

    // Advance by `n` Phi2 cycles -- paces the in-flight transmit shift.
    void tick(int n);

    // Real bits-per-second selected by CTRL's baud bits, off the 1.8432MHz
    // crystal (WDC datasheet Table 4). 0 = external receiver clock (CTRL
    // bits=0000) -- not driven on this board, so treated as "no shift
    // delay" rather than modeling an unconnected clock source.
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
