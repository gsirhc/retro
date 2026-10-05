#include "ega.h"

#include <algorithm>
#include <cmath>

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
    recompute_timing_();  // registers are all 0 here -- lands on the implausible-rate fallback below
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
    // Capability query: see kVbeGetCaps in ega.h. Geometry registers
    // answer with maxima; BANK answers with the 32KB-granularity flag in
    // its high byte -- what dispi_support_bank_granularity_32k reads.
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
            // A probe writes a revision number and reads it back; only
            // revisions this card actually implements stick, so an
            // unsupported one reads back as whatever was there before and
            // the probe correctly fails. See ega.h.
            if (v >= kVbeIdLowest && v <= kVbeIdHighest) vbe_[kVbeRegId] = v;
            return;
        case kVbeRegVideoMemory64K:
            return;  // read-only: how much RAM is soldered to the card
        case kVbeRegBank: {
            // Low 9 bits are the bank number; bits 14/15 are the optional
            // RD/WR selects the firmware ORs in (see kVbeBankNumberMask).
            // Bochs vga.cc masks the same way before storing.
            uint16_t bank = uint16_t(v & kVbeBankNumberMask);
            uint32_t bank_bytes = vbe_bank_bytes();
            uint32_t num_banks = uint32_t(vram.size() / bank_bytes);
            // At bpp=4 the aperture slides over per-plane offsets, and each
            // plane_off unit costs 4 interleaved VRAM bytes, so the number
            // of reachable banks is quartered. Rejecting past-end values
            // matches Bochs (vga.cc VBE_DISPI_INDEX_BANK).
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
            // Switching the Bank step size (or turning an SVGA mode on)
            // resets the window so software never sees a stale bank index
            // interpreted under the new granularity -- Bochs does the same.
            if (now_32k != was_32k) vbe_[kVbeRegBank] = 0;
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
        case 0x3C5:
            sequencer_[sequencer_index_ % sequencer_.size()] = v;
            // Clocking Mode (SR01): dots-per-char (bit 0) and the dot-clock
            // divide-by-2 (bit 3) -- see recompute_timing_().
            if (sequencer_index_ % sequencer_.size() == 0x01) recompute_timing_();
            break;
        case 0x3CE: gfx_index_ = v; break;
        case 0x3CF: gfx_[gfx_index_ % gfx_.size()] = v; break;
        case 0x3D4: crtc_index_ = v; break;
        case 0x3D5: {
            uint8_t idx = crtc_index_ % crtc_.size();
            crtc_[idx] = v;
            // Horizontal Total/Display End, Vertical Total, Vertical Retrace
            // Start/End, Vertical Display End and their Overflow bits -- the
            // only CRTC registers recompute_timing_() consults.
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
    // A VBE mode change moves the aperture between planar and linear, so the
    // mapping has to be re-checked here; every other path below goes through
    // out(), which notes for itself.
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

// Derives the vertical-frame period and retrace-window length from the
// CRTC's own programmed timing -- the same inputs a real CRT controller's
// scanout counters use, not a hardcoded refresh-rate constant. Register
// semantics: IBM VGA CRTC/Sequencer/Misc Output, as documented in the
// standard VGA register reference (e.g. FreeVGA's "CRTC Registers",
// "Sequencer Registers" and "General Registers" pages) and reproduced
// identically by every compatible BIOS, including this machine's Bochs
// vgabios. See PC486_REVIEW.md §8.6.
void Ega::recompute_timing_() {
    int dots_per_char = (sequencer_[1] & 0x01) ? 8 : 9;               // Clocking Mode bit 0
    double dot_clock = (misc_output_ & 0x04) ? 28322000.0 : 25175000.0;  // Misc Output bit 2
    if (sequencer_[1] & 0x08) dot_clock *= 0.5;                       // Clocking Mode bit 3: /2

    int h_total_chars = int(crtc_[0x00]) + 5;  // Horizontal Total stores total-5
    int v_total_lines = (int(crtc_[0x06]) |
                          ((int(crtc_[0x07]) & 0x01) << 8) |
                          (((int(crtc_[0x07]) >> 5) & 1) << 9)) + 2;  // Vertical Total (+Overflow) stores total-2

    double frame_hz = dot_clock / (double(dots_per_char) * double(h_total_chars) * double(v_total_lines));

    // A freshly reset (or mid-mode-set) CRTC has every register at 0, which
    // plugs into the formula above as several hundred kHz -- no real
    // monitor could sync to that. Fall back to the rate this machine's own
    // BIOS programs for its default mode 03h until the guest programs
    // something plausible, rather than let a "wait for vertical retrace"
    // loop see a nonsense frequency.
    constexpr double kFallbackHz = 70.0;
    bool implausible = !(frame_hz >= 40.0 && frame_hz <= 120.0);
    frame_period_cycles_ = cpu_hz_ / (implausible ? kFallbackHz : frame_hz);

    if (implausible) {
        // The same implausible registers make Vertical Retrace Start/End
        // meaningless too (their scanline numbers presume a real
        // v_total_lines) -- use a plausible fixed slice of the fallback
        // frame instead of propagating garbage into the retrace window.
        retrace_start_cycles_ = 0.0;
        retrace_window_cycles_ = frame_period_cycles_ * 0.08;
        scanline_cycles_ = frame_period_cycles_ / 449.0;
        h_display_cycles_ = scanline_cycles_ * 0.8;
        v_display_cycles_ = scanline_cycles_ * 400.0;
        return;
    }

    // Vertical Retrace Start (CRTC 10h) + its two Overflow bits.
    int vrs = int(crtc_[0x10]) |
              ((int(crtc_[0x07]) & 0x04) << 6) |
              (((int(crtc_[0x07]) >> 7) & 1) << 9);
    // Vertical Retrace End (CRTC 11h) is only a 4-bit comparator against the
    // low bits of the scanline counter, so its value wraps relative to vrs.
    int vre = (vrs & ~0x0F) | (int(crtc_[0x11]) & 0x0F);
    if (vre <= vrs) vre += 16;

    double per_scanline_cycles = frame_period_cycles_ / double(v_total_lines);
    retrace_start_cycles_ = double(vrs) * per_scanline_cycles;
    retrace_window_cycles_ = double(vre - vrs) * per_scanline_cycles;

    // Horizontal Display End (CRTC 01h) stores displayed chars - 1; Vertical
    // Display End (CRTC 12h + Overflow bits 1 and 6) stores displayed lines - 1.
    int h_display_chars = std::min(int(crtc_[0x01]) + 1, h_total_chars);
    int vde = (int(crtc_[0x12]) |
               ((int(crtc_[0x07]) & 0x02) << 7) |
               (((int(crtc_[0x07]) >> 6) & 1) << 9)) + 1;
    scanline_cycles_ = per_scanline_cycles;
    h_display_cycles_ = per_scanline_cycles * double(h_display_chars) / double(h_total_chars);
    v_display_cycles_ = per_scanline_cycles * double(std::min(vde, v_total_lines));
}

// Input Status 1 bit 0 is the inverted display-enable signal: set during any
// horizontal or vertical blanking interval (FreeVGA "General Registers";
// DOSBox vga_other.cpp). Palette loaders poll it before each DAC write.
bool Ega::display_disabled_() const {
    if (retrace_credit_ >= v_display_cycles_) return true;
    return std::fmod(retrace_credit_, scanline_cycles_) >= h_display_cycles_;
}

uint8_t *Ega::linear_page(uint32_t page_base, bool write) {
    if (vbe_mode_active()) return nullptr;   // the VBE window has its own banked mapping
    if (!seq_chain4()) return nullptr;
    // Every planar stage must be pass-through, or a byte write is not just a
    // store: Map Mask can drop planes (a mode-13h driver narrowing it really
    // does lose pixels), the bit mask and rotate alter the byte, Set/Reset
    // substitutes it outright, and write modes 1-3 ignore it entirely.
    if (write) {
        if (gc_write_mode() != 0) return nullptr;
        if (seq_map_mask() != 0x0F) return nullptr;
        if (gc_bit_mask() != 0xFF) return nullptr;
        if (gc_rotate_count() != 0) return nullptr;
        if (gc_enable_set_reset() != 0) return nullptr;
    } else if (gc_read_mode1()) {
        return nullptr;  // read mode 1 returns a colour-compare result, not data
    }
    // The whole page has to sit inside the active window, so the CPU cannot
    // walk off the end of it through a pointer we handed out.
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

// Planar plane_off after the Bank register's slide, when a 4bpp DISPI mode
// is on. Bochs does the same as vgacore's ext_offset (bank << 16 at 64KB
// gran, bank << 15 at 32KB).
uint32_t Ega::vbe_planar_plane_off(uint32_t plane_off) const {
    if (!vbe_planar_banked()) return plane_off;
    return plane_off + uint32_t(vbe_[kVbeRegBank]) * vbe_bank_bytes();
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
    plane_off = vbe_planar_plane_off(plane_off);
    // Past the last interleaved group there is nothing to answer -- same
    // open-bus convention as an address outside the active window.
    if ((plane_off << 2) + 3 >= vram.size()) return 0xFF;

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
    plane_off = vbe_planar_plane_off(plane_off);
    if ((plane_off << 2) + 3 >= vram.size()) return;
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
