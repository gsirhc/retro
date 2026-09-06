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

    // Host/front-end convenience: one Attribute Controller internal
    // palette register (0-15), the real 6-bit EGA color value (2 bits per
    // channel -- primary + secondary/intensity, each channel decoding as
    // primary*0xAA + secondary*0x55) that byte-attribute nibble maps to.
    // A renderer combines this with the character-generator bits in plane
    // 2 (also public, via `vram`) to paint an actual screen.
    uint8_t attr_palette(int index) const { return uint8_t(attr_[index & 0x0F] & 0x3F); }

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
