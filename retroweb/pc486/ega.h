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
// pointed at a different pair of bits).
//
// Milestone 3 adds the three pieces of genuine VGA (not EGA) silicon this
// machine's card actually has, which Milestone 1 stopped short of:
//
//   - Chain 4 (Sequencer Memory Mode, SR04 bit 3): mode 13h's addressing.
//     CPU address bits 0-1 select the plane and the per-plane offset is
//     addr>>2, so four consecutive CPU bytes land at the same offset in
//     planes 0,1,2,3 -- one byte per pixel from software's point of view.
//     Because this file's VRAM is stored byte-interleaved as
//     vram[(plane_offset << 2) + plane], chain-4's decode collapses to
//     vram[offset] exactly: ((off>>2)<<2) + (off&3) == off. That identity
//     is not a shortcut, it IS why real VGA's interleaved planes make mode
//     13h look linear.
//   - The 256-entry RGB DAC (ports 0x3C6-0x3C9): 6 significant bits per
//     channel, a PEL mask, separate read/write index registers each with
//     their own R->G->B sub-counter that auto-advances to the next entry
//     on the third access, and the DAC State register (0x3C7 read).
//   - This card's SVGA extension registers, which its own ROM BIOS drives
//     to implement the VESA BIOS Extensions -- see the VBE/DISPI block
//     near the bottom of this class and PC486_REVIEW.md §7.
//
// Register semantics are the IBM EGA/VGA standard, reproduced identically
// by every compatible BIOS (including this machine's Bochs vgabios) since
// software that pokes these registers directly depends on exact IBM
// compatibility; the memory-access algorithm additionally matches Bochs's
// own reference implementation (bx_vgacore_c::mem_read/mem_write in
// vgacore.cc, pinned commit ff17a0c2bbabccf96d33af4e08ba8061889b079d --
// the same source tree this machine's own BIOS/VGABIOS images are built
// from). See ibmpc-at/IBM_PCAT_REVIEW.md §12 (the planar engine this class was
// adapted from) and PC486_REVIEW.md §7.
#ifndef PC486_EGA_H
#define PC486_EGA_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace pc486 {

class Ega {
public:
    void reset();

    bool owns_port(uint16_t port) const;
    uint8_t in(uint16_t port);
    void out(uint16_t port, uint8_t v);

    // The SVGA extension index/data ports are inherently 16-bit registers
    // at a single address -- the card's own ROM drives them with `out dx,
    // ax` / `in ax, dx` -- so they need the same single-bus-cycle handling
    // the IDE data registers get, not two composed byte accesses. See
    // chipset.cpp's io_in16/io_out16 and cpu80486.h's Bus::in16 comment.
    static bool owns_port16(uint16_t port) { return port == kVbeIndexPort || port == kVbeDataPort; }
    uint16_t in16(uint16_t port);
    void out16(uint16_t port, uint16_t v);

    bool owns_mem(uint32_t addr) const { return addr >= 0xA0000 && addr <= 0xBFFFF; }
    uint8_t mem_read(uint32_t addr) const;
    void mem_write(uint32_t addr, uint8_t v);

    // Advances the Input Status 1 retrace toggle against the CPU's running
    // cycle count -- so a BIOS/driver's "wait for vertical retrace" polling
    // loop can't hang. Not a real ~70Hz refresh timing; just enough
    // liveness that the bit visibly changes over a bounded number of ticks.
    //
    // Inline for the same reason as Fdc765::tick: Machine::run_cycles()
    // calls it after every instruction. See PC486_REVIEW.md §8 -- including
    // the note that kFramePeriod's 8 MHz numerator is inherited from
    // ibmpc-at and is NOT this machine's clock.
    void tick(uint64_t cpu_cycles) {
        uint64_t d = cpu_cycles - prev_cycles_;
        prev_cycles_ = cpu_cycles;
        retrace_credit_ += double(d);
        constexpr double kFramePeriod = 8000000.0 / 60.0;
        constexpr double kRetraceWindow = kFramePeriod * 0.08;
        while (retrace_credit_ >= kFramePeriod) retrace_credit_ -= kFramePeriod;
        retrace_ = retrace_credit_ < kRetraceWindow;
    }

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

