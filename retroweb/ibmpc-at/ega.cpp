#include "ega.h"

namespace ibmpcat {

void Ega::reset() {
    crtc_.fill(0); crtc_index_ = 0;
    sequencer_.fill(0); sequencer_index_ = 0;
    gfx_.fill(0); gfx_index_ = 0;
    attr_.fill(0); attr_index_ = 0; attr_flip_flop_addr_ = true;
    misc_output_ = 0;
    retrace_ = false;
    prev_cycles_ = 0;
    retrace_credit_ = 0.0;
    // vram deliberately NOT cleared -- matches real hardware (contents
    // survive a controller reset; software clears it explicitly if it
    // wants a blank screen, typically as part of a mode set).
}

bool Ega::owns_port(uint16_t port) const {
    switch (port) {
        case 0x3C0: case 0x3C1: case 0x3C2: case 0x3C4: case 0x3C5:
        case 0x3CC: case 0x3CE: case 0x3CF: case 0x3D4: case 0x3D5: case 0x3DA:
            return true;
        default: return false;
    }
}

uint8_t Ega::in(uint16_t port) {
    switch (port) {
        case 0x3DA: {
            attr_flip_flop_addr_ = true;  // reading Input Status 1 resets the AC address/data flip-flop
            return retrace_ ? 0x08 : 0x00;
        }
        case 0x3C0: return attr_flip_flop_addr_ ? attr_index_ : uint8_t(0xFF);
        case 0x3C1: return attr_[attr_index_ % attr_.size()];
        case 0x3C2: return 0x00;  // Input Status 0 -- not modeled meaningfully
        case 0x3C4: return sequencer_index_;
        case 0x3C5: return sequencer_[sequencer_index_ % sequencer_.size()];
        case 0x3CC: return misc_output_;
        case 0x3CE: return gfx_index_;
        case 0x3CF: return gfx_[gfx_index_ % gfx_.size()];
        case 0x3D4: return crtc_index_;
        case 0x3D5: return crtc_[crtc_index_ % crtc_.size()];
        default: return 0xFF;
    }
}

void Ega::out(uint16_t port, uint8_t v) {
    switch (port) {
        case 0x3C0:
            if (attr_flip_flop_addr_) attr_index_ = uint8_t(v & 0x1F);
            else attr_[attr_index_ % attr_.size()] = v;
            attr_flip_flop_addr_ = !attr_flip_flop_addr_;
            break;
        case 0x3C2: misc_output_ = v; break;
        case 0x3C4: sequencer_index_ = v; break;
        case 0x3C5: sequencer_[sequencer_index_ % sequencer_.size()] = v; break;
        case 0x3CE: gfx_index_ = v; break;
        case 0x3CF: gfx_[gfx_index_ % gfx_.size()] = v; break;
        case 0x3D4: crtc_index_ = v; break;
        case 0x3D5: crtc_[crtc_index_ % crtc_.size()] = v; break;
        default: break;
    }
}

uint8_t Ega::mem_read(uint32_t addr) const {
    uint32_t off;
    if (addr <= 0xAFFFF) off = addr - 0xA0000;
    else if (addr <= 0xB7FFF) off = 0x10000 + (addr - 0xB0000);
    else off = 0x18000 + (addr - 0xB8000);
    return vram[off];
}
void Ega::mem_write(uint32_t addr, uint8_t v) {
    uint32_t off;
    if (addr <= 0xAFFFF) off = addr - 0xA0000;
    else if (addr <= 0xB7FFF) off = 0x10000 + (addr - 0xB0000);
    else off = 0x18000 + (addr - 0xB8000);
    vram[off] = v;
}

void Ega::tick(uint64_t cpu_cycles) {
    uint64_t d = cpu_cycles - prev_cycles_;
    prev_cycles_ = cpu_cycles;
    retrace_credit_ += double(d);
    constexpr double kFramePeriod = 8000000.0 / 60.0;    // ~60Hz frame, this machine's fixed 8MHz clock
    constexpr double kRetraceWindow = kFramePeriod * 0.08;  // a plausible vertical-retrace duty cycle
    while (retrace_credit_ >= kFramePeriod) retrace_credit_ -= kFramePeriod;
    retrace_ = retrace_credit_ < kRetraceWindow;
}

}  // namespace ibmpcat
