// MITS Altair 88-2SIO serial board.
//
// The 2SIO carries two Motorola 6850 ACIA channels. The Altair monitor / CP/M
// almost always uses channel A at ports 0x10 (control|status) and 0x11 (data);
// channel B sits at 0x12 / 0x13. Each channel is:
//
//   IN  base+0  -> status register   (RDRF, TDRE, overrun, ...)
//   OUT base+0  -> control register   (clock divide, word fmt, irq enable)
//   IN  base+1  -> receive data       (pops a byte the host sent us)
//   OUT base+1  -> transmit data      (pushes a byte toward the host)
//
// Both data directions pass through ring buffers so a front end (a websocket
// bridge, a PTY, a test driver) can fill the receive side and drain the
// transmit side asynchronously.

#ifndef EMULATOR8080_SERIAL2SIO_H
#define EMULATOR8080_SERIAL2SIO_H

#include "ringbuffer.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace altair {

// 6850 status-register bits (read at control|status port).
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
    // Depth of each direction's FIFO. 6850 hardware is single-byte; the extra
    // buffering just decouples the emulated CPU from the host I/O rate.
    static constexpr std::size_t kFifoDepth = 512;

    struct Channel {
        RingBuffer<kFifoDepth> rx;   // host -> CPU  (read via data-in port)
        RingBuffer<kFifoDepth> tx;   // CPU  -> host (written via data-out port)
        uint8_t control = 0;
        bool    overrun = false;
        bool    rx_irq_enabled = false;
        bool    tx_irq_enabled = false;
    };

    // `base_a` is channel A's control|status port; data is base_a+1.
    // Channel B follows at base_a+2 / base_a+3 (the physical 2SIO layout).
    explicit Serial2SIO(uint8_t base_a = 0x10) : base_(base_a) {}

    // Fires when the board asserts its interrupt line (needs wiring to
    // Cpu::interrupt with the machine's RST vector, typically RST 7).
    std::function<void()> on_irq;

    bool owns(uint8_t port) const { return (port & 0xFC) == (base_ & 0xFC); }

    // 8080 bus hooks — wire these to i8080::Bus::in / ::out.
    uint8_t in(uint8_t port);
    void    out(uint8_t port, uint8_t value);

    // ---- host / front-end side ------------------------------------------
    // Feed a byte the terminal typed toward the CPU. Returns false if the
    // receive FIFO is full (the channel latches an overrun).
    bool host_send(uint8_t byte, int channel = 0);
    std::size_t host_send(const std::string &s, int channel = 0);

    // Pull one byte the CPU transmitted. Returns false when nothing is queued.
    bool host_recv(uint8_t &out, int channel = 0);
    // Drain everything the CPU has transmitted since the last call.
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
