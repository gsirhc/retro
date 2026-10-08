// IBM Enhanced Graphics Adapter.
//
// Decodes 0xA0000-0xBFFFF (graphics at A0000, color text at B8000, mono text
// at B0000) and the EGA register set: CRTC 0x3D4/5, Sequencer 0x3C4/5,
// Graphics Controller 0x3CE/F, Attribute Controller 0x3C0 (flip-flop reset by
// reading 0x3DA), Input Status 1 0x3DA, Misc Output 0x3C2. The CRTC and Input
// Status 1 sit at 0x3Bx instead when Misc Output bit 0 is clear.
//
// VRAM is 4 bitplanes of 64KB, byte-interleaved as vram[(plane_offset << 2) + plane].
// The CPU's plane offset is its address with A0 multiplexed under odd/even
// chaining; the display's is the CRTC address in byte or word mode. Reads load
// all 4 planes into a latch. Writes run through Set/Reset, the rotate ALU and
// Bit Mask, gated by Map Mask and odd/even parity (86Box vid_ega.c,
// IBM_PCAT_REVIEW.md §49).
#ifndef IBMPCAT_EGA_H
#define IBMPCAT_EGA_H

#include <array>
#include <cstdint>

namespace ibmpcat {

class Ega {
public:
    void reset();

    bool owns_port(uint16_t port) const;
    uint8_t in(uint16_t port);
    void out(uint16_t port, uint8_t v);

    bool owns_mem(uint32_t addr) const { return addr >= 0xA0000 && addr <= 0xBFFFF; }
    uint8_t mem_read(uint32_t addr) const;
    void mem_write(uint32_t addr, uint8_t v);

    // Advances the raster against the CPU clock. Input Status 1 reads vertical
    // retrace and display enable from it.
    void tick(uint64_t cpu_cycles);

    // Vertical retrace interrupt latch, on the card's IRQ2 pin (IRQ9 on the AT).
    bool vertical_interrupt() const { return vint_; }

    // Configuration switches, bit n = switch n+1, 1 when open. 9 is an Enhanced
    // Display in high-resolution mode (IBM EGA Technical Reference, switch settings).
    void set_switches(uint8_t v) { switches_ = uint8_t(v & 0x0F); }

    // CPU clocks a display-memory byte access starting at `now` takes to land in
    // the next processor memory cycle the sequencer grants. IBM_PCAT_REVIEW.md §47.
    int cpu_access_clocks(uint32_t addr, uint64_t now);

    // CRTC cursor position and display start address (16-bit register pairs).
    uint16_t cursor_offset() const { return uint16_t((crtc_[0x0E] << 8) | crtc_[0x0F]); }
    uint16_t start_offset() const { return uint16_t((crtc_[0x0C] << 8) | crtc_[0x0D]); }

    uint8_t cursor_start_scanline() const { return uint8_t(crtc_[0x0A] & 0x1F); }
    uint8_t cursor_end_scanline() const { return uint8_t(crtc_[0x0B] & 0x1F); }
    // The end register is the row after the last; at or below the start the
    // cursor runs to the bottom of the cell (IBM EGA BIOS listing, CALC_CURSOR).
    bool cursor_on_row(int row) const {
        int start = cursor_start_scanline(), end = cursor_end_scanline();
        return row >= start && (end <= start || row < end);
    }
    int cursor_skew() const { return (crtc_[0x0B] >> 5) & 3; }
    int crtc_underline_row() const { return crtc_[0x14] & 0x1F; }

    // Vertical frames since reset. Cursor and blinking characters both toggle
    // every 16 (86Box vid_ega.c).
    uint32_t frame_count() const { return frame_count_; }
    bool cursor_blink_phase_on() const { return (frame_count_ & 16) == 0; }
    bool char_blink_phase_on() const { return (frame_count_ & 16) == 0; }

