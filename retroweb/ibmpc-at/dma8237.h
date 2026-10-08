// Intel 8237A DMA controller (Intel 8237A-5 data sheet, 231466).
//
// The AT has two cascaded: DMA1 (channels 0-3, 8-bit, ports 0x00-0x0F) carries
// the floppy on channel 2; DMA2 (channels 4-7, 16-bit, ports 0xC0-0xDF) takes
// DMA1's HRQ on its channel 0 (system channel 4). DMA2 registers sit on even
// ports (IBM PC/AT Technical Reference), modeled by `stride`. The page registers
// are separate 74LS612 glue, kept here per channel but untouched by master clear.
#ifndef IBMPCAT_DMA8237_H
#define IBMPCAT_DMA8237_H

#include <cstdint>

namespace ibmpcat {

class Dma8237 {
public:
    // `base` is 0x00 (DMA1) or 0xC0 (DMA2); `stride` is 1 or 2 respectively.
    Dma8237(uint16_t base, int stride) : base_(base), stride_(stride) {}

    // Hardware RESET and Master Clear: command, status, request, temporary and the
    // flip-flop clear, every mask bit sets. Address, count and mode survive.
    void master_clear();
    void reset() { master_clear(); }

    bool owns(uint16_t port) const;
    uint8_t in(uint16_t port);
    void out(uint16_t port, uint8_t v);

    void set_page(int channel, uint8_t v) { page_[channel & 3] = v; }
    uint8_t page(int channel) const { return page_[channel & 3]; }

    enum Type { kVerify = 0, kWrite = 1, kRead = 2, kIllegal = 3 };
    static constexpr int kCascadeMode = 3;

    uint16_t address(int channel) const { return ch_[channel & 3].address; }
    uint16_t count(int channel) const { return ch_[channel & 3].count; }
    uint8_t mode(int channel) const { return ch_[channel & 3].mode; }
    Type type(int channel) const { return Type((ch_[channel & 3].mode >> 2) & 3); }
    bool channel_masked(int channel) const { return (mask_ >> (channel & 3)) & 1; }
    bool disabled() const { return (command_ & 0x04) != 0; }

    // The device's DREQ line; shows in status bits 4-7.
    void set_dreq(int channel, bool level);
    // DREQ will be honoured: controller enabled and channel unmasked.
    bool can_service(int channel) const { return !disabled() && !channel_masked(channel); }

    // One transfer cycle on `channel`: returns the 16-bit address used, then steps
    // address and count. `*tc` reports terminal count (count 0000h -> FFFFh).
    uint16_t transfer(int channel, bool* tc);

private:
    struct Channel {
        uint16_t address = 0, count = 0;
        uint16_t base_address = 0, base_count = 0;
        uint8_t mode = 0;
    };
    Channel ch_[4];
    uint8_t page_[4] = {};
    uint8_t command_ = 0;
    uint8_t mask_ = 0x0F;
    uint8_t request_ = 0;
    uint8_t tc_ = 0;
    uint8_t dreq_ = 0;
    uint8_t temp_ = 0;
    bool flip_flop_ = false;  // false = next address/count byte is the low half
    uint16_t base_;
    int stride_;
};

}  // namespace ibmpcat

#endif  // IBMPCAT_DMA8237_H
