#include "dma8237.h"

namespace ibmpcat {

void Dma8237::reset() {
    for (auto &c : ch_) c = Channel{};
    for (auto &p : page_) p = 0;
    command_ = 0;
    request_ = 0;
    flip_flop_ = false;
}

bool Dma8237::owns(uint16_t port) const {
    uint16_t span = uint16_t(16 * stride_);
    return port >= base_ && port < uint16_t(base_ + span);
}

uint8_t Dma8237::in(uint16_t port) {
    int reg = (port - base_) / stride_;
    if (reg <= 7) {
        int ch = reg / 2;
        bool is_count = (reg & 1) != 0;
        uint16_t &val = is_count ? ch_[ch].count : ch_[ch].address;
        uint8_t byte = flip_flop_ ? uint8_t(val >> 8) : uint8_t(val & 0xFF);
        flip_flop_ = !flip_flop_;
        return byte;
    }
    switch (reg) {
        case 8: return command_;  // status register on read -- TC/request bits not modeled, see file header
        case 15: {
            uint8_t m = 0;
            for (int i = 0; i < 4; ++i)
                if (ch_[i].masked) m = uint8_t(m | (1 << i));
            return m;
        }
        default: return 0xFF;
    }
}

void Dma8237::out(uint16_t port, uint8_t v) {
    int reg = (port - base_) / stride_;
    if (reg <= 7) {
        int ch = reg / 2;
        bool is_count = (reg & 1) != 0;
        uint16_t &val = is_count ? ch_[ch].count : ch_[ch].address;
        if (!flip_flop_) val = uint16_t((val & 0xFF00) | v);
        else val = uint16_t((val & 0x00FF) | (uint16_t(v) << 8));
        flip_flop_ = !flip_flop_;
        return;
    }
    switch (reg) {
        case 8: command_ = v; break;
        case 9: request_ = v; break;
        case 10: { int ch = v & 3; ch_[ch].masked = (v & 4) != 0; break; }              // single mask
        case 11: { int ch = v & 3; ch_[ch].mode = v; break; }                            // mode
        case 12: flip_flop_ = false; break;                                             // clear byte pointer
        case 13: reset(); break;                                                        // master clear
        case 14: for (auto &c : ch_) c.masked = false; break;                           // clear mask register
        case 15: for (int i = 0; i < 4; ++i) ch_[i].masked = (v & (1 << i)) != 0; break; // write all-channels mask
        default: break;
    }
}

bool Dma8237::advance(int channel) {
    Channel &c = ch_[channel & 3];
    bool tc = (c.count == 0);
    c.address = uint16_t(c.address + 1);
    c.count = uint16_t(c.count - 1);
    return tc;
}

}  // namespace ibmpcat
