#include "ega.h"

namespace pc486 {

void Ega::reset() {
    crtc_.fill(0); crtc_index_ = 0;
    sequencer_.fill(0); sequencer_index_ = 0;
    gfx_.fill(0); gfx_index_ = 0;
    attr_.fill(0); attr_index_ = 0; attr_flip_flop_addr_ = true;
    misc_output_ = 0;
    retrace_ = false;
    prev_cycles_ = 0;
    retrace_credit_ = 0.0;
    dac_.fill(0);
    dac_write_index_ = dac_read_index_ = dac_write_sub_ = dac_read_sub_ = dac_state_ = 0;
    dac_mask_ = 0xFF;  // real power-on default: every pixel bit reaches the DAC
    vbe_.fill(0);
    vbe_index_ = 0;
    vbe_[kVbeRegId] = kVbeIdLowest;
    vbe_[kVbeRegVideoMemory64K] = uint16_t(vram.size() / 65536);
    // vram deliberately NOT cleared -- matches real hardware (contents
    // survive a controller reset; software clears it explicitly if it
    // wants a blank screen, typically as part of a mode set). The DAC
    // above is cleared, unlike VRAM, only because every mode set loads it
    // wholesale anyway and a deterministic all-black palette is the honest
    // "nothing has programmed me yet" answer for a renderer.
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
    // Capability query: see kVbeGetCaps in ega.h. Only the three geometry
    // registers answer differently; everything else reads normally.
    if (vbe_[kVbeRegEnable] & kVbeGetCaps) {
        switch (index) {
            case kVbeRegXres: return kVbeMaxXres;
            case kVbeRegYres: return kVbeMaxYres;
            case kVbeRegBpp: return kVbeMaxBpp;
            default: break;
        }
    }
    return vbe_[std::size_t(index)];
}

void Ega::vbe_write_(int index, uint16_t v) {
    if (index < 0 || index >= kVbeRegCount) return;
    switch (index) {
        case kVbeRegId:
            // A probe writes a revision number and reads it back; only
            // revisions this card actually implements stick, so an
            // unsupported one reads back as whatever was there before and
            // the probe correctly fails. See ega.h.
            if (v >= kVbeIdLowest && v <= kVbeIdHighest) vbe_[kVbeRegId] = v;
            return;
        case kVbeRegVideoMemory64K:
            return;  // read-only: how much RAM is soldered to the card
        case kVbeRegEnable: {
            bool was_on = (vbe_[kVbeRegEnable] & kVbeEnabled) != 0;
            vbe_[kVbeRegEnable] = v;
            if ((v & kVbeEnabled) && !was_on) {
                // Switching an SVGA mode on resets the window/pan state and
                // (unless the caller asks otherwise) blanks the frame
                // buffer, so a mode set never shows the previous mode's
                // leftovers -- the same guarantee a legacy BIOS mode set
                // gives by clearing VRAM itself.
                vbe_[kVbeRegBank] = 0;
                vbe_[kVbeRegXOffset] = 0;
                vbe_[kVbeRegYOffset] = 0;
                // A logical line narrower than the displayed one is not a
                // thing; software that wants a *wider* one writes VirtWidth
                // before enabling and keeps it.
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
            return retrace_ ? 0x08 : 0x00;
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
        case 0x3C2: misc_output_ = v; break;
        case 0x3C6: dac_mask_ = v; break;                        // PEL Mask
        case 0x3C7:                                              // PEL Address Read Mode
            dac_read_index_ = v; dac_read_sub_ = 0; dac_state_ = 0x03; break;
        case 0x3C8:                                              // PEL Address Write Mode
            dac_write_index_ = v; dac_write_sub_ = 0; dac_state_ = 0x00; break;
        case 0x3C9:                                              // PEL Data
            // Only 6 bits per channel reach the DAC's RAM; the top two are
            // simply not wired, which is why a driver that writes 8-bit
            // values gets a washed-out picture on real hardware rather than
            // an error.
            dac_[std::size_t(dac_write_index_) * 3 + dac_write_sub_] = uint8_t(v & 0x3F);
            if (++dac_write_sub_ == 3) { dac_write_sub_ = 0; ++dac_write_index_; }
            break;
        // These two are 16-bit registers (see in16/out16); a byte-wide
        // access reaches only their low half, high half untouched.
        case kVbeIndexPort: vbe_index_ = uint16_t((vbe_index_ & 0xFF00) | v); break;
        case kVbeDataPort: vbe_write_(vbe_index_, uint16_t((vbe_read_(vbe_index_) & 0xFF00) | v)); break;
        case 0x3C4: sequencer_index_ = v; break;
        case 0x3C5: sequencer_[sequencer_index_ % sequencer_.size()] = v; break;
        case 0x3CE: gfx_index_ = v; break;
        case 0x3CF: gfx_[gfx_index_ % gfx_.size()] = v; break;
        case 0x3D4: crtc_index_ = v; break;
        case 0x3D5: crtc_[crtc_index_ % crtc_.size()] = v; break;
        default: break;
    }
}

uint16_t Ega::in16(uint16_t port) {
    if (port == kVbeIndexPort) return vbe_index_;
    if (port == kVbeDataPort) return vbe_read_(vbe_index_);
    return uint16_t(in(port)) | (uint16_t(in(uint16_t(port + 1))) << 8);
}

void Ega::out16(uint16_t port, uint16_t v) {
    if (port == kVbeIndexPort) { vbe_index_ = v; return; }
    if (port == kVbeDataPort) { vbe_write_(vbe_index_, v); return; }
    out(port, uint8_t(v & 0xFF));
    out(uint16_t(port + 1), uint8_t(v >> 8));
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
    uint32_t lin = uint32_t(vbe_[kVbeRegBank]) * kVbeBankSize + (addr - 0xA0000);
    return lin < vram.size() ? lin : kOutOfWindow;
}

uint8_t Ega::mem_read(uint32_t addr) const {
    // An SVGA mode replaces the whole planar engine -- no latches, no plane
    // select, no Graphics Controller ALU -- with a flat byte-per-pixel
    // frame buffer seen 64KB at a time. See ega.h's VBE block.
    if (vbe_mode_active()) {
        uint32_t lin = vbe_linear_offset(addr);
        return lin == kOutOfWindow ? uint8_t(0xFF) : vram[lin];
    }
    uint32_t off = window_offset(addr);
    if (off == kOutOfWindow) return 0xFF;  // not decoded by the active window -- open bus

    // Odd/even chaining folds the CPU's own address parity into the plane
    // selection (see the file header), so the per-plane storage offset
    // drops that low bit -- consecutive CPU bytes of the same parity land
    // at consecutive per-plane offsets, exactly how text mode's character/
    // attribute pairs end up adjacent within each of planes 0 and 1.
    // Chain 4 (mode 13h) folds the CPU address's low TWO bits into the
    // plane selection and drops them from the per-plane offset, so four
    // consecutive CPU bytes are four different planes at one offset -- see
    // the file header. It takes priority over odd/even chaining.
    bool c4 = seq_chain4();
    bool oe = !c4 && !seq_odd_even_disabled();
    uint32_t plane_off = c4 ? (off >> 2) : (oe ? (off >> 1) : off);

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
    // ... and in Chain 4 the CPU address's low two bits replace Read Map
    // Select outright, which is what makes a flat read of mode 13h's frame
    // buffer come back as consecutive pixels.
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
        // Chain 4 / odd-even chaining also gate *which* plane the CPU's
        // address is even allowed to reach -- see mem_read. Map Mask still
        // applies on top in both cases (the Sequencer's plane-enable wires
        // are the same ones), which is why a mode-13h driver that narrows
        // Map Mask genuinely stops some pixels landing.
        if (c4 && p != int(off & 3)) continue;
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

}  // namespace pc486
