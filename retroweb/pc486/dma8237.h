// Intel 8237A DMA controller.
// The AT has two, cascaded: DMA1 (channels 0-3, 8-bit, ports 0x00-0x0F) and
// DMA2 (channels 4-7, 16-bit, ports 0xC0-0xDF) cascading into DMA1 channel 4.
// DMA2 registers sit one address bit to the left, so spacing is 2 ports
// instead of 1 (the `stride` parameter). Page registers are separate glue
// (IBM 5170 Technical Reference). Intel 8237A-5 data sheet, "Programming".
#ifndef PC486_DMA8237_H
#define PC486_DMA8237_H

#include <cstdint>

namespace pc486 {

class Dma8237 {
public:
    // `base` is 0x00 (DMA1) or 0xC0 (DMA2); `stride` is 1 or 2 respectively.
    Dma8237(uint16_t base, int stride) : base_(base), stride_(stride) {}

    void reset();

    bool owns(uint16_t port) const;
    uint8_t in(uint16_t port);
    void out(uint16_t port, uint8_t v);

    // Page registers (address bits 16-23) are separate glue; chipset.h owns the decode
    void set_page(int channel, uint8_t v) { page_[channel & 3] = v; }
    uint8_t page(int channel) const { return page_[channel & 3]; }

    bool channel_masked(int channel) const { return ch_[channel & 3].masked; }

    // advance() moves one byte. The address wraps within the 64KB page and
    // count is N-1, so it returns true when count was 0 (TC on 0x0000 -> 0xFFFF).
    // chipset.cpp orchestrates the device-to-memory move.
    uint16_t address(int channel) const { return ch_[channel & 3].address; }
    uint16_t count(int channel) const { return ch_[channel & 3].count; }
    bool advance(int channel);

    // DREQ is a live line: chipset.cpp sets it every tick from each device,
    // regardless of the mask bit. Status bits 4-7 OR it with the software
    // request (out(9)), unlatched, per the 8237A-5 "channel request" bit.
    void set_dreq(int channel, bool asserted) { dreq_[channel & 3] = asserted; }

private:
    struct Channel {
        uint16_t address = 0, count = 0;
        // Shadow base registers latched on the same write as address/count.
        // Autoinitialize (mode bit 4) reloads current from these at TC (8237A-5
        // "Autoinitialize"); SB16 auto-init playback needs it.
        uint16_t base_address = 0, base_count = 0;
        uint8_t mode = 0;
        bool masked = true;  // real chips power on with every channel masked
    };
    Channel ch_[4];
    uint8_t page_[4] = {};
    uint8_t command_ = 0;
    // Software Request Register (out(9)): bits 0-1 select the channel, bit 2
    // sets/resets its request bit. 8237A-5 "Request Register".
    bool soft_request_[4] = {};
    bool dreq_[4] = {};        // live hardware DREQ per channel, see set_dreq()
    // TC latch per channel (status bits 0-3): set by advance(), cleared by a
    // status read and by master reset (8237A-5 "Status Register")
    bool tc_latch_[4] = {};
    bool flip_flop_ = false;  // false = next address/count byte is the low half
    uint16_t base_;
    int stride_;
};

}  // namespace pc486

#endif  // PC486_DMA8237_H
