#include "dma8237.h"

namespace ibmpcat {

void Dma8237::master_clear() {
    command_ = 0;
    request_ = 0;
    tc_ = 0;
    temp_ = 0;
    mask_ = 0x0F;
    flip_flop_ = false;
}

bool Dma8237::owns(uint16_t port) const {
    uint16_t span = uint16_t(16 * stride_);
    return port >= base_ && port < uint16_t(base_ + span);
}

void Dma8237::set_dreq(int channel, bool level) {
    uint8_t bit = uint8_t(1 << (channel & 3));
    dreq_ = level ? uint8_t(dreq_ | bit) : uint8_t(dreq_ & ~bit);
}

uint8_t Dma8237::in(uint16_t port) {
    int reg = (port - base_) / stride_;
    if (reg <= 7) {
        Channel& c = ch_[reg / 2];
        uint16_t val = (reg & 1) ? c.count : c.address;
        uint8_t byte = flip_flop_ ? uint8_t(val >> 8) : uint8_t(val & 0xFF);
        flip_flop_ = !flip_flop_;
        return byte;
    }
    switch (reg) {
        case 8: {
            uint8_t v = uint8_t(tc_ | ((dreq_ | request_) << 4));
            tc_ = 0;
            return v;
        }
        case 13: return temp_;
        default: return 0xFF;  // write-only registers, nothing drives the bus
    }
}

void Dma8237::out(uint16_t port, uint8_t v) {
    int reg = (port - base_) / stride_;
    if (reg <= 7) {
        Channel& c = ch_[reg / 2];
        uint16_t& cur = (reg & 1) ? c.count : c.address;
        uint16_t& base = (reg & 1) ? c.base_count : c.base_address;
        if (!flip_flop_) cur = uint16_t((cur & 0xFF00) | v);
        else cur = uint16_t((cur & 0x00FF) | (uint16_t(v) << 8));
        base = cur;
        flip_flop_ = !flip_flop_;
        return;
    }
    uint8_t bit = uint8_t(1 << (v & 3));
    switch (reg) {
        case 8: command_ = v; break;
        case 9: request_ = (v & 4) ? uint8_t(request_ | bit) : uint8_t(request_ & ~bit); break;
        case 10: mask_ = (v & 4) ? uint8_t(mask_ | bit) : uint8_t(mask_ & ~bit); break;
        case 11: ch_[v & 3].mode = v; break;
        case 12: flip_flop_ = false; break;
        case 13: master_clear(); break;
        case 14: mask_ = 0; break;
        case 15: mask_ = uint8_t(v & 0x0F); break;
        default: break;
    }
}

uint16_t Dma8237::transfer(int channel, bool* tc) {
    int idx = channel & 3;
    Channel& c = ch_[idx];
    uint16_t used = c.address;
    // The address counter is 16 bits; the page register never carries.
    c.address = uint16_t((c.mode & 0x20) ? c.address - 1 : c.address + 1);
    *tc = (c.count == 0);
    c.count = uint16_t(c.count - 1);
    if (*tc) {
        uint8_t bit = uint8_t(1 << idx);
        tc_ = uint8_t(tc_ | bit);
        request_ = uint8_t(request_ & ~bit);
        if (c.mode & 0x10) {
            c.address = c.base_address;
            c.count = c.base_count;
        } else {
            mask_ = uint8_t(mask_ | bit);
        }
    }
    return used;
}

}  // namespace ibmpcat
