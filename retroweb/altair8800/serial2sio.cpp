#include "serial2sio.h"

namespace altair {

void Serial2SIO::reset() {
    for (Channel &c : ch_) {
        c.rx.clear();
        c.tx.clear();
        c.control = 0;
        c.overrun = false;
        c.rx_irq_enabled = false;
        c.tx_irq_enabled = false;
    }
}

uint8_t Serial2SIO::status(const Channel &c) const {
    uint8_t s = 0;
    if (!c.rx.empty()) s |= ACIA_RDRF;
    if (!c.tx.full())  s |= ACIA_TDRE;       // buffered: room means "ready"
    if (c.overrun)     s |= ACIA_OVRN;
    // DCD / CTS are active-low on the 6850; a ready line reads 0, so leave them.
    bool irq = (c.rx_irq_enabled && (s & ACIA_RDRF)) ||
               (c.tx_irq_enabled && (s & ACIA_TDRE));
    if (irq) s |= ACIA_IRQ;
    return s;
}

void Serial2SIO::write_control(Channel &c, uint8_t value) {
    c.control = value;

    // Counter-divide field = 0b11 is the ACIA master reset.
    if ((value & 0x03) == 0x03) {
        c.overrun = false;
        c.rx_irq_enabled = false;
        c.tx_irq_enabled = false;
        return;
    }

    c.rx_irq_enabled = (value & 0x80) != 0;          // bit 7: receive IRQ enable
    c.tx_irq_enabled = (value & 0x60) == 0x20;       // bits 6-5 == 01: transmit IRQ enable
}

void Serial2SIO::refresh_irq() {
    bool asserted = false;
    for (const Channel &c : ch_)
        if (status(c) & ACIA_IRQ) asserted = true;
    if (asserted && on_irq) on_irq();
}

// --- 8080 bus side --------------------------------------------------------

uint8_t Serial2SIO::in(uint8_t port) {
    if (!owns(port)) return 0xFF;
    Channel &c = ch_[channel_for(port)];
    bool data_port = ((port - base_) & 1) != 0;

    if (!data_port)
        return status(c);

    uint8_t v = 0;
    if (c.rx.pop(v)) {
        c.overrun = false;      // 6850: reading RDR clears the overrun latch
    }
    return v;
}

void Serial2SIO::out(uint8_t port, uint8_t value) {
    if (!owns(port)) return;
    Channel &c = ch_[channel_for(port)];
    bool data_port = ((port - base_) & 1) != 0;

    if (!data_port) {
        write_control(c, value);
        return;
    }

    // Transmit: hand the byte to the front end. If the FIFO is somehow full
    // we drop it, mirroring a real overrun on the wire.
    c.tx.push(value);
    refresh_irq();
}

// --- host / front-end side ----------------------------------------------

bool Serial2SIO::host_send(uint8_t byte, int channel) {
    Channel &c = ch_[idx(channel)];
    if (!c.rx.push(byte)) {
        c.overrun = true;       // byte arrived with the receive FIFO full
        return false;
    }
    if (c.rx_irq_enabled && on_irq) on_irq();
    return true;
}

std::size_t Serial2SIO::host_send(const std::string &s, int channel) {
    std::size_t n = 0;
    for (unsigned char ch : s) {
        if (!host_send(static_cast<uint8_t>(ch), channel)) break;
        ++n;
    }
    return n;
}

bool Serial2SIO::host_recv(uint8_t &out, int channel) {
    return ch_[idx(channel)].tx.pop(out);
}

std::vector<uint8_t> Serial2SIO::host_drain(int channel) {
    std::vector<uint8_t> bytes;
    Channel &c = ch_[idx(channel)];
    uint8_t v = 0;
    while (c.tx.pop(v)) bytes.push_back(v);
    return bytes;
}

} // namespace altair
