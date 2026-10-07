// MITS Altair 88-2SIO: two Motorola 6850 ACIA channels, A at 0x10 (control|status)
// / 0x11 (data), B at 0x12 / 0x13. Ring buffers decouple the host from the CPU.

#ifndef EMULATOR8080_SERIAL2SIO_H
#define EMULATOR8080_SERIAL2SIO_H

#include "ringbuffer.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace altair {

enum AciaStatus : uint8_t {
    ACIA_RDRF = 1 << 0,   // receive data register full  -> CPU has a byte to read
    ACIA_TDRE = 1 << 1,   // transmit data register empty -> CPU may write a byte
    ACIA_DCD  = 1 << 2,   // data carrier detect (held low/asserted here)
    ACIA_CTS  = 1 << 3,   // clear to send        (held low/asserted here)
    ACIA_FE   = 1 << 4,   // framing error
    ACIA_OVRN = 1 << 5,   // receiver overrun (a byte arrived while RDRF still set)
    ACIA_PE   = 1 << 6,   // parity error
    ACIA_IRQ  = 1 << 7,   // interrupt request pending
};

class Serial2SIO {
public:
    // 6850 hardware is single-byte; the extra depth decouples the CPU from host I/O
    static constexpr std::size_t kFifoDepth = 512;

    struct Channel {
        RingBuffer<kFifoDepth> rx;   // host -> CPU  (read via data-in port)
        RingBuffer<kFifoDepth> tx;   // CPU  -> host (written via data-out port)
        uint8_t control = 0;
        bool    overrun = false;
        bool    rx_irq_enabled = false;
        bool    tx_irq_enabled = false;
    };

    // base_a is channel A's control|status port; B follows at base_a+2
    explicit Serial2SIO(uint8_t base_a = 0x10) : base_(base_a) {}

    // fires when the board asserts IRQ; wire to Cpu::interrupt (typically RST 7)
    std::function<void()> on_irq;

    bool owns(uint8_t port) const { return (port & 0xFC) == (base_ & 0xFC); }

    uint8_t in(uint8_t port);
    void    out(uint8_t port, uint8_t value);

    // ---- host / front-end side ------------------------------------------
    bool host_send(uint8_t byte, int channel = 0);
    std::size_t host_send(const std::string &s, int channel = 0);

    bool host_recv(uint8_t &out, int channel = 0);
    std::vector<uint8_t> host_drain(int channel = 0);

    std::size_t rx_pending(int channel = 0) const { return ch_[idx(channel)].rx.size(); }
    std::size_t tx_pending(int channel = 0) const { return ch_[idx(channel)].tx.size(); }

    void reset();

private:
    static int  idx(int channel) { return channel & 1; }
    int         channel_for(uint8_t port) const { return (port >= base_ + 2) ? 1 : 0; }

    uint8_t status(const Channel &c) const;
    void    write_control(Channel &c, uint8_t value);
    void    refresh_irq();

    uint8_t base_;
    Channel ch_[2];
};

} // namespace altair

#endif // EMULATOR8080_SERIAL2SIO_H
