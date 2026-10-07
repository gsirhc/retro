#include "dma8237.h"

namespace pc486 {

void Dma8237::reset() {
    for (auto &c : ch_) c = Channel{};
    for (auto &p : page_) p = 0;
    command_ = 0;
    for (auto &r : soft_request_) r = false;
    for (auto &d : dreq_) d = false;
    for (auto &t : tc_latch_) t = false;
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
        case 8: {
            // Status (8237A-5): bits 0-3 are TC latches set by advance() and cleared
            // by this read; bits 4-7 are live DREQ ORed with the software request, not latched
            uint8_t v = 0;
            for (int i = 0; i < 4; ++i) {
                if (tc_latch_[i]) v = uint8_t(v | (1 << i));
                if (dreq_[i] || soft_request_[i]) v = uint8_t(v | (1 << (4 + i)));
            }
            for (auto &t : tc_latch_) t = false;
            return v;
        }
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
        uint16_t &base = is_count ? ch_[ch].base_count : ch_[ch].base_address;
        if (!flip_flop_) { val = uint16_t((val & 0xFF00) | v); base = uint16_t((base & 0xFF00) | v); }
        else { val = uint16_t((val & 0x00FF) | (uint16_t(v) << 8)); base = uint16_t((base & 0x00FF) | (uint16_t(v) << 8)); }
        flip_flop_ = !flip_flop_;
        return;
    }
    switch (reg) {
        case 8: command_ = v; break;
        case 9: soft_request_[v & 3] = (v & 4) != 0; break;  // Request Register: ch select + set/reset bit
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
    if (tc) tc_latch_[channel & 3] = true;
    if (tc && (c.mode & 0x10) != 0) {
        // Autoinitialize reloads from the base registers
        c.address = c.base_address;
        c.count = c.base_count;
    } else {
        // 8237A-5 "Mode Register": bit 5 selects decrement. The uint16_t address
        // wraps within the 64KB page, as on the real chip.
        if ((c.mode & 0x20) != 0) c.address = uint16_t(c.address - 1);
        else c.address = uint16_t(c.address + 1);
        c.count = uint16_t(c.count - 1);
    }
    return tc;
}

}  // namespace pc486
