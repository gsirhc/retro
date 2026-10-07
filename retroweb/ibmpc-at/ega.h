// IBM Enhanced Graphics Adapter.
//
// Decodes 0xA0000-0xBFFFF (graphics at A0000, color text at B8000, mono text
// at B0000) and the EGA register set: CRTC 0x3D4/5, Sequencer 0x3C4/5,
// Graphics Controller 0x3CE/F, Attribute Controller 0x3C0 (flip-flop reset by
// reading 0x3DA), Input Status 1 0x3DA, Misc Output 0x3C2/0x3CC.
//
// VRAM is 4 bitplanes of 64KB, byte-interleaved as vram[(plane_offset << 2) + plane].
// Reads load all 4 planes into a latch. Read Mode 0 returns one plane (the CPU
// address parity picks it under odd/even chaining), Read Mode 1 does a colour
// compare. Writes run through Set/Reset, the rotate ALU and Bit Mask, gated by
// Map Mask and odd/even parity. Chain Four is VGA-only and not modeled
// (IBM_PCAT_REVIEW.md §12).
//
// The memory algorithm follows Bochs bx_vgacore_c::mem_read/mem_write in
// vgacore.cc (commit ff17a0c2bbabccf96d33af4e08ba8061889b079d, the tree this
// machine's BIOS/VGABIOS are built from). IBM_PCAT_REVIEW.md §12.
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

    // Toggles the Input Status 1 retrace bit so wait-for-retrace loops can't hang.
    // Not real ~70Hz timing.
    void tick(uint64_t cpu_cycles);

    // CRTC cursor position and display start address (16-bit register pairs).
    uint16_t cursor_offset() const { return uint16_t((crtc_[0x0E] << 8) | crtc_[0x0F]); }
    uint16_t start_offset() const { return uint16_t((crtc_[0x0C] << 8) | crtc_[0x0D]); }

    // Cursor shape: CRTC 0x0A (bit 5 = disable, bits 0-4 = start scanline) and 0x0B (bits 0-4 = end).
    bool cursor_disabled() const { return (crtc_[0x0A] >> 5) & 1; }
    uint8_t cursor_start_scanline() const { return uint8_t(crtc_[0x0A] & 0x1F); }
    uint8_t cursor_end_scanline() const { return uint8_t(crtc_[0x0B] & 0x1F); }

    // Attribute Controller palette register (0-15), a 6-bit EGA color.
    uint8_t attr_palette(int index) const { return uint8_t(attr_[index & 0x0F] & 0x3F); }

    // GR06 bit 0: graphics versus alphanumeric addressing.
    bool graphics_mode_active() const { return (gfx_[6] & 0x01) != 0; }

    // GR05 bits 5-6 Shift Register: 0 = 16-color planar (verified against this
    // BIOS's INT 10h AL=0x10, IBM_PCAT_REVIEW.md §16), 1 = CGA-compatible 4-color.
    // Value 2 (VGA 256-color Chain-4) has no EGA hardware, but this VGA BIOS
    // lets software program it anyway.
    uint8_t gc_shift_register_mode() const { return uint8_t((gfx_[5] >> 5) & 0x03); }

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

    // Scan Doubling (R09 bit 7) is VGA-only: each scanline is drawn twice so a
    // 200-line mode fills a ~400-line raster. The VGA-heritage BIOS sets it in
    // mode 0Dh with Vertical Display End 399 (Prince of Persia). See
    // RenderEgaNative16Screen() in ega_render.cpp and IBM_PCAT_REVIEW.md.
    bool crtc_scan_doubling() const { return (crtc_[0x09] >> 7) & 1; }

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

    bool retrace_ = false;
    uint64_t prev_cycles_ = 0;
    double retrace_credit_ = 0.0;
};

}  // namespace ibmpcat

#endif  // IBMPCAT_EGA_H
