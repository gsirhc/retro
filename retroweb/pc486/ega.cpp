#include "ega.h"

#include <algorithm>
#include <cmath>

namespace pc486 {

void Ega::reset() {
    crtc_.fill(0); crtc_index_ = 0;
    // Line Compare at its 10-bit maximum, so an unprogrammed card shows no split.
    crtc_[0x18] = 0xFF; crtc_[0x07] = 0x10; crtc_[0x09] = 0x40;
    sequencer_.fill(0); sequencer_index_ = 0;
    gfx_.fill(0); gfx_index_ = 0;
    attr_.fill(0); attr_index_ = 0; attr_flip_flop_addr_ = true;
    misc_output_ = 0;
    retrace_ = false;
    prev_cycles_ = 0;
    retrace_credit_ = 0.0;
    frame_count_ = 0;
    recompute_timing_();  // registers are all 0 here -- lands on the implausible-rate fallback below
    dac_.fill(0);
    dac_write_index_ = dac_read_index_ = dac_write_sub_ = dac_read_sub_ = dac_state_ = 0;
    dac_mask_ = 0xFF;  // real power-on default: every pixel bit reaches the DAC
    vbe_.fill(0);
    vbe_index_ = 0;
    vbe_[kVbeRegId] = kVbeIdLowest;
    vbe_[kVbeRegVideoMemory64K] = uint16_t(vram.size() / 65536);
    // VRAM is not cleared; contents survive a controller reset.
}

bool Ega::owns_port(uint16_t port) const {
    switch (port) {
        case 0x3C0: case 0x3C1: case 0x3C2: case 0x3C4: case 0x3C5:
        case 0x3C6: case 0x3C7: case 0x3C8: case 0x3C9:
        case 0x3CC: case 0x3CE: case 0x3CF: case 0x3D4: case 0x3D5: case 0x3DA:
        case kVbeIndexPort: case kVbeDataPort:
            return true;
        default: return false;
    }
}

uint16_t Ega::vbe_read_(int index) const {
    if (index < 0 || index >= kVbeRegCount) return 0;
    // Capability query (kVbeGetCaps): geometry reads return maxima, BANK returns
    // the 32KB-granularity flag in its high byte.
    if (vbe_[kVbeRegEnable] & kVbeGetCaps) {
        switch (index) {
            case kVbeRegXres: return kVbeMaxXres;
            case kVbeRegYres: return kVbeMaxYres;
            case kVbeRegBpp: return kVbeMaxBpp;
            case kVbeRegBank: return uint16_t(kVbeBankGranularity32K << 8);
            default: break;
        }
    }
    return vbe_[std::size_t(index)];
}

void Ega::vbe_write_(int index, uint16_t v) {
    if (index < 0 || index >= kVbeRegCount) return;
    switch (index) {
        case kVbeRegId:
            // Only implemented revisions stick, so a probe for an unsupported one fails.
            if (v >= kVbeIdLowest && v <= kVbeIdHighest) vbe_[kVbeRegId] = v;
            return;
        case kVbeRegVideoMemory64K:
            return;  // read-only: how much RAM is soldered to the card
        case kVbeRegBank: {
            // Low 9 bits are the bank number; bits 14/15 are RD/WR selects. Bochs vga.cc masks the same way.
            uint16_t bank = uint16_t(v & kVbeBankNumberMask);
            uint32_t bank_bytes = vbe_bank_bytes();
            uint32_t num_banks = uint32_t(vram.size() / bank_bytes);
            // At bpp=4 each plane_off unit is 4 VRAM bytes, so reachable banks are quartered (Bochs vga.cc).
            if (vbe_[kVbeRegBpp] == 4) num_banks >>= 2;
            if (bank >= num_banks) return;
            vbe_[kVbeRegBank] = bank;
            return;
        }
        case kVbeRegEnable: {
            bool was_on = (vbe_[kVbeRegEnable] & kVbeEnabled) != 0;
            bool was_32k = (vbe_[kVbeRegEnable] & kVbeBankGranularity32K) != 0;
            vbe_[kVbeRegEnable] = v;
            bool now_32k = (v & kVbeBankGranularity32K) != 0;
            // A step-size change resets the window (Bochs does the same).
            if (now_32k != was_32k) vbe_[kVbeRegBank] = 0;
            if ((v & kVbeEnabled) && !was_on) {
                // Mode set clears window/pan state and, unless NoClearMem, VRAM.
                vbe_[kVbeRegBank] = 0;
                vbe_[kVbeRegXOffset] = 0;
                vbe_[kVbeRegYOffset] = 0;
                // Logical line is at least as wide as the displayed one.
                if (vbe_[kVbeRegVirtWidth] < vbe_[kVbeRegXres]) vbe_[kVbeRegVirtWidth] = vbe_[kVbeRegXres];
                if (vbe_[kVbeRegVirtHeight] < vbe_[kVbeRegYres]) vbe_[kVbeRegVirtHeight] = vbe_[kVbeRegYres];
                if (!(v & kVbeNoClearMem)) vram.fill(0);
            }
            return;
        }
        default:
            vbe_[std::size_t(index)] = v;
            return;
    }
}

uint8_t Ega::in(uint16_t port) {
    switch (port) {
        case 0x3DA: {
            attr_flip_flop_addr_ = true;  // reading Input Status 1 resets the AC address/data flip-flop
            uint8_t v = retrace_ ? 0x08 : 0x00;
            if (retrace_ || display_disabled_()) v |= 0x01;
            return v;
        }
        case 0x3C0: return attr_flip_flop_addr_ ? attr_index_ : uint8_t(0xFF);
        case 0x3C1: return attr_[attr_index_ % attr_.size()];
        case 0x3C2: return 0x00;  // Input Status 0 -- not modeled meaningfully
        case 0x3C6: return dac_mask_;                 // PEL Mask
        case 0x3C7: return dac_state_;                // DAC State (3 = read mode, 0 = write mode)
        case 0x3C8: return dac_write_index_;          // PEL Address Write Mode reads back its index
        case 0x3C9: {                                 // PEL Data
            uint8_t v = dac_[std::size_t(dac_read_index_) * 3 + dac_read_sub_];
            if (++dac_read_sub_ == 3) { dac_read_sub_ = 0; ++dac_read_index_; }
            return v;
        }
        case kVbeIndexPort: return uint8_t(vbe_index_ & 0xFF);
        case kVbeDataPort: return uint8_t(vbe_read_(vbe_index_) & 0xFF);
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
        case 0x3C2: misc_output_ = v; recompute_timing_(); break;  // bit 2 picks the dot clock -- see recompute_timing_()
        case 0x3C6: dac_mask_ = v; break;                        // PEL Mask
        case 0x3C7:                                              // PEL Address Read Mode
            dac_read_index_ = v; dac_read_sub_ = 0; dac_state_ = 0x03; break;
        case 0x3C8:                                              // PEL Address Write Mode
            dac_write_index_ = v; dac_write_sub_ = 0; dac_state_ = 0x00; break;
        case 0x3C9:                                              // PEL Data
            // Only 6 bits per channel are wired; 8-bit values come out washed out.
            dac_[std::size_t(dac_write_index_) * 3 + dac_write_sub_] = uint8_t(v & 0x3F);
            if (++dac_write_sub_ == 3) { dac_write_sub_ = 0; ++dac_write_index_; }
            break;
        // 16-bit registers; a byte access reaches only the low half.
        case kVbeIndexPort: vbe_index_ = uint16_t((vbe_index_ & 0xFF00) | v); break;
        case kVbeDataPort: vbe_write_(vbe_index_, uint16_t((vbe_read_(vbe_index_) & 0xFF00) | v)); break;
        case 0x3C4: sequencer_index_ = v; break;
        case 0x3C5:
            sequencer_[sequencer_index_ % sequencer_.size()] = v;
            // Clocking Mode (SR01) changes the dot clock; see recompute_timing_().
            if (sequencer_index_ % sequencer_.size() == 0x01) recompute_timing_();
            break;
        case 0x3CE: gfx_index_ = v; break;
        case 0x3CF: gfx_[gfx_index_ % gfx_.size()] = v; break;
        case 0x3D4: crtc_index_ = v; break;
        case 0x3D5: {
            uint8_t idx = crtc_index_ % crtc_.size();
            crtc_[idx] = v;
            // The only CRTC registers recompute_timing_() reads.
            if (idx == 0x00 || idx == 0x01 || idx == 0x06 || idx == 0x07 || idx == 0x10 || idx == 0x11 ||
                idx == 0x12) recompute_timing_();
            break;
        }
        default: break;
    }
    note_mapping_change();
}

uint16_t Ega::in16(uint16_t port) {
    if (port == kVbeIndexPort) return vbe_index_;
    if (port == kVbeDataPort) return vbe_read_(vbe_index_);
    return uint16_t(in(port)) | (uint16_t(in(uint16_t(port + 1))) << 8);
}

void Ega::out16(uint16_t port, uint16_t v) {
    if (port == kVbeIndexPort) { vbe_index_ = v; return; }
    // A VBE mode change moves the aperture between planar and linear.
    if (port == kVbeDataPort) { vbe_write_(vbe_index_, v); note_mapping_change(); return; }
    out(port, uint8_t(v & 0xFF));
    out(uint16_t(port + 1), uint8_t(v >> 8));
}

void Ega::note_mapping_change() {
    uint32_t sig = uint32_t(sequencer_[2] & 0x0F) |
                   (uint32_t(sequencer_[4] & 0x0F) << 4) |
                   (uint32_t(gfx_[1] & 0x0F) << 8) |
                   (uint32_t(gfx_[3] & 0x07) << 12) |
                   (uint32_t(gfx_[5] & 0x0B) << 16) |
                   (uint32_t(gfx_[6] & 0x0F) << 20) |
                   (uint32_t(gfx_[8]) << 24);
    if (vbe_mode_active()) sig = ~sig;
    if (sig != mapping_sig_) { mapping_sig_ = sig; ++mapping_epoch_; }
}

// Frame period and retrace window from the CRTC's programmed timing (FreeVGA
// CRTC/Sequencer/General register pages). See PC486_REVIEW.md §8.6.
void Ega::recompute_timing_() {
    int dots_per_char = (sequencer_[1] & 0x01) ? 8 : 9;               // Clocking Mode bit 0
    double dot_clock = (misc_output_ & 0x04) ? 28322000.0 : 25175000.0;  // Misc Output bit 2
    if (sequencer_[1] & 0x08) dot_clock *= 0.5;                       // Clocking Mode bit 3: /2

    int h_total_chars = int(crtc_[0x00]) + 5;  // Horizontal Total stores total-5
    int v_total_lines = (int(crtc_[0x06]) |
                          ((int(crtc_[0x07]) & 0x01) << 8) |
                          (((int(crtc_[0x07]) >> 5) & 1) << 9)) + 2;  // Vertical Total (+Overflow) stores total-2

    double frame_hz = dot_clock / (double(dots_per_char) * double(h_total_chars) * double(v_total_lines));

    // Zeroed registers give an implausible rate; fall back to the BIOS mode 03h
    // rate so retrace-wait loops see sane timing.
    constexpr double kFallbackHz = 70.0;
    bool implausible = !(frame_hz >= 40.0 && frame_hz <= 120.0);
    frame_period_cycles_ = cpu_hz_ / (implausible ? kFallbackHz : frame_hz);

    if (implausible) {
        // Retrace scanline numbers are meaningless here too; use a fixed slice.
        retrace_start_cycles_ = 0.0;
        retrace_window_cycles_ = frame_period_cycles_ * 0.08;
        scanline_cycles_ = frame_period_cycles_ / 449.0;
        h_display_cycles_ = scanline_cycles_ * 0.8;
        v_display_cycles_ = scanline_cycles_ * 400.0;
        return;
    }

    // Vertical Retrace Start (CRTC 10h) plus overflow bits.
    int vrs = int(crtc_[0x10]) |
              ((int(crtc_[0x07]) & 0x04) << 6) |
              (((int(crtc_[0x07]) >> 7) & 1) << 9);
    // Vertical Retrace End is a 4-bit comparator on the low scanline bits.
    int vre = (vrs & ~0x0F) | (int(crtc_[0x11]) & 0x0F);
    if (vre <= vrs) vre += 16;

    double per_scanline_cycles = frame_period_cycles_ / double(v_total_lines);
    retrace_start_cycles_ = double(vrs) * per_scanline_cycles;
    retrace_window_cycles_ = double(vre - vrs) * per_scanline_cycles;

    // Display End registers store count - 1.
    int h_display_chars = std::min(int(crtc_[0x01]) + 1, h_total_chars);
    int vde = (int(crtc_[0x12]) |
               ((int(crtc_[0x07]) & 0x02) << 7) |
               (((int(crtc_[0x07]) >> 6) & 1) << 9)) + 1;
    scanline_cycles_ = per_scanline_cycles;
    h_display_cycles_ = per_scanline_cycles * double(h_display_chars) / double(h_total_chars);
    v_display_cycles_ = per_scanline_cycles * double(std::min(vde, v_total_lines));
}

// Input Status 1 bit 0 is inverted display enable (FreeVGA General Registers;
// DOSBox vga_other.cpp). Palette loaders poll it.
bool Ega::display_disabled_() const {
    if (retrace_credit_ >= v_display_cycles_) return true;
    return std::fmod(retrace_credit_, scanline_cycles_) >= h_display_cycles_;
}

uint8_t *Ega::linear_page(uint32_t page_base, bool write) {
    if (vbe_mode_active()) return nullptr;   // the VBE window has its own banked mapping
    if (!seq_chain4()) return nullptr;
    // Every planar stage must be pass-through or a byte write is not a plain store.
    if (write) {
        if (gc_write_mode() != 0) return nullptr;
        if (seq_map_mask() != 0x0F) return nullptr;
        if (gc_bit_mask() != 0xFF) return nullptr;
        if (gc_rotate_count() != 0) return nullptr;
        if (gc_enable_set_reset() != 0) return nullptr;
    } else if (gc_read_mode1()) {
        return nullptr;  // read mode 1 returns a colour-compare result, not data
    }
    // The whole page must sit inside the active window.
    uint32_t off = window_offset(page_base);
    if (off == kOutOfWindow) return nullptr;
    if (window_offset(page_base + 0xFFFu) != off + 0xFFFu) return nullptr;
    if (off + 0x1000u > vram.size()) return nullptr;
    return vram.data() + off;
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

uint32_t Ega::vbe_linear_offset(uint32_t addr) const {
    if (addr < 0xA0000 || addr > 0xAFFFF) return kOutOfWindow;
    uint32_t lin = uint32_t(vbe_[kVbeRegBank]) * vbe_bank_bytes() + (addr - 0xA0000);
    return lin < vram.size() ? lin : kOutOfWindow;
}

// Planar plane_off after the Bank slide in 4bpp DISPI modes (Bochs ext_offset).
uint32_t Ega::vbe_planar_plane_off(uint32_t plane_off) const {
    if (!vbe_planar_banked()) return plane_off;
    return plane_off + uint32_t(vbe_[kVbeRegBank]) * vbe_bank_bytes();
}

uint8_t Ega::mem_read(uint32_t addr) const {
    // SVGA modes bypass the planar engine for a flat banked frame buffer.
    if (vbe_mode_active()) {
        uint32_t lin = vbe_linear_offset(addr);
        return lin == kOutOfWindow ? uint8_t(0xFF) : vram[lin];
    }
    uint32_t off = window_offset(addr);
    if (off == kOutOfWindow) return 0xFF;  // not decoded by the active window -- open bus

    // Odd/even chaining drops the address low bit from the per-plane offset;
    // Chain 4 drops two and takes priority.
    bool c4 = seq_chain4();
    bool oe = !c4 && !seq_odd_even_disabled();
    uint32_t plane_off = c4 ? (off >> 2) : (oe ? (off >> 1) : off);
    plane_off = vbe_planar_plane_off(plane_off);
    // Past the last group: open bus.
    if ((plane_off << 2) + 3 >= vram.size()) return 0xFF;

    // Every read loads all 4 planes into the latch.
    for (int p = 0; p < 4; ++p) latch_[p] = vram[(plane_off << 2) + p];

    if (gc_read_mode1()) {
        // Read Mode 1: a result bit is set where every "care" plane matches Color Compare.
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

    // Read Mode 0: Read Map Select picks the plane. Odd/even chaining replaces its
    // low bit with address parity, Chain 4 replaces it with address bits 0-1.
    uint8_t plane = gc_read_map_select();
    if (c4) plane = uint8_t(off & 3);
    else if (oe) plane = uint8_t((plane & 0xFE) | (off & 1));
    return latch_[plane & 3];
}

void Ega::mem_write(uint32_t addr, uint8_t v) {
    if (vbe_mode_active()) {  // see mem_read
        uint32_t lin = vbe_linear_offset(addr);
        if (lin != kOutOfWindow) vram[lin] = v;
        return;
    }
    uint32_t off = window_offset(addr);
    if (off == kOutOfWindow) return;  // not decoded by the active window

    bool c4 = seq_chain4();
    bool oe = !c4 && !seq_odd_even_disabled();
    uint32_t plane_off = c4 ? (off >> 2) : (oe ? (off >> 1) : off);
    plane_off = vbe_planar_plane_off(plane_off);
    if ((plane_off << 2) + 3 >= vram.size()) return;
    uint8_t map_mask = seq_map_mask();
    uint8_t write_mode = gc_write_mode();
    uint8_t bit_mask = gc_bit_mask();
    uint8_t rotated = rotate_right8(v, gc_rotate_count());
    if (write_mode == 3) {
        // Write Mode 3: the rotated byte also masks, and Set/Reset supplies the value
        // (Enable Set/Reset ignored).
        bit_mask = uint8_t(bit_mask & rotated);
    }

    for (int p = 0; p < 4; ++p) {
        if (!(map_mask & (1 << p))) continue;
        // Chain 4 / odd-even gate which plane the address reaches; Map Mask still applies.
        if (c4 && p != int(off & 3)) continue;
        if (oe && (p & 1) != int(off & 1)) continue;

        uint32_t idx = (plane_off << 2) + uint32_t(p);
        if (write_mode == 1) {
            // Write Mode 1: latch copy, CPU byte ignored (VRAM-to-VRAM blit).
            vram[idx] = latch_[p];
            continue;
        }

        uint8_t val;
        if (write_mode == 2) {
            // Write Mode 2: CPU bit p gives plane p all 1s or 0s.
            val = (v & (1 << p)) ? uint8_t(0xFF) : uint8_t(0x00);
        } else if (write_mode == 3 || (gc_enable_set_reset() & (1 << p))) {
            // Set/Reset value: always in mode 3, per Enable Set/Reset in mode 0.
            val = (gc_set_reset() & (1 << p)) ? uint8_t(0xFF) : uint8_t(0x00);
        } else {
            // Mode 0: rotate, then combine with the latch per the ALU function.
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

}  // namespace pc486
