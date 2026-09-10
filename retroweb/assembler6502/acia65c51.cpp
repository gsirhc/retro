// WDC W65C51 ACIA implementation. See acia65c51.h for the interface and
// citation.

#include "acia65c51.h"

namespace acia65c51 {

// STATUS register bits (WDC datasheet).
enum : uint8_t {
    ST_PE = 1 << 0, ST_FE = 1 << 1, ST_OVRN = 1 << 2, ST_RDRF = 1 << 3,
    ST_TDRE = 1 << 4, ST_DCD = 1 << 5, ST_DSR = 1 << 6, ST_IRQ = 1 << 7,
};

void Acia::reset() {
    status_ = ST_TDRE;   // DCD/DSR bits stay clear: both pins are grounded (asserted) -- see header
    cmd_ = 0;
    ctrl_ = 0;
    irq_ = false;
    rx_byte_ = 0;
    tx_cycles_left_ = 0;
    tx_pending_ = false;
}

// Baud-select nibble (CTRL bits 3-0) -> real bps off the 1.8432MHz crystal.
// WDC W65C51N datasheet, Table 4 "Baud Rate Generator".
static const int kBaudTable[16] = {
    0 /* external clock */, 50, 75, 110 /* 109.92 */, 135 /* 134.58 */, 150, 300, 600,
    1200, 1800, 2400, 3600, 4800, 7200, 9600, 19200,
};

int Acia::baud() const { return kBaudTable[ctrl_ & 0x0F]; }

int Acia::cycles_per_byte() const {
    int b = baud();
    if (b == 0) return 0;
    // 1 start + 8 data + 1 stop = 10 bit times, at 1 MHz Phi2. Word length
    // and parity/stop-bit configuration shift this slightly on real
    // hardware; 10 bits is this board's actual configuration (bios.s: 8-N-1)
    // and a reasonable approximation for others.
    return (10 * 1000000) / b;
}

void Acia::update_irq() {
    // IRQ bit in STATUS mirrors whatever's currently asserting the (open-
    // collector) IRQ output: RDRF with receiver interrupts enabled (CMD
    // bit1=0), or TDRE with transmit interrupts enabled (CMD bits3-2=01).
    bool rx_irq = (status_ & ST_RDRF) && !(cmd_ & 0x02);
    bool tx_irq = (status_ & ST_TDRE) && ((cmd_ & 0x0C) == 0x04);
    irq_ = rx_irq || tx_irq;
    status_ = irq_ ? uint8_t(status_ | ST_IRQ) : uint8_t(status_ & ~ST_IRQ);
}

uint8_t Acia::read(uint8_t reg) {
    switch (reg & 0x03) {
        case 0: {                              // DATA: reading clears RDRF
            uint8_t v = rx_byte_;
            status_ = uint8_t(status_ & ~ST_RDRF);
            update_irq();
            return v;
        }
        case 1: {                              // STATUS -- read has no side effect other than IRQ bit reflecting current state
            update_irq();
            return status_;
        }
        case 2: return cmd_;
        default: return ctrl_;
    }
}

void Acia::write(uint8_t reg, uint8_t v) {
    switch (reg & 0x03) {
        case 0:                                // DATA: begin transmit shift
            tx_byte_ = v;
            tx_pending_ = true;
            status_ = uint8_t(status_ & ~ST_TDRE);   // busy until the real bit time elapses -- see header
            tx_cycles_left_ = cycles_per_byte();
            update_irq();
            break;
        case 1:                                 // STATUS: any write is a soft reset of the status/error bits (datasheet)
            status_ = uint8_t(status_ & (ST_DCD | ST_DSR));
            status_ = uint8_t(status_ | ST_TDRE);
            update_irq();
            break;
        case 2: cmd_ = v; update_irq(); break;
        default: ctrl_ = v; break;
    }
}

void Acia::rx_push(uint8_t byte) {
    if (status_ & ST_RDRF) {                     // single holding register, no FIFO -- real 6551/65C51 behavior
        status_ = uint8_t(status_ | ST_OVRN);
        return;
    }
    rx_byte_ = byte;
    status_ = uint8_t(status_ | ST_RDRF);
    update_irq();
}

void Acia::tick(int n) {
    if (!tx_pending_) return;
    tx_cycles_left_ -= n;
    if (tx_cycles_left_ > 0) return;
    tx_pending_ = false;
    status_ = uint8_t(status_ | ST_TDRE);
    update_irq();
    if (on_tx) on_tx(tx_byte_);
}

} // namespace acia65c51