    // --- VGA DAC (ports 0x3C6-0x3C9) --------------------------------------
    // One of the 256 DAC colour registers, as the three RAW 6-bit channel
    // values the hardware actually stores (0-63 each). A renderer scales
    // them to whatever its output wants; nothing here presumes 8-bit RGB,
    // because the real part doesn't -- 6 bits per channel drive three
    // analog ramps, and 63 is full scale.
    void dac_entry(int index, uint8_t &r, uint8_t &g, uint8_t &b) const {
        std::size_t i = std::size_t(index & 0xFF) * 3;
        r = dac_[i + 0]; g = dac_[i + 1]; b = dac_[i + 2];
    }
    // PEL Mask (0x3C6): ANDed with every pixel value on its way from the
    // shift registers into the DAC's address lines, so it can blank or
    // fold the palette without touching a single colour register. Real
    // period software uses it for fades and for 16-colour-in-256-mode
    // tricks; a renderer must apply it, not just store it.
    uint8_t dac_mask() const { return dac_mask_; }

    // Attribute Controller Mode Control (AR10) bit 6, "8-bit colour": in
    // 256-colour modes the VGA clocks two dot-clocks per pixel, so a CRTC
    // programmed for 640 dots displays 320 pixels. This bit is how a real
    // CRT controller knows that, and how the renderer derives mode 13h's
    // 320 from the same Horizontal Display End=79 a 640-wide mode uses.
    bool attr_8bit_color() const { return (attr_[0x10] >> 6) & 1; }

    // --- CRTC address-unit selection --------------------------------------
    // The CRTC's memory address counter is not always a byte counter. Two
    // register bits scale it, and every VGA mode relies on the result:
    //   Underline Location (R14) bit 6 = Doubleword Mode -> 4 bytes/unit
    //   Mode Control (R17) bit 6 = Byte Mode (1) -> 1 byte/unit,
    //                              cleared means Word Mode -> 2 bytes/unit
    // Doubleword mode wins when both are set, matching the IBM VGA
    // hardware description. This is what makes mode 13h's Offset Register
    // value of 40 mean a 320-byte scan line (40 * 2 * 4) while mode 10h's
    // identical 40 means an 80-byte one (40 * 2 * 1, byte mode) -- the
    // stride genuinely comes from the registers, not from a mode table.
    bool crtc_dword_mode() const { return (crtc_[0x14] >> 6) & 1; }
    bool crtc_byte_mode() const { return (crtc_[0x17] >> 6) & 1; }
    int crtc_address_unit_bytes() const {
        if (crtc_dword_mode()) return 4;
        return crtc_byte_mode() ? 1 : 2;
    }
    // Bytes between the start of one scan line and the next, with the
    // address-unit scaling above applied -- the figure a byte-per-pixel
    // renderer walks VRAM by. crtc_scanline_stride() above stays the
    // per-plane (unscaled) figure the planar renderer wants.
    int crtc_row_byte_stride() const { return int(crtc_[0x13]) * 2 * crtc_address_unit_bytes(); }
    // Byte offset of the first displayed pixel, same scaling applied to
    // the CRTC Start Address register pair.
    uint32_t start_byte_offset() const { return uint32_t(start_offset()) * uint32_t(crtc_address_unit_bytes()); }

