// VGA-class adapter (named Ega for historical reasons). 1MB VRAM, standard
// CRTC/Sequencer/Graphics Controller/Attribute Controller/DAC register sets.
// VRAM is 4 byte-interleaved planes, vram[(plane_offset << 2) + plane]. The
// planar latch/ALU engine follows Bochs bx_vgacore_c::mem_read/mem_write
// (vgacore.cc, commit ff17a0c2bbabccf96d33af4e08ba8061889b079d). Chain-4
// decodes to vram[offset] because ((off>>2)<<2) + (off&3) == off, which is why
// mode 13h looks linear. SVGA extension registers back the VBE; see
// PC486_REVIEW.md §7.
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

    // The SVGA index/data ports are 16-bit registers at a single address, so they
    // get one bus cycle like the IDE data registers.
    static bool owns_port16(uint16_t port) { return port == kVbeIndexPort || port == kVbeDataPort; }
    uint16_t in16(uint16_t port);
    void out16(uint16_t port, uint16_t v);

    bool owns_mem(uint32_t addr) const { return addr >= 0xA0000 && addr <= 0xBFFFF; }
    uint8_t mem_read(uint32_t addr) const;
    void mem_write(uint32_t addr, uint8_t v);

    // A 4KB aperture page the CPU may touch as linear bytes, or nullptr. Valid
    // when every planar stage is pass-through (mode 13h). mapping_epoch() changes
    // whenever the answer could.
    uint8_t *linear_page(uint32_t page_base, bool write);
    // Bumped when a register that could change linear_page() is written.
    uint32_t mapping_epoch() const { return mapping_epoch_; }
    // Bumps mapping_epoch() if any register linear_page() consults changed.
    void note_mapping_change();


    // Advances the Input Status 1 retrace toggle. Cached timing keeps this cheap
    // enough to call after every instruction.
    void tick(uint64_t cpu_cycles) {
        uint64_t d = cpu_cycles - prev_cycles_;
        prev_cycles_ = cpu_cycles;
        retrace_credit_ += double(d);
        while (retrace_credit_ >= frame_period_cycles_) {
            retrace_credit_ -= frame_period_cycles_;
            ++frame_count_;
        }
        retrace_ = retrace_credit_ >= retrace_start_cycles_ &&
                   retrace_credit_ < retrace_start_cycles_ + retrace_window_cycles_;
    }

    // Frame period is in CPU cycles; the Turbo button changes the CPU clock, not the video crystal.
    void set_cpu_hz(double hz) { cpu_hz_ = hz; recompute_timing_(); }

    // Cursor toggles every 8 frames, blinking characters every 16 (IBM VGA Technical Reference).
    uint32_t frame_count() const { return frame_count_; }
    bool cursor_blink_phase_on() const { return (frame_count_ & 8) == 0; }
    bool char_blink_phase_on() const { return (frame_count_ & 16) == 0; }

    // CRTC cursor position and display start address.
    uint16_t cursor_offset() const { return uint16_t((crtc_[0x0E] << 8) | crtc_[0x0F]); }
    uint16_t start_offset() const { return uint16_t((crtc_[0x0C] << 8) | crtc_[0x0D]); }

    // Cursor Start (0Ah: bit 5 disable, bits 0-4 start) and End (0Bh: bits 0-4).
    bool cursor_disabled() const { return (crtc_[0x0A] >> 5) & 1; }
    uint8_t cursor_start_scanline() const { return uint8_t(crtc_[0x0A] & 0x1F); }
    uint8_t cursor_end_scanline() const { return uint8_t(crtc_[0x0B] & 0x1F); }
    // Cursor Skew (CRTC 0Bh bits 5-6): character clocks right of the address.
    int cursor_skew() const { return (crtc_[0x0B] >> 5) & 3; }
    // Underline Location (CRTC 14h bits 0-4).
    int crtc_underline_row() const { return crtc_[0x14] & 0x1F; }

    // Attribute palette register (0-15): a DAC address on VGA, see attr_dac_index().
    uint8_t attr_palette(int index) const { return uint8_t(attr_[index & 0x0F] & 0x3F); }

    // DAC address for a 4-bit pixel: palette bits 0-5, bits 6-7 from Color Select
    // (AR14) bits 2-3, and bits 4-5 from AR14 bits 0-1 when AR10 bit 7 is set (IBM VGA TRM).
    uint8_t attr_dac_index(int pixel) const {
        uint8_t p = attr_palette(pixel);
        uint8_t cs = attr_[0x14];
        if (attr_[0x10] & 0x80) p = uint8_t((p & 0x0F) | ((cs & 0x03) << 4));
        return uint8_t(p | ((cs & 0x0C) << 4));
    }
    // Color Plane Enable (AR12): the bit planes that reach the palette.
    uint8_t attr_plane_enable() const { return uint8_t(attr_[0x12] & 0x0F); }
    // AR10 bit 2: line graphics, bit 3: blink, bit 5: pel panning stops at the split.
    bool attr_line_graphics() const { return (attr_[0x10] >> 2) & 1; }
    bool attr_blink_enabled() const { return (attr_[0x10] >> 3) & 1; }
    bool attr_pan_split_reset() const { return (attr_[0x10] >> 5) & 1; }
    uint8_t attr_pel_pan() const { return uint8_t(attr_[0x13] & 0x0F); }

    // --- VGA DAC (ports 0x3C6-0x3C9) ---
    // Raw 6-bit channel values (0-63).
    void dac_entry(int index, uint8_t &r, uint8_t &g, uint8_t &b) const {
        std::size_t i = std::size_t(index & 0xFF) * 3;
        r = dac_[i + 0]; g = dac_[i + 1]; b = dac_[i + 2];
    }
    // PEL Mask (0x3C6): ANDed with each pixel value before the DAC lookup.
    uint8_t dac_mask() const { return dac_mask_; }

    // AR10 bit 6, 8-bit colour: two dot clocks per pixel in 256-colour modes.
    bool attr_8bit_color() const { return (attr_[0x10] >> 6) & 1; }

    // --- CRTC address-unit selection ---
    // Underline Location (R14) bit 6 = doubleword mode (4 bytes/unit); Mode
    // Control (R17) bit 6 = byte mode (1), clear = word mode (2). Doubleword wins.
    // So Offset 40 is 320 bytes in mode 13h but 80 in mode 10h.
    bool crtc_dword_mode() const { return (crtc_[0x14] >> 6) & 1; }
    bool crtc_byte_mode() const { return (crtc_[0x17] >> 6) & 1; }
    // CRTC Mode Control (17h) bit 0 clear: CGA's two interleaved 8KB banks.
    bool crtc_cga_banks() const { return (crtc_[0x17] & 1) == 0; }
    int crtc_address_unit_bytes() const {
        if (crtc_dword_mode()) return 4;
        return crtc_byte_mode() ? 1 : 2;
    }
    // Bytes between scan lines with address-unit scaling; crtc_scanline_stride() is per-plane.
    int crtc_row_byte_stride() const { return int(crtc_[0x13]) * 2 * crtc_address_unit_bytes(); }
    // Byte offset of the first displayed pixel.
    uint32_t start_byte_offset() const { return uint32_t(start_offset()) * uint32_t(crtc_address_unit_bytes()); }

    // --- VESA BIOS Extensions ---
    // SVGA extension registers on index port 0x1CE / data port 0x1CF, driven by
    // the card's ROM BIOS to implement VBE. The register numbers are the interface
    // of the freely-licensed VGA BIOS this machine substitutes for IBM's
    // (roms/fetch-bios.sh), not any real 1993 SVGA chip. See PC486_REVIEW.md §7.
    static constexpr uint16_t kVbeIndexPort = 0x01CE;
    static constexpr uint16_t kVbeDataPort  = 0x01CF;
    enum VbeReg : uint16_t {
        kVbeRegId = 0x0, kVbeRegXres = 0x1, kVbeRegYres = 0x2, kVbeRegBpp = 0x3,
        kVbeRegEnable = 0x4, kVbeRegBank = 0x5, kVbeRegVirtWidth = 0x6,
        kVbeRegVirtHeight = 0x7, kVbeRegXOffset = 0x8, kVbeRegYOffset = 0x9,
        kVbeRegVideoMemory64K = 0xA, kVbeRegCount = 0xB,
    };
    // Enable-register bits. With GETCAPS set, XRES/YRES/BPP report the card's
    // maxima, which the ROM uses to decide which VESA modes to list.
    static constexpr uint16_t kVbeEnabled    = 0x01;
    static constexpr uint16_t kVbeGetCaps    = 0x02;
    // 32KB bank granularity. The VGABIOS enables it when GETCAPS reports it and
    // then writes bank*2; without it every write lands twice as far (SimCity 2000
    // banded title screen). Bochs vga.cc does the same.
    static constexpr uint16_t kVbeBankGranularity32K = 0x10;
    static constexpr uint16_t kVbeNoClearMem = 0x80;
    // Largest 8bpp frame that fits the 1MB VRAM.
    static constexpr uint16_t kVbeMaxXres = 1024;
    static constexpr uint16_t kVbeMaxYres = 768;
    static constexpr uint16_t kVbeMaxBpp  = 8;
    // Only these IDs are accepted, so a probe sees an unknown revision rejected.
    static constexpr uint16_t kVbeIdLowest  = 0xB0C0;
    static constexpr uint16_t kVbeIdHighest = 0xB0C5;

    uint16_t vbe_reg(int index) const {
        return index >= 0 && index < kVbeRegCount ? vbe_[std::size_t(index)] : uint16_t(0);
    }
    // True when an SVGA linear byte-per-pixel mode is on.
    bool vbe_mode_active() const {
        return (vbe_[kVbeRegEnable] & kVbeEnabled) != 0 && vbe_[kVbeRegBpp] == 8;
    }
    // DISPI on with bpp=4: the planar engine stays in charge (as in Bochs vga.cc)
    // and the Bank register slides the 64KB aperture. Needed for 104h
    // (1024x768x4). See PC486_REVIEW.md §7.5.1.
    bool vbe_planar_banked() const {
        return (vbe_[kVbeRegEnable] & kVbeEnabled) != 0 && vbe_[kVbeRegBpp] == 4;
    }
    // CPU-visible window size at 0xA0000, reported as WinGranularity and WinSize.
    static constexpr uint32_t kVbeBankSize = 65536;
    // Bank register low bits are the bank number; bits 14/15 are the RD/WR window
    // selects the VGABIOS 4F05 path ORs in. Bochs masks the same way.
    static constexpr uint16_t kVbeBankNumberMask = 0x1FF;
    // Live Bank step: 32KB when the Enable bit is on, else 64KB.
    uint32_t vbe_bank_bytes() const {
        return (vbe_[kVbeRegEnable] & kVbeBankGranularity32K) ? 32768u : kVbeBankSize;
    }

    // Graphics Controller Miscellaneous (GR06) bit 0: graphics vs alphanumeric.
    bool graphics_mode_active() const { return (gfx_[6] & 0x01) != 0; }

    // Shift Register field (GR05 bits 5-6): 0 = 16-colour planar, 1 = CGA 4-colour
    // (plane-0 then plane-1 byte as 2-bit pixels), 2 = VGA 256-colour (mode 13h).
    // Value 2 does not exist on 1984 EGA silicon.
    uint8_t gc_shift_register_mode() const { return uint8_t((gfx_[5] >> 5) & 0x03); }

    // Sequencer Chain-4 (SR04 bit 3). With it on, the CPU byte offset is the
    // interleaved vram[] index. Unchained mode 13h (DOOM-style) walks plane_off and
    // plane directly instead. See PC486_REVIEW.md.
    bool chain4_enabled() const { return seq_chain4(); }
    // Clocking Mode (SR01) bit 0: 8-dot character clock, clear = 9.
    bool seq_8dot_chars() const { return sequencer_[1] & 1; }
    // Character Map Select (SR03): map A in bits 5,3,2, map B in bits 4,1,0.
    int seq_char_map_a() const { return ((sequencer_[3] >> 3) & 4) | ((sequencer_[3] >> 2) & 3); }
    int seq_char_map_b() const { return ((sequencer_[3] >> 2) & 4) | (sequencer_[3] & 3); }

    // CRTC registers that set graphics resolution: Horizontal Display End (01h)
    // and Vertical Display End (12h plus R07 bit 1). Only one overflow bit is
    // used, as on 1984 EGA. R07 bit 6, VGA's bit 9, is left out because folding
    // it in made the range jump 512 lines (Prince of Persia black canvas).
    uint16_t crtc_horizontal_display_end() const { return crtc_[0x01]; }
    uint16_t crtc_vertical_display_end() const {
        return uint16_t(crtc_[0x12] | ((crtc_[0x07] >> 1 & 1) << 8));
    }
    // Maximum Scan Line (CRTC R09 bits 0-4): scan lines per character row minus 1.
    uint8_t crtc_max_scan_line() const { return uint8_t(crtc_[0x09] & 0x1F); }

    // Scan Doubling (CRTC R09 bit 7): VGA draws each scanline twice, so a 200-line
    // mode fills a ~400-line raster. Prince of Persia's EGA mode leaves Vertical
    // Display End at 399 with this set. See RenderEgaNative16Screen().
    bool crtc_scan_doubling() const { return (crtc_[0x09] >> 7) & 1; }

    // Line Compare (CRTC 18h, bit 8 in R07 bit 4, bit 9 in R09 bit 6): split-screen restart line.
    int crtc_line_compare() const {
        return int(crtc_[0x18]) | ((crtc_[0x07] >> 4 & 1) << 8) | ((crtc_[0x09] >> 6 & 1) << 9);
    }
    // Preset Row Scan (CRTC 08h): bits 0-4 smooth vertical scroll, bits 5-6 byte pan.
    int crtc_preset_row_scan() const { return crtc_[0x08] & 0x1F; }
    int crtc_byte_pan() const { return (crtc_[0x08] >> 5) & 3; }

    // Offset Register (CRTC R13): per-scanline stride in words per plane,
    // independent of Horizontal Display End. Panning and sub-window blits rely on it.
    uint8_t crtc_offset() const { return crtc_[0x13]; }
    // Per-plane byte stride between scanlines (crtc_offset() * 2).
    int crtc_scanline_stride() const { return int(crtc_[0x13]) * 2; }

    // 1MB VRAM, interleaved as above. Legacy addressing reaches the first 256KB;
    // the rest is reached through the Bank register (vbe_linear_offset,
    // vbe_planar_banked).
    std::array<uint8_t, 1024 * 1024> vram{};
    uint32_t mapping_epoch_ = 0;
    uint32_t mapping_sig_ = 0xFFFFFFFFu;