    // Attribute Controller palette register (0-15), a 6-bit EGA color.
    uint8_t attr_palette(int index) const { return uint8_t(attr_[index & 0x0F] & 0x3F); }
    // AR10 bit 1: monochrome attributes, bit 2: line graphics, bit 3: blink.
    bool attr_mono() const { return (attr_[0x10] >> 1) & 1; }
    bool attr_line_graphics() const { return (attr_[0x10] >> 2) & 1; }
    bool attr_blink_enabled() const { return (attr_[0x10] >> 3) & 1; }
    uint8_t attr_plane_enable() const { return uint8_t(attr_[0x12] & 0x0F); }
    uint8_t attr_pel_pan() const { return uint8_t(attr_[0x13] & 0x0F); }

    bool seq_8dot_chars() const { return sequencer_[1] & 1; }
    // SR03: map A (bits 2-3) for attribute bit 3 set, map B (bits 0-1) for clear.
    // Without Memory Mode bit 1 (more than 64KB) only map 0 exists.
    int seq_char_map_a() const { return (sequencer_[4] & 2) ? (sequencer_[3] >> 2) & 3 : 0; }
    int seq_char_map_b() const { return (sequencer_[4] & 2) ? sequencer_[3] & 3 : 0; }

    // Line Compare: 9 bits, bit 8 in Overflow bit 4.
    int crtc_line_compare() const { return crtc_[0x18] | ((crtc_[0x07] >> 4 & 1) << 8); }
    int crtc_preset_row_scan() const { return crtc_[0x08] & 0x1F; }
    // CRTC 17h bit 0 clear puts row scan bit 0 on MA13: CGA's two 8KB banks.
    bool crtc_cga_banks() const { return (crtc_[0x17] & 1) == 0; }
    bool crtc_row_scan_ma14() const { return (crtc_[0x17] & 2) == 0; }
    bool crtc_running() const { return (crtc_[0x17] & 0x80) != 0; }
    // Plane offset the display fetches for CRTC address `ma` on row scan line `row_scan`:
    // byte or word mode (CRTC 17h bit 6) and the row scan banks (86Box vid_ega_render_remap.h).
    uint32_t display_address(uint32_t ma, int row_scan) const;

    // Switches 4, 5 and A-F describe a monochrome display (IBM EGA TR, switch settings).
    bool mono_display() const { return switches_ == 4 || switches_ == 5 || switches_ >= 0x0A; }

    // GR06 bit 0: graphics versus alphanumeric addressing.
    bool graphics_mode_active() const { return (gfx_[6] & 0x01) != 0; }

    // GR05 bit 5: CGA-compatible 2-bit shift for modes 4 and 5 (IBM EGA TR, Mode Register).
    bool gc_cga_shift() const { return (gfx_[5] >> 5) & 1; }

    // Horizontal Display End (R01, character clocks; EGA graphics use an 8-dot
    // clock) and Vertical Display End (R12 plus overflow bit 1 of R07). Checked
    // against the BIOS's mode 0x10 programming (IBM_PCAT_REVIEW.md §16).
    //
    // Only one overflow bit is used. Real EGA R07 has one bit per counter (bit 0
    // Vertical Total, 1 Vertical Display End, 2 Retrace Start, 3 Start Blanking,
    // 4 Line Compare). Bits 5-7 are VGA extensions. Folding in R07 bit 6 (VGA
    // Display End bit 9) made the range jump 512 lines (Prince of Persia went black).
    uint16_t crtc_horizontal_display_end() const { return crtc_[0x01]; }
    uint16_t crtc_vertical_display_end() const {
        return uint16_t(crtc_[0x12] | ((crtc_[0x07] >> 1 & 1) << 8));
    }
    // Maximum Scan Line (R09 bits 0-4): scan lines per character row minus 1.
    uint8_t crtc_max_scan_line() const { return uint8_t(crtc_[0x09] & 0x1F); }

