// IBM Enhanced Graphics Adapter.
//
// Owns the 256KB video RAM window (0xA0000-0xBFFFF... the card itself only
// decodes 0xA0000-0xAFFFF for graphics and 0xB8000-0xBFFFF for text/CGA
// compatibility; 0xB0000-0xB7FFF is the MDA-compatible mono text window,
// unused by this color-display machine but still decoded so a write there
// doesn't fall through to conventional RAM) and the standard EGA register
// set: CRTC (0x3D4/0x3D5), Sequencer (0x3C4/0x3C5), Graphics Controller
// (0x3CE/0x3CF), Attribute Controller (0x3C0, address/data toggled by
// writes and reset by reading Input Status 1), Input Status 1 (0x3DA),
// and the Miscellaneous Output Register (0x3C2 write / 0x3CC read).
//
// Phase 5: real planar memory. VRAM is 4 bitplanes of 64KB each, byte-
// interleaved as vram[(plane_offset << 2) + plane] -- the genuine EGA/VGA
// hardware layout, not an approximation of it. mem_read/mem_write implement
// the real latch-and-ALU engine: every read loads all 4 planes' bytes into
// a 4-byte latch (Read Mode 0 then returns one plane, substituted by the
// CPU address's own odd/even-ness when the Sequencer's odd/even chain is
// active, exactly like text mode's character/attribute split; Read Mode 1
// does a 4-plane colour-compare instead); every write runs the CPU byte (or
// the latch, or bit-per-plane selection, depending on Write Mode) through
// Set/Reset, the data-rotate ALU function, and the Bit Mask before storing,
// gated per-plane by the Sequencer's Map Mask and -- when odd/even chaining
// is active -- by the CPU address's own parity, which is what lets a flat
// CPU address transparently reach alternating planes without the CPU or
// BIOS ever needing to think in terms of planes at all (used by text mode
// for character/attribute, and by the BIOS's own character-generator/font-
// loading code targeting planes 2/3 the same way, just with Map Mask
// pointed at a different pair of bits). Chain Four (VGA mode 13h's chained-
// pixel addressing) does not exist on real EGA hardware and is deliberately
// not modeled here -- see IBM_PCAT_REVIEW.md §12.
//
// Register semantics are the IBM EGA/VGA standard, reproduced identically
// by every compatible BIOS (including this machine's Bochs vgabios) since
// software that pokes these registers directly depends on exact IBM
// compatibility; the memory-access algorithm additionally matches Bochs's
// own reference implementation (bx_vgacore_c::mem_read/mem_write in
// vgacore.cc, pinned commit ff17a0c2bbabccf96d33af4e08ba8061889b079d --
// the same source tree this machine's own BIOS/VGABIOS images are built
// from). See IBM_PCAT_REVIEW.md §12.
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

    // Advances the Input Status 1 retrace toggle against the CPU's running
    // cycle count -- so a BIOS/driver's "wait for vertical retrace" polling
    // loop can't hang. Not a real ~70Hz refresh timing; just enough
    // liveness that the bit visibly changes over a bounded number of ticks.
    void tick(uint64_t cpu_cycles);

    // Host/front-end convenience for a future renderer: current CRTC
    // cursor position and display start address (both are 16-bit CRTC
    // register pairs, offsets into the text-mode VRAM window).
    uint16_t cursor_offset() const { return uint16_t((crtc_[0x0E] << 8) | crtc_[0x0F]); }
    uint16_t start_offset() const { return uint16_t((crtc_[0x0C] << 8) | crtc_[0x0D]); }

    // Cursor shape, CRTC registers 0x0A (Cursor Start: bit 5 = disable,
    // bits 0-4 = start scanline) / 0x0B (Cursor End: bits 0-4 = end
    // scanline) -- what a renderer needs to draw the real block cursor at
    // the right scanlines within a character cell, or not draw it at all.
    bool cursor_disabled() const { return (crtc_[0x0A] >> 5) & 1; }
    uint8_t cursor_start_scanline() const { return uint8_t(crtc_[0x0A] & 0x1F); }
    uint8_t cursor_end_scanline() const { return uint8_t(crtc_[0x0B] & 0x1F); }

    // Host/front-end convenience: one Attribute Controller internal
    // palette register (0-15), the real 6-bit EGA color value (2 bits per
    // channel -- primary + secondary/intensity, each channel decoding as
    // primary*0xAA + secondary*0x55) that byte-attribute nibble maps to.
    // A renderer combines this with the character-generator bits in plane
    // 2 (also public, via `vram`) to paint an actual screen.
    uint8_t attr_palette(int index) const { return uint8_t(attr_[index & 0x0F] & 0x3F); }

    // Host/front-end convenience: whether the Graphics Controller's own
    // Miscellaneous register currently selects graphics addressing over
    // alphanumeric (GR06 bit 0) -- a renderer's first branch, deciding
    // between a text-mode and a graphics-mode screen, exactly like a real
    // CRT controller's own mode logic.
    bool graphics_mode_active() const { return (gfx_[6] & 0x01) != 0; }

    // Host/front-end convenience: the Graphics Controller Mode register's
    // Shift Register field (GR05 bits 5-6) -- 0 selects normal 16-color
    // planar shift-out (real, native EGA graphics: verified by directly
    // invoking this machine's own BIOS INT 10h AL=0x10 mode-set and
    // reading back exactly what it programs -- see IBM_PCAT_REVIEW.md
    // §16), 1 selects "Shift 2 4-color" (the CGA-compatibility mode: each
    // memory cycle's plane-0 byte then plane-1 byte, each read as four
    // 2-bit CGA-style pixels in turn). Genuine 1984 EGA silicon has no
    // hardware for value 2 (VGA's 256-color Chain-4 mode) -- but this
    // machine's freely-licensed BIOS substitute is a full VGA BIOS (see
    // IBM_PCAT_REVIEW.md §6), so software that auto-detects and finds
    // VGA-class capability can and does legitimately program it anyway
    // (confirmed happening with a real commercial game); that's a real
    // firmware/hardware mismatch this machine's real EGA device correctly
    // can't display, not a rendering gap to fill.
    uint8_t gc_shift_register_mode() const { return uint8_t((gfx_[5] >> 5) & 0x03); }

    // Host/front-end convenience: the CRTC registers that determine a
    // graphics mode's actual resolution -- Horizontal Display End
    // (register 0x01, in character clocks; genuine EGA graphics modes
    // always use an 8-dot character clock) and Vertical Display End
    // (register 0x12, plus its one overflow bit in register 0x07 bit 1) --
    // what a real CRT controller's own scanout timing is built from, not a
    // BIOS video-mode-number guess. Verified against this machine's own
    // BIOS's real mode-0x10 (640x350x16) register programming -- see
    // IBM_PCAT_REVIEW.md §16.
    //
    // Deliberately only ONE overflow bit: on genuine 1984 EGA silicon, CRTC
    // Overflow (R07) defines exactly one bit per counter that can exceed 8
    // bits (bit 0 = Vertical Total, bit 1 = Vertical Display End, bit 2 =
    // Vertical Retrace Start, bit 3 = Start Vertical Blanking, bit 4 = Line
    // Compare) -- 9 bits tops, plenty for EGA's max 350 lines. Bits 5-7 are
    // unimplemented on real EGA hardware; VGA later reused them as a second
    // overflow bit per counter (a 10-bit extension, for its taller modes).
    // A prior version of this accessor folded register 0x07 bit 6 in as a
    // "Vertical Display End bit 9", which is exactly that VGA-only meaning
    // -- and since this machine's freely-licensed BIOS substitute is a full
    // VGA BIOS (see the gc_shift_register_mode() comment above for the same
    // phenomenon with Chain-4), software that detects VGA-class capability
    // can and does legitimately set that bit. A real EGA card's CRTC simply
    // has no thirteenth wire for it to land on, so real hardware would
    // never see the vertical range jump by 512 lines the way reading it
    // back here used to -- confirmed live as Prince of Persia's playfield
    // rendering correctly followed by several hundred lines of pure black
    // canvas. See IBM_PCAT_REVIEW.md.
    uint16_t crtc_horizontal_display_end() const { return crtc_[0x01]; }
    uint16_t crtc_vertical_display_end() const {
        return uint16_t(crtc_[0x12] | ((crtc_[0x07] >> 1 & 1) << 8));
    }
    // Maximum Scan Line (CRTC R09, bits 0-4): scan lines per character row
    // minus 1 -- what a real CRT controller's own text-mode row pitch is
    // built from, not a hardcoded per-mode constant. See
    // IBM_PCAT_REVIEW.md's font-descender investigation.
    uint8_t crtc_max_scan_line() const { return uint8_t(crtc_[0x09] & 0x1F); }

    // Scan Doubling (CRTC R09 bit 7): another VGA-only addition riding on a
    // bit genuine EGA silicon never wired up (same story as the R07
    // overflow bits above, and gc_shift_register_mode()'s Chain-4 note) --
    // when set, a VGA CRTC draws every logical scanline twice in a row so a
    // "200-line" mode fills the same ~400-scanline raster its 350-line
    // modes use. This substitute firmware is VGA-heritage, so its mode-0Dh
    // (320x200x16) setup programs Vertical Total/Display End for the full
    // ~400-line doubled raster AND sets this bit, exactly like a real VGA
    // card -- but genuine EGA hardware has no doubling circuit, so a real
    // EGA BIOS's own 320x200 mode-set programs the CRTC for 200 real
    // scanlines directly, no doubling, nothing to detect here. Confirmed
    // live: Prince of Persia's EGA mode left Vertical Display End at 399
    // (carried over verbatim from the prior 640x400 text mode) with this
    // bit set, expecting the doubling hardware to fold it back down to a
    // 200-line picture -- without this accessor the renderer took 399+1
    // literally, drawing the real 200-line playfield in the top half of a
    // 400-line canvas and leaving the bottom half black. See
    // RenderEgaNative16Screen() in ega_render.cpp and IBM_PCAT_REVIEW.md.
    bool crtc_scan_doubling() const { return (crtc_[0x09] >> 7) & 1; }

    // Offset Register (CRTC R13): the real per-scanline memory stride, in
    // WORDS (2 bytes) per plane -- genuinely independent of Horizontal
    // Display End. A real CRT controller advances exactly this many bytes
    // between scanlines regardless of how much of that row is actually
    // displayed; software that programs a logical scan-line width wider
    // than what it shows (panning, or a sub-window blit into a larger
    // off-screen buffer) relies on this distinction. See
    // crtc_scanline_stride() below and IBM_PCAT_REVIEW.md.
    uint8_t crtc_offset() const { return crtc_[0x13]; }
    // Convenience: the real per-plane byte stride between scanlines
    // (crtc_offset() * 2), with the same "0 means not programmed yet"
    // fallback the other CRTC accessors use -- a renderer should walk
    // VRAM by this, not by (displayed width / 8), whenever it differs.
    int crtc_scanline_stride() const { return int(crtc_[0x13]) * 2; }

    // 256KB planar VRAM: 4 bitplanes x 64KB, byte-interleaved as
    // vram[(plane_offset << 2) + plane] -- see the file header.
    std::array<uint8_t, 256 * 1024> vram{};

