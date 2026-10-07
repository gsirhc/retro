// Intel 8237A DMA controller, register file plus single-byte advance.
//
// The AT has two cascaded: DMA1 (channels 0-3, 8-bit, ports 0x00-0x0F) carries
// the floppy on channel 2; DMA2 (channels 4-7, 16-bit, ports 0xC0-0xDF) is
// unused here. DMA2 registers sit on even ports (AT quirk), modeled by `stride`.
// Page registers are separate glue logic (IBM 5170 Technical Reference);
// 8237A-5 data sheet for the rest.
#ifndef IBMPCAT_DMA8237_H
#define IBMPCAT_DMA8237_H

#include <cstdint>

namespace ibmpcat {

class Dma8237 {
public:
    // `base` is 0x00 (DMA1) or 0xC0 (DMA2); `stride` is 1 or 2 respectively.
    Dma8237(uint16_t base, int stride) : base_(base), stride_(stride) {}

    void reset();

    bool owns(uint16_t port) const;
    uint8_t in(uint16_t port);
    void out(uint16_t port, uint8_t v);

    // Page registers (address bits 16-23) are decoded by chipset.h.
    void set_page(int channel, uint8_t v) { page_[channel & 3] = v; }
    uint8_t page(int channel) const { return page_[channel & 3]; }

    bool channel_masked(int channel) const { return ch_[channel & 3].masked; }

    // Programmed address/count per channel. advance() moves one byte, wrapping
    // the address within the 64KB page, and returns true on terminal count
    // (count is programmed as N-1; TC is the 0x0000 -> 0xFFFF underflow).
    uint16_t address(int channel) const { return ch_[channel & 3].address; }
    uint16_t count(int channel) const { return ch_[channel & 3].count; }
    bool advance(int channel);

private:
    struct Channel {
        uint16_t address = 0, count = 0;
        uint8_t mode = 0;
        bool masked = true;  // real chips power on with every channel masked
    };
    Channel ch_[4];
    uint8_t page_[4] = {};
    uint8_t command_ = 0;
    uint8_t request_ = 0;
    bool flip_flop_ = false;  // false = next address/count byte is the low half
    uint16_t base_;
    int stride_;
};

}  // namespace ibmpcat

#endif  // IBMPCAT_DMA8237_H
