// Intel 8237A DMA controller -- register file only (this phase).
//
// The AT has two, cascaded: DMA1 (channels 0-3, 8-bit, ports 0x00-0x0F,
// plus page registers on separate glue-decoded ports) handles the floppy
// on channel 2; DMA2 (channels 4-7, 16-bit, ports 0xC0-0xDF) cascades into
// DMA1's channel 4 and isn't used by anything this machine emulates yet.
// DMA2's registers are wired one address bit to the left of DMA1's (a real,
// documented AT quirk: A0 isn't decoded into the chip, only into which
// nibble of the 16-bit bus carries the byte), so its register spacing is 2
// ports apart instead of 1 -- modeled via the `stride` constructor
// parameter rather than two near-duplicate classes.
//
// Scope: address/count/mode/mask/command register read-write is complete
// enough for a BIOS's POST-time DMA controller check to program and read
// back without hanging. Actual memory<->device byte transfer (driven by a
// device's DREQ, e.g. the floppy controller pulling bytes over channel 2)
// is NOT implemented here -- that lands in Phase 3 alongside fdc765.h,
// which is the first device that actually needs it. Reference: Intel
// 8237A-5 data sheet, "Programming the DMA Controller"; the AT's page-
// register wiring (separate 74LS670-family glue, not part of the 8237
// itself) is documented in the IBM 5170 Technical Reference.
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

    // Page registers (address bits 16-23 for each channel) live on a
    // separate port range decoded by different glue logic, not the 8237
    // itself -- chipset.h owns that decode and calls these directly.
    void set_page(int channel, uint8_t v) { page_[channel & 3] = v; }
    uint8_t page(int channel) const { return page_[channel & 3]; }

    bool channel_masked(int channel) const { return ch_[channel & 3].masked; }

    // Live-transfer support (Phase 3, for fdc765.h): the programmed
    // address/count for a channel, and advance() to move one byte --
    // wraps the address within the 64KB page (a real, documented 8237
    // quirk: a transfer never carries across a page-register boundary,
    // it just wraps) and decrements count, returning true if count was 0
    // before the decrement (terminal count -- real hardware programs
    // count as N-1 and detects TC on the 0x0000 -> 0xFFFF underflow).
    // `chipset.cpp` orchestrates the actual byte move between a device
    // and memory using these, the same way it orchestrates the PIC
    // master/slave cascade -- devices don't reach into each other
    // directly.
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