private:
    // Decodes a CPU address (already known to be within 0xA0000-0xBFFFF)
    // against the Graphics Controller's Memory Mapping field (GR06 bits
    // 2-3) into an offset local to whichever legacy window is currently
    // selected -- 128K@A0000, 64K@A0000, 32K@B0000 (mono), or 32K@B8000
    // (color). Real hardware only ever decodes ONE of these windows at a
    // time; an address outside the currently-selected window isn't this
    // device's to answer. Returns kOutOfWindow for such an address.
    static constexpr uint32_t kOutOfWindow = 0xFFFFFFFF;
    uint32_t window_offset(uint32_t addr) const;

    // Register field accessors -- decode straight from the raw indexed
    // register arrays below (the single source of truth, also what in()/
    // out() read and write directly), so there's no duplicated state to
    // fall out of sync.
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
    bool attr_flip_flop_addr_ = true;  // true = next 0x3C0 write is an index, false = it's data

    uint8_t misc_output_ = 0;

    // Read latch: real hardware loads all 4 planes' bytes on every memory
    // read, regardless of read mode, and every write mode except direct-
    // CPU-passthrough draws on this latch rather than the CPU's byte. It's
    // a genuine hardware side effect of reading, hence mutable on an
    // otherwise-const mem_read.
    mutable std::array<uint8_t, 4> latch_{};

    bool retrace_ = false;
    uint64_t prev_cycles_ = 0;
    double retrace_credit_ = 0.0;
};

}  // namespace ibmpcat

#endif  // IBMPCAT_EGA_H