private:
    // Decodes a CPU address in 0xA0000-0xBFFFF against GR06 bits 2-3 (128K@A0000,
    // 64K@A0000, 32K@B0000, 32K@B8000) to a window-local offset, or kOutOfWindow.
    static constexpr uint32_t kOutOfWindow = 0xFFFFFFFF;
    uint32_t window_offset(uint32_t addr) const;
    // SVGA mode: 0xA0000 is a 64KB window onto a linear frame, positioned by the
    // Bank register. Returns the VRAM offset or kOutOfWindow.
    uint32_t vbe_linear_offset(uint32_t addr) const;
    // 4bpp DISPI mode: the Bank register slides plane_off (Bochs ext_offset).
    uint32_t vbe_planar_plane_off(uint32_t plane_off) const;

    // Decode straight from the raw register arrays, the single source of truth.
    uint8_t seq_map_mask() const { return uint8_t(sequencer_[2] & 0x0F); }
    bool seq_odd_even_disabled() const { return (sequencer_[4] >> 2) & 1; }
    // Chain 4 (SR04 bit 3) overrides odd/even chaining.
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

    // Rederives the cached retrace timing. Called from the register writes that
    // can change it, reset() and set_cpu_hz(), never from tick().
    void recompute_timing_();
    bool display_disabled_() const;

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

    // DAC colour RAM, 256 x {R,G,B}, 6 bits each. Read and write sides keep
    // independent index and R->G->B sub-counters.
    std::array<uint8_t, 256 * 3> dac_{};
    uint8_t dac_write_index_ = 0;
    uint8_t dac_read_index_ = 0;
    uint8_t dac_write_sub_ = 0;
    uint8_t dac_read_sub_ = 0;
    // DAC State (0x3C7 read): 3 = last index write was the read register, 0 = write register.
    uint8_t dac_state_ = 0;
    uint8_t dac_mask_ = 0xFF;

    std::array<uint16_t, kVbeRegCount> vbe_{};
    uint16_t vbe_index_ = 0;
    uint16_t vbe_read_(int index) const;
    void vbe_write_(int index, uint16_t v);

    // Read latch: loaded with all 4 planes on every read; mutable because that is
    // a side effect of reading.
    mutable std::array<uint8_t, 4> latch_{};

    bool retrace_ = false;
    uint64_t prev_cycles_ = 0;
    double retrace_credit_ = 0.0;
    uint32_t frame_count_ = 0;

    // --- Retrace timing cache ---
    double cpu_hz_ = 66e6;                // this machine's CPU clock; see set_cpu_hz()
    double frame_period_cycles_ = 0.0;    // cached CPU cycles per vertical frame
    double retrace_start_cycles_ = 0.0;   // cached cycles from frame start to retrace onset
    double retrace_window_cycles_ = 0.0;  // cached cycles the retrace bit stays asserted
    double scanline_cycles_ = 1.0;        // cached CPU cycles per scanline
    double h_display_cycles_ = 0.0;       // cached cycles into a scanline where display enable drops
    double v_display_cycles_ = 0.0;       // cached cycles into a frame where display enable drops
};

}  // namespace pc486

#endif  // PC486_EGA_H