    // Offset Register (R13): per-scanline stride in words per plane, independent
    // of Horizontal Display End (panning and off-screen buffers rely on this).
    uint8_t crtc_offset() const { return crtc_[0x13]; }
    // Per-plane byte stride between scanlines (crtc_offset() * 2).
    int crtc_scanline_stride() const { return int(crtc_[0x13]) * 2; }

    // 256KB planar VRAM, layout in the file header.
    std::array<uint8_t, 256 * 1024> vram{};

private:
    // Offset within whichever window GR06 bits 2-3 selects: 128K@A0000,
    // 64K@A0000, 32K@B0000 (mono) or 32K@B8000 (color). kOutOfWindow for
    // addresses outside it.
    static constexpr uint32_t kOutOfWindow = 0xFFFFFFFF;
    uint32_t window_offset(uint32_t addr) const;
    uint32_t cpu_plane_offset(uint32_t window_off) const;
    uint16_t color_port(uint16_t port) const;

    // Register field accessors over the raw indexed register arrays.
    uint8_t seq_map_mask() const { return uint8_t(sequencer_[2] & 0x0F); }
    bool seq_odd_even_disabled() const { return (sequencer_[4] >> 2) & 1; }

    uint8_t gc_set_reset() const { return uint8_t(gfx_[0] & 0x0F); }
    uint8_t gc_enable_set_reset() const { return uint8_t(gfx_[1] & 0x0F); }
    uint8_t gc_color_compare() const { return uint8_t(gfx_[2] & 0x0F); }
    uint8_t gc_rotate_count() const { return uint8_t(gfx_[3] & 0x07); }
    uint8_t gc_raster_op() const { return uint8_t((gfx_[3] >> 3) & 0x03); }
    uint8_t gc_read_map_select() const { return uint8_t(gfx_[4] & 0x03); }
    uint8_t gc_write_mode() const { return uint8_t(gfx_[5] & 0x03); }
    bool gc_read_mode1() const { return (gfx_[5] >> 3) & 1; }
    uint8_t gc_color_dont_care() const { return uint8_t(gfx_[7] & 0x0F); }
    uint8_t gc_bit_mask() const { return gfx_[8]; }
    uint8_t gc_memory_mapping() const { return uint8_t((gfx_[6] >> 2) & 0x03); }

    static uint8_t rotate_right8(uint8_t v, uint8_t count) {
        count &= 7;
        return uint8_t((v >> count) | (v << ((8 - count) & 7)));
    }

    std::array<uint8_t, 25> crtc_{};
    uint8_t crtc_index_ = 0;

    std::array<uint8_t, 5> sequencer_{};
    uint8_t sequencer_index_ = 0;

    std::array<uint8_t, 9> gfx_{};
    uint8_t gfx_index_ = 0;

    std::array<uint8_t, 21> attr_{};
    uint8_t attr_index_ = 0;
    bool attr_flip_flop_addr_ = true;  // next 0x3C0 write is an index when true, data when false

    uint8_t misc_output_ = 0;

    // Read latch. Loaded on every read, mutable because reads have this side effect.
    mutable std::array<uint8_t, 4> latch_{};

    // Raster timing in CPU cycles from the CRTC and clock registers (IBM EGA
    // Technical Reference). See IBM_PCAT_REVIEW.md §46.4.
    static constexpr double kCpuHz = 8000000.0;
    void recompute_timing_();
    double line_cycles_ = 0.0, frame_cycles_ = 0.0, hde_cycles_ = 0.0;
    int vde_lines_ = 0, vrs_line_ = 0, vre_line_ = 0;
    uint64_t prev_cycles_ = 0;
    double raster_ = 0.0;  // cycles into the current frame
    double mem_cycle_ = 0.0;  // CPU clocks per display-memory cycle
    double next_free_ = 0.0;  // end of the last processor cycle granted
    uint32_t frame_count_ = 0;
    bool vint_ = false;
    uint8_t switches_ = 0x09;
};

}  // namespace ibmpcat

#endif  // IBMPCAT_EGA_H