    // --- VESA BIOS Extensions -------------------------------------------
    // This card's SVGA extension registers: an index port (0x1CE) and a
    // data port (0x1CF) exposing a small bank of 16-bit registers, which
    // the card's own ROM BIOS drives to implement VBE (INT 10h AX=4Fxx).
    //
    // DEPARTURE, CLEARLY LABELLED (CLAUDE.md's substitution rule): every
    // real SVGA card of this era implemented VBE in exactly this shape --
    // vendor-specific extension registers that only the card's own ROM
    // knew about (Tseng's ET4000 extended CRTC set, Cirrus's, S3's) -- but
    // the *specific* register numbers below are not any of those. They are
    // the interface expected by the freely-licensed VGA BIOS this machine
    // substitutes for IBM's still-copyrighted one (see roms/fetch-bios.sh),
    // and they only exist here because that firmware is the card's ROM.
    // The card is a compatible stand-in, not a clone of a named 1993 part.
    // See PC486_REVIEW.md §7.
    static constexpr uint16_t kVbeIndexPort = 0x01CE;
    static constexpr uint16_t kVbeDataPort  = 0x01CF;
    enum VbeReg : uint16_t {
        kVbeRegId = 0x0, kVbeRegXres = 0x1, kVbeRegYres = 0x2, kVbeRegBpp = 0x3,
        kVbeRegEnable = 0x4, kVbeRegBank = 0x5, kVbeRegVirtWidth = 0x6,
        kVbeRegVirtHeight = 0x7, kVbeRegXOffset = 0x8, kVbeRegYOffset = 0x9,
        kVbeRegVideoMemory64K = 0xA, kVbeRegCount = 0xB,
    };
    // Enable-register bits. GETCAPS is a query mode, not a display mode:
    // while it is set, reading the XRES/YRES/BPP registers reports the
    // card's MAXIMA instead of the current mode's values, which is how the
    // card's ROM discovers which VESA modes this board can actually do
    // before it will list them (vgabios's dispi_get_max_xres/yres/bpp,
    // called from mode_info_check_mode -- see PC486_REVIEW.md §7).
    static constexpr uint16_t kVbeEnabled    = 0x01;
    static constexpr uint16_t kVbeGetCaps    = 0x02;
    static constexpr uint16_t kVbeNoClearMem = 0x80;
    // What this board can actually do in a linear 8-bit-per-pixel mode:
    // 640x400 is the largest such frame that fits in its 256KB of VRAM
    // (640*400 = 256,000 <= 262,144), and 8bpp is the only depth its DAC
    // path handles. Reporting anything larger would have the ROM advertise
    // modes the card cannot display.
    static constexpr uint16_t kVbeMaxXres = 640;
    static constexpr uint16_t kVbeMaxYres = 400;
    static constexpr uint16_t kVbeMaxBpp  = 8;
    // Only these IDs are accepted into the ID register, so a probe that
    // writes an unknown value and reads it back correctly concludes this
    // card does not speak that revision -- the whole point of the probe.
    static constexpr uint16_t kVbeIdLowest  = 0xB0C0;
    static constexpr uint16_t kVbeIdHighest = 0xB0C5;

    uint16_t vbe_reg(int index) const {
        return index >= 0 && index < kVbeRegCount ? vbe_[std::size_t(index)] : uint16_t(0);
    }
    // Whether an SVGA (VBE-programmed, linear byte-per-pixel) mode is
    // currently switched on, as opposed to one of the legacy VGA modes the
    // CRTC/Sequencer/Graphics-Controller registers describe.
    bool vbe_mode_active() const {
        return (vbe_[kVbeRegEnable] & kVbeEnabled) != 0 && vbe_[kVbeRegBpp] == 8;
    }
    // Size of the window the Bank register slides over VRAM. 64KB is the
    // whole 0xA0000 aperture, which is what the ROM reports to software as
    // both WinGranularity and WinSize in every ModeInfoBlock it builds.
    static constexpr uint32_t kVbeBankSize = 65536;

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
    // reading back exactly what it programs -- see
    // ibmpc-at/IBM_PCAT_REVIEW.md §16), 1 selects "Shift 2 4-color" (the
    // CGA-compatibility mode: each
    // memory cycle's plane-0 byte then plane-1 byte, each read as four
    // 2-bit CGA-style pixels in turn), and 2 selects VGA's 256-color
    // shift-out -- mode 13h, one byte per pixel straight into the DAC.
    // Genuine 1984 EGA silicon has no hardware for value 2, but this
    // machine's card is a VGA (see this file's header and PC486_REVIEW.md
    // §7), so it does. Value 3 is not a mode real VGA silicon defines.
    uint8_t gc_shift_register_mode() const { return uint8_t((gfx_[5] >> 5) & 0x03); }

