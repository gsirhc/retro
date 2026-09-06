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

uint32_t Ega::window_offset(uint32_t addr) const {
    switch (gc_memory_mapping()) {
        case 0:  // 128K @ A0000 -- spans this device's whole claimed range
            return addr - 0xA0000;
        case 1:  // 64K @ A0000 (the standard EGA graphics-mode mapping)
            if (addr > 0xAFFFF) return kOutOfWindow;
            return addr - 0xA0000;
        case 2:  // 32K @ B0000, MDA-compatible mono text window
            if (addr < 0xB0000 || addr > 0xB7FFF) return kOutOfWindow;
            return addr - 0xB0000;
        default:  // 3: 32K @ B8000, CGA-compatible color text window
            if (addr < 0xB8000) return kOutOfWindow;
            return addr - 0xB8000;
    }
}

uint8_t Ega::mem_read(uint32_t addr) const {
    uint32_t off = window_offset(addr);
    if (off == kOutOfWindow) return 0xFF;  // not decoded by the active window -- open bus

    // Odd/even chaining folds the CPU's own address parity into the plane
    // selection (see the file header), so the per-plane storage offset
    // drops that low bit -- consecutive CPU bytes of the same parity land
    // at consecutive per-plane offsets, exactly how text mode's character/
    // attribute pairs end up adjacent within each of planes 0 and 1.
    bool oe = !seq_odd_even_disabled();
    uint32_t plane_off = oe ? (off >> 1) : off;

    // Real hardware: every memory read loads all 4 planes into the latch,
    // regardless of which read mode (or even which write mode a later
    // write will use) is selected.
    for (int p = 0; p < 4; ++p) latch_[p] = vram[(plane_off << 2) + p];

    if (gc_read_mode1()) {
        // Read Mode 1: colour-compare. A result bit is set where every
        // plane the Color Don't Care register marks as "care about" (bit
        // set = participates) matches the corresponding bit of Color
        // Compare.
        uint8_t result = 0xFF;
        uint8_t care = gc_color_dont_care();
        uint8_t want = gc_color_compare();
        for (int p = 0; p < 4; ++p) {
            if (!(care & (1 << p))) continue;
            uint8_t match = (want & (1 << p)) ? latch_[p] : uint8_t(~latch_[p]);
            result &= match;
        }
        return result;
    }

    // Read Mode 0: one plane, selected by Read Map Select -- except that
    // when odd/even chaining is active, the CPU address's own parity
    // substitutes for Read Map Select's low bit (this is what makes flat
    // sequential reads of text-mode VRAM alternate between the character
    // plane and the attribute plane without software ever touching this
    // register).
    uint8_t plane = gc_read_map_select();
    if (oe) plane = uint8_t((plane & 0xFE) | (off & 1));
    return latch_[plane & 3];
}

void Ega::mem_write(uint32_t addr, uint8_t v) {
    uint32_t off = window_offset(addr);
    if (off == kOutOfWindow) return;  // not decoded by the active window

    bool oe = !seq_odd_even_disabled();
    uint32_t plane_off = oe ? (off >> 1) : off;
    uint8_t map_mask = seq_map_mask();
    uint8_t write_mode = gc_write_mode();
    uint8_t bit_mask = gc_bit_mask();
    uint8_t rotated = rotate_right8(v, gc_rotate_count());
    if (write_mode == 3) {
        // Write Mode 3: the rotated CPU byte becomes an *additional* bit
        // mask (ANDed with GR08), and Set/Reset supplies every plane's
        // value outright -- Enable Set/Reset is not consulted in this
        // mode, a real, documented EGA/VGA quirk.
        bit_mask = uint8_t(bit_mask & rotated);
    }

    for (int p = 0; p < 4; ++p) {
        if (!(map_mask & (1 << p))) continue;
        // Odd/even chaining also gates *which* plane of a same-parity pair
        // the CPU's address is even allowed to reach -- see mem_read.
        if (oe && (p & 1) != int(off & 1)) continue;

        uint32_t idx = (plane_off << 2) + uint32_t(p);
        if (write_mode == 1) {
            // Write Mode 1: direct latch-to-VRAM copy, CPU byte ignored --
            // this is the real hardware's VRAM-to-VRAM block-copy trick
            // (read a source address to load the latch, then write the
            // destination address).
            vram[idx] = latch_[p];
            continue;
        }

        uint8_t val;
        if (write_mode == 2) {
            // Write Mode 2: the CPU byte's bit p selects this plane's
            // value outright (all-1s or all-0s), one CPU bit per plane.
            val = (v & (1 << p)) ? uint8_t(0xFF) : uint8_t(0x00);
        } else if (write_mode == 3 || (gc_enable_set_reset() & (1 << p))) {
            // Set/Reset supplies this plane's value: mode 3 always (see
            // above), or mode 0 when Enable Set/Reset says so per-plane.
            val = (gc_set_reset() & (1 << p)) ? uint8_t(0xFF) : uint8_t(0x00);
        } else {
            // Write Mode 0's CPU-data path: rotate, then optionally
            // combine with the latch via the Data Rotate ALU function.
            val = rotated;
            switch (gc_raster_op()) {
                case 1: val = uint8_t(val & latch_[p]); break;   // AND
                case 2: val = uint8_t(val | latch_[p]); break;   // OR
                case 3: val = uint8_t(val ^ latch_[p]); break;   // XOR
                default: break;                                   // 0: replace
            }
        }
        vram[idx] = uint8_t((val & bit_mask) | (vram[idx] & ~bit_mask));
    }
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
