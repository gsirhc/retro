#include "ega.h"

#include <algorithm>
#include <cmath>

namespace ibmpcat {

void Ega::reset() {
    crtc_.fill(0); crtc_index_ = 0;
    sequencer_.fill(0); sequencer_index_ = 0;
    gfx_.fill(0); gfx_index_ = 0;
    attr_.fill(0); attr_index_ = 0; attr_flip_flop_addr_ = true;
    misc_output_ = 0;
    prev_cycles_ = 0;
    raster_ = 0.0;
    next_free_ = 0.0;
    frame_count_ = 0;
    vint_ = false;
    recompute_timing_();
    // vram survives reset, like real hardware.
}

bool Ega::owns_port(uint16_t port) const {
    switch (port) {
        case 0x3C0: case 0x3C2: case 0x3C4: case 0x3C5: case 0x3CA:
        case 0x3CC: case 0x3CE: case 0x3CF:
            return true;
        case 0x3B4: case 0x3B5: case 0x3BA: return !(misc_output_ & 1);
        case 0x3D4: case 0x3D5: case 0x3DA: return (misc_output_ & 1) != 0;
        default: return false;
    }
}

// Misc Output bit 0 moves the CRTC and Input Status 1 between 3Bx and 3Dx (IBM EGA TR).
uint16_t Ega::color_port(uint16_t port) const {
    if (port >= 0x3B0 && port <= 0x3BF) return uint16_t(port + 0x20);
    return port;
}

uint8_t Ega::in(uint16_t port) {
    if (!owns_port(port)) return 0xFF;
    switch (color_port(port)) {
        case 0x3DA: {
            attr_flip_flop_addr_ = true;  // reading Input Status 1 resets the AC flip-flop
            int line = int(raster_ / line_cycles_);
            double x = raster_ - line * line_cycles_;
            bool vretrace = line >= vrs_line_ && line < vre_line_;
            bool display = line < vde_lines_ && x < hde_cycles_;
            // Bit 0 is 1 outside display enable, the CGA's polarity; the EGA manual's wording says the reverse.
            return uint8_t((vretrace ? 0x08 : 0x00) | (display ? 0x00 : 0x01));
        }
        case 0x3C2: {
            // Clock select 0-3 reads switch 4-1, 0 when closed (IBM EGA BIOS listing, RD_SWS).
            // Bits 0-3 float high (DOSBox vga_misc.cpp); no feature card drives bits 5-6.
            int sel = (misc_output_ >> 2) & 3;
            bool open = (switches_ >> (3 - sel)) & 1;
            return uint8_t(0x0F | (open ? 0x10 : 0x00) | (vint_ ? 0x80 : 0x00));
        }
        case 0x3D5:
            // Only the start address, cursor and light pen registers read back (IBM EGA TR, CRT Controller).
            if (crtc_index_ >= 0x0C && crtc_index_ <= 0x0F) return crtc_[crtc_index_];
            if (crtc_index_ == 0x10 || crtc_index_ == 0x11) return 0x00;  // light pen latch, no pen
            return 0xFF;
        default: return 0xFF;  // write-only
    }
}

void Ega::out(uint16_t port, uint8_t v) {
    if (!owns_port(port)) return;
    switch (color_port(port)) {
        case 0x3C0:
            if (attr_flip_flop_addr_) attr_index_ = uint8_t(v & 0x1F);
            else attr_[attr_index_ % attr_.size()] = v;
            attr_flip_flop_addr_ = !attr_flip_flop_addr_;
            break;
        case 0x3C2: misc_output_ = v; recompute_timing_(); break;
        case 0x3C4: sequencer_index_ = v; break;
        case 0x3C5: sequencer_[sequencer_index_ % sequencer_.size()] = v; recompute_timing_(); break;
        case 0x3CE: gfx_index_ = v; break;
        case 0x3CF: gfx_[gfx_index_ % gfx_.size()] = v; break;
        case 0x3D4: crtc_index_ = v; break;
        case 0x3D5:
            crtc_[crtc_index_ % crtc_.size()] = v;
            if (crtc_index_ == 0x11 && !(v & 0x10)) vint_ = false;  // Clear Vertical Interrupt
            recompute_timing_();
            break;
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

// A0 is replaced in odd/even chaining: by the inverted page bit, or A16 in the 128KB map (86Box vid_ega.c).
uint32_t Ega::cpu_plane_offset(uint32_t off) const {
    uint32_t a = off & 0xFFFF;
    if (gfx_[6] & 0x02) {
        uint32_t a0 = gc_memory_mapping() == 0 ? (off >> 16) & 1 : (~misc_output_ >> 5) & 1;
        a = (a & ~1u) | a0;
    }
    if (!(sequencer_[4] & 0x02)) a &= 0x3FFF;  // Memory Mode bit 1 clear: 64KB card addressing
    return a;
}

uint32_t Ega::display_address(uint32_t ma, int row_scan) const {
    uint32_t a;
    if (crtc_[0x17] & 0x40) {
        a = ma;
    } else {
        // Word mode: MA shifts up one and MA15 (or MA13, CRTC 17h bit 5 clear) fills bit 0.
        uint32_t low = (crtc_[0x17] & 0x20) ? (ma >> 15) & 1 : (ma >> 13) & 1;
        a = (ma << 1) | low;
    }
    if (crtc_cga_banks()) a = (a & ~0x2000u) | (uint32_t(row_scan & 1) << 13);
    if (crtc_row_scan_ma14()) a = (a & ~0x4000u) | (uint32_t((row_scan >> 1) & 1) << 14);
    return a & 0xFFFF;
}

uint8_t Ega::mem_read(uint32_t addr) const {
    uint32_t off = window_offset(addr);
    if (off == kOutOfWindow) return 0xFF;  // not decoded by the active window

    uint32_t plane_off = cpu_plane_offset(off);

    // Every read loads all 4 planes into the latch, whatever the read mode.
    for (int p = 0; p < 4; ++p) latch_[p] = vram[(plane_off << 2) + p];

    if (gc_read_mode1()) {
        // Read Mode 1: colour compare against planes marked in Color Don't Care.
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

    // Read Mode 0: Read Map Select picks the plane; GR05 bit 4 puts CPU A0 in its low bit (86Box vid_ega.c).
    uint8_t plane = gc_read_map_select();
    if (gfx_[5] & 0x10) plane = uint8_t((plane & 0xFE) | (off & 1));
    return latch_[plane & 3];
}

void Ega::mem_write(uint32_t addr, uint8_t v) {
    uint32_t off = window_offset(addr);
    if (off == kOutOfWindow) return;  // not decoded by the active window

    bool oe = !seq_odd_even_disabled();
    uint32_t plane_off = cpu_plane_offset(off);
    uint8_t map_mask = seq_map_mask();
    uint8_t write_mode = gc_write_mode();
    uint8_t bit_mask = gc_bit_mask();
    uint8_t rotated = rotate_right8(v, gc_rotate_count());
    if (write_mode == 3) {
        // Write Mode 3: the rotated CPU byte is ANDed into the bit mask and Set/Reset
        // supplies every plane (Enable Set/Reset is ignored), an EGA/VGA quirk.
        bit_mask = uint8_t(bit_mask & rotated);
    }

    for (int p = 0; p < 4; ++p) {
        if (!(map_mask & (1 << p))) continue;
        // Odd/even chaining limits the CPU to one plane of each pair.
        if (oe && (p & 1) != int(off & 1)) continue;

        uint32_t idx = (plane_off << 2) + uint32_t(p);
        if (write_mode == 1) {
            // Write Mode 1: latch copy, CPU byte ignored (VRAM-to-VRAM block copy).
            vram[idx] = latch_[p];
            continue;
        }

        uint8_t val;
        if (write_mode == 2) {
            // Write Mode 2: CPU bit p selects plane p's value.
            val = (v & (1 << p)) ? uint8_t(0xFF) : uint8_t(0x00);
        } else if (write_mode == 3 || (gc_enable_set_reset() & (1 << p))) {
            // Set/Reset supplies the value: always in mode 3, per-plane via Enable Set/Reset in mode 0.
            val = (gc_set_reset() & (1 << p)) ? uint8_t(0xFF) : uint8_t(0x00);
        } else {
            val = rotated;
        }
        // Data Rotate logic applies to CPU, Set/Reset and write mode 2 data alike (86Box vid_ega.c).
        switch (gc_raster_op()) {
            case 1: val = uint8_t(val & latch_[p]); break;   // AND
            case 2: val = uint8_t(val | latch_[p]); break;   // OR
            case 3: val = uint8_t(val ^ latch_[p]); break;   // XOR
            default: break;                                   // 0: replace
        }
        // Bits the Bit Mask leaves out come from the latch, not memory (86Box vid_ega.c).
        vram[idx] = uint8_t((val & bit_mask) | (latch_[p] & ~bit_mask));
    }
}

void Ega::recompute_timing_() {
    double dot_hz = ((misc_output_ >> 2) & 3) == 1 ? 16257000.0 : 14318180.0;
    if (sequencer_[1] & 0x08) dot_hz /= 2;
    int char_dots = (sequencer_[1] & 0x01) ? 8 : 9;
    int h_total = crtc_[0x00] + 2;
    int h_display = crtc_[0x01] + 1;
    int v_total = crtc_[0x06] | ((crtc_[0x07] & 0x01) << 8);
    int v_display = (crtc_[0x12] | (((crtc_[0x07] >> 1) & 1) << 8)) + 1;
    int vrs = crtc_[0x10] | (((crtc_[0x07] >> 2) & 1) << 8);
    // Five memory cycles fetch four characters at the undivided dot clock (EGA TR, Clocking Mode bit 1).
    mem_cycle_ = 4.0 * char_dots / (((misc_output_ >> 2) & 3) == 1 ? 16257000.0 : 14318180.0) * kCpuHz / 5.0;
    line_cycles_ = h_total * char_dots / dot_hz * kCpuHz;
    frame_cycles_ = line_cycles_ * v_total;
    double hz = frame_cycles_ > 0 ? kCpuHz / frame_cycles_ : 0.0;
    if (v_total < 2 || h_display >= h_total || vrs >= v_total || hz < 30.0 || hz > 120.0) {
        // Unprogrammed or mid mode set: a 60 Hz raster with the retrace at the top.
        frame_cycles_ = kCpuHz / 60.0;
        line_cycles_ = frame_cycles_ / 262;
        hde_cycles_ = line_cycles_;
        vde_lines_ = 262;
        vrs_line_ = 0;
        vre_line_ = 21;
    } else {
        hde_cycles_ = line_cycles_ * h_display / h_total;
        vde_lines_ = v_display;
        vrs_line_ = vrs;
        // Vertical Retrace End compares 4 bits against the line counter.
        int span = ((crtc_[0x11] & 0x0F) - vrs) & 0x0F;
        vre_line_ = vrs + (span ? span : 16);
    }
    while (raster_ >= frame_cycles_) raster_ -= frame_cycles_;
}

int Ega::cpu_access_clocks(uint32_t addr, uint64_t now) {
    if (window_offset(addr) == kOutOfWindow) return 0;
    // The CRT takes 4 of 5 cycles, or 2 of 5 with Clocking Mode bit 1 set (EGA TR).
    // The TR doesn't say which of the five are the CPU's.
    static constexpr int kHighRes[] = {4}, kMediumRes[] = {1, 3, 4};
    bool medium = sequencer_[1] & 0x02;
    const int *slots = medium ? kMediumRes : kHighRes;
    int n = medium ? 3 : 1;
    double frame = 5.0 * mem_cycle_;
    double t = std::max(double(now), next_free_);
    double base = std::floor(t / frame) * frame;
    double start = base + frame + slots[0] * mem_cycle_;
    for (int i = n - 1; i >= 0; --i) {
        double s = base + slots[i] * mem_cycle_;
        if (s >= t) start = s;
    }
    next_free_ = start + mem_cycle_;
    return int(std::ceil(next_free_ - double(now)));
}

void Ega::tick(uint64_t cpu_cycles) {
    uint64_t d = cpu_cycles - prev_cycles_;
    prev_cycles_ = cpu_cycles;
    double retrace = vrs_line_ * line_cycles_;
    bool crossed = d >= uint64_t(frame_cycles_) ||
                   (raster_ < retrace ? raster_ + double(d) >= retrace
                                      : raster_ + double(d) >= retrace + frame_cycles_);
    raster_ += double(d);
    while (raster_ >= frame_cycles_) { raster_ -= frame_cycles_; ++frame_count_; }
    // Latched at the start of vertical retrace while CRTC 11h bit 5 is 0 and bit 4 is 1 (EGA TR, DOSBox vga_draw.cpp).
    if (crossed && (crtc_[0x11] & 0x30) == 0x10) vint_ = true;
}

}  // namespace ibmpcat