    // Host/front-end convenience: the CRTC registers that determine a
    // graphics mode's actual resolution -- Horizontal Display End
    // (register 0x01, in character clocks; genuine EGA graphics modes
    // always use an 8-dot character clock) and Vertical Display End
    // (register 0x12, plus its one overflow bit in register 0x07 bit 1) --
    // what a real CRT controller's own scanout timing is built from, not a
    // BIOS video-mode-number guess. Verified against this machine's own
    // BIOS's real mode-0x10 (640x350x16) register programming -- see
    // ibmpc-at/IBM_PCAT_REVIEW.md §16.
    //
    // Deliberately only ONE overflow bit: on genuine 1984 EGA silicon, CRTC
    // Overflow (R07) defines exactly one bit per counter that can exceed 8
    // bits (bit 0 = Vertical Total, bit 1 = Vertical Display End, bit 2 =
    // Vertical Retrace Start, bit 3 = Start Vertical Blanking, bit 4 = Line
    // Compare) -- 9 bits tops, plenty for EGA's max 350 lines. Bits 5-7 are
    // unimplemented on real EGA hardware; VGA later reused them as a second
    // overflow bit per counter (a 10-bit extension, for its taller modes).
    // Deliberately excludes register 0x07 bit 6, which VGA (not real EGA)
    // reuses as a "Vertical Display End bit 9" -- this machine's BIOS
    // substitute is a full VGA BIOS (see gc_shift_register_mode() above), so
    // VGA-aware software can legitimately set that bit, but a real EGA CRTC
    // has no wire to read it back on. Folding it in made the vertical range
    // jump 512 lines on real-hardware-targeted software (confirmed live:
    // Prince of Persia's playfield rendering correctly, then black canvas).
    // See PC486_REVIEW.md.
    uint16_t crtc_horizontal_display_end() const { return crtc_[0x01]; }
    uint16_t crtc_vertical_display_end() const {
        return uint16_t(crtc_[0x12] | ((crtc_[0x07] >> 1 & 1) << 8));
    }
    // Maximum Scan Line (CRTC R09, bits 0-4): scan lines per character row
    // minus 1 -- what a real CRT controller's own text-mode row pitch is
    // built from, not a hardcoded per-mode constant. See
    // PC486_REVIEW.md's font-descender investigation.
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
    // RenderEgaNative16Screen() in ega_render.cpp and PC486_REVIEW.md.
    bool crtc_scan_doubling() const { return (crtc_[0x09] >> 7) & 1; }

    // Offset Register (CRTC R13): the real per-scanline memory stride, in
    // WORDS (2 bytes) per plane -- genuinely independent of Horizontal
    // Display End. A real CRT controller advances exactly this many bytes
    // between scanlines regardless of how much of that row is actually
    // displayed; software that programs a logical scan-line width wider
    // than what it shows (panning, or a sub-window blit into a larger
    // off-screen buffer) relies on this distinction. See
    // crtc_scanline_stride() below and PC486_REVIEW.md.
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
    // In an SVGA mode the planar engine is bypassed entirely: the 0xA0000
    // aperture is a plain 64KB window onto a linear byte-per-pixel frame
    // buffer, positioned by the Bank register. Returns the linear VRAM
    // offset, or kOutOfWindow.
    uint32_t vbe_linear_offset(uint32_t addr) const;

    // Register field accessors -- decode straight from the raw indexed
    // register arrays below (the single source of truth, also what in()/
    // out() read and write directly), so there's no duplicated state to
    // fall out of sync.
    uint8_t seq_map_mask() const { return uint8_t(sequencer_[2] & 0x0F); }
    bool seq_odd_even_disabled() const { return (sequencer_[4] >> 2) & 1; }
    // Sequencer Memory Mode (SR04) bit 3, Chain 4 -- see the file header.
    // Overrides odd/even chaining when both are somehow set, matching the
    // real part's addressing priority.
    bool seq_chain4() const { return (sequencer_[4] >> 3) & 1; }

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

    // DAC colour RAM: 256 entries x {R,G,B}, 6 significant bits each.
    // Separate read and write index registers, each with its own R->G->B
    // sub-counter -- real hardware keeps the two sides independent, so a
    // driver reading one entry mid-way through writing another doesn't
    // corrupt either.
    std::array<uint8_t, 256 * 3> dac_{};
    uint8_t dac_write_index_ = 0;
    uint8_t dac_read_index_ = 0;
    uint8_t dac_write_sub_ = 0;
    uint8_t dac_read_sub_ = 0;
    // DAC State (0x3C7 read): 3 = the last index write was to the read
    // register (0x3C7), 0 = to the write register (0x3C8).
    uint8_t dac_state_ = 0;
    uint8_t dac_mask_ = 0xFF;

    std::array<uint16_t, kVbeRegCount> vbe_{};
    uint16_t vbe_index_ = 0;
    uint16_t vbe_read_(int index) const;
    void vbe_write_(int index, uint16_t v);

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

}  // namespace pc486

#endif  // PC486_EGA_H
