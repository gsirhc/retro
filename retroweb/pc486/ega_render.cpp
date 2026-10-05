#include "ega_render.h"

namespace pc486 {

namespace {

// Real VGA DAC decode: 6 significant bits per channel (0-63) driving a
// full-scale analog ramp, so 63 is maximum brightness. (v<<2)|(v>>4) maps
// that range onto 0-255 exactly at both ends. The PEL
// Mask (0x3C6) is applied to the pixel value first, exactly where real
// hardware applies it: between the shift registers and the DAC's address
// lines, not to the stored colors.
void DecodeDacColor(const Ega &ega, uint8_t pixel, uint8_t &r, uint8_t &g, uint8_t &b) {
    uint8_t six_r, six_g, six_b;
    ega.dac_entry(pixel & ega.dac_mask(), six_r, six_g, six_b);
    auto full_scale = [](uint8_t v) { return uint8_t((v << 2) | (v >> 4)); };
    r = full_scale(six_r); g = full_scale(six_g); b = full_scale(six_b);
}

// A text or 16-colour pixel through the VGA's real path: attribute palette,
// then the DAC.
void DecodeAttrColor(const Ega &ega, uint8_t pixel, uint8_t &r, uint8_t &g, uint8_t &b) {
    DecodeDacColor(ega, ega.attr_dac_index(pixel), r, g, b);
}

// Where the CRTC's address counter is on one raster line: the row start
// address it reloaded from, the character row since then and the scan line
// within that row. Past Line Compare the counter restarts at 0 and the row
// scan at 0, which is the split screen; above it the start address and
// Preset Row Scan apply (IBM VGA Technical Reference, CRT Controller).
struct ScanPos {
    uint32_t base;
    int row;
    int row_scan;
    bool lower;
};

ScanPos LocateRaster(const Ega &ega, int raster, int lines_per_row, uint32_t top_base) {
    int dbl = ega.crtc_scan_doubling() ? 2 : 1;
    int lc = ega.crtc_line_compare();
    ScanPos p;
    int line;
    if (raster > lc) {
        line = (raster - lc - 1) / dbl;
        p.base = 0;
        p.lower = true;
    } else {
        line = raster / dbl + ega.crtc_preset_row_scan();
        p.base = top_base;
        p.lower = false;
    }
    p.row = line / lines_per_row;
    p.row_scan = line % lines_per_row;
    return p;
}

// AR13 in 8-dot text and graphics shifts 0-7 pixels; in 9-dot text 8 means
// 0 and 0-7 mean 1-8; in 256-colour mode it counts half pixels.
int PelPan(const Ega &ega, int dots_per_char, bool vga256) {
    int v = ega.attr_pel_pan();
    if (vga256) return (v >> 1) & 3;
    if (dots_per_char == 9) return v < 8 ? v + 1 : 0;
    return v < 8 ? v : 0;
}

// A graphics pixel through Color Plane Enable and, with AR10 bit 3 set,
// graphics blink: bit 3 reads 1, except that a pixel with bit 3 set (or
// any pixel with plane 3 disabled) drops it in the off phase. IBM's
// manual leaves this undefined; this is 86Box's rule (vid_svga_render.c),
// checked there against Lotus 1-2-3 WYSIWYG and QBASIC SCREEN 10.
uint8_t GraphicsAttr(const Ega &ega, uint8_t pixel) {
    const uint8_t pm = ega.attr_plane_enable();
    if (!ega.attr_blink_enabled()) return uint8_t(pixel & pm);
    const bool drops = !ega.char_blink_phase_on() && ((pixel & 8) || !(pm & 8));
    return uint8_t((pixel & pm & 7) | (drops ? 0 : 8));
}

void PutPixel(std::vector<uint8_t> &rgba, int width, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    std::size_t i = (std::size_t(y) * std::size_t(width) + std::size_t(x)) * 4;
    rgba[i + 0] = r;
    rgba[i + 1] = g;
    rgba[i + 2] = b;
    rgba[i + 3] = 255;
}

}  // namespace

void RenderTextScreen(const Ega &ega, std::vector<uint8_t> &rgba, int &width, int &height) {
    const int cw = ega.seq_8dot_chars() ? 8 : 9;
    // Max Scan Line 0 is no real text mode and is what a never-programmed
    // card reads, so it falls back to 14 lines; the same goes for an
    // unprogrammed Horizontal Display End (80 columns) and a Vertical
    // Display End shorter than one row (25 rows).
    int scan_lines = int(ega.crtc_max_scan_line()) + 1;
    const int ch_h = scan_lines <= 1 ? 14 : scan_lines;
    int cols_reg = int(ega.crtc_horizontal_display_end()) + 1;
    const int cols = cols_reg <= 1 ? 80 : cols_reg;
    const int dbl = ega.crtc_scan_doubling() ? 2 : 1;
    int H = (int(ega.crtc_vertical_display_end()) + 1) / dbl;
    if (H < ch_h) H = 25 * ch_h;
    const int W = cw * cols;
    width = W; height = H;
    rgba.assign(std::size_t(W) * std::size_t(H) * 4, 0);

    const int stride = ega.crtc_scanline_stride() > 0 ? ega.crtc_scanline_stride() : cols;
    const uint32_t top_base = uint32_t(ega.start_offset()) + uint32_t(ega.crtc_byte_pan());
    const int pan = PelPan(ega, cw, false);
    const bool pan_split_reset = ega.attr_pan_split_reset();
    const bool line_graphics = ega.attr_line_graphics();
    const bool blink_enabled = ega.attr_blink_enabled();
    const bool chars_on = ega.char_blink_phase_on();
    const uint8_t plane_enable = ega.attr_plane_enable();
    // Font map n sits at these plane-2 offsets (IBM VGA Technical Reference,
    // Character Map Select).
    static constexpr uint32_t kMapOffset[8] = {0x0000, 0x4000, 0x8000, 0xC000, 0x2000, 0x6000, 0xA000, 0xE000};
    const uint32_t map_a = kMapOffset[ega.seq_char_map_a()];
    const uint32_t map_b = kMapOffset[ega.seq_char_map_b()];

    const uint16_t cursor_off = uint16_t(ega.cursor_offset() + ega.cursor_skew());
    const int underline_row = ega.crtc_underline_row();
    const bool cursor_visible = ega.cursor_blink_phase_on() && !ega.cursor_disabled();

    for (int y = 0; y < H; ++y) {
        ScanPos pos = LocateRaster(ega, y * dbl, ch_h, top_base);
        const int line_pan = pos.lower && pan_split_reset ? 0 : pan;
        const uint32_t row_base = pos.base + uint32_t(pos.row * stride);
        const bool cursor_row = cursor_visible &&
            pos.row_scan >= ega.cursor_start_scanline() && pos.row_scan <= ega.cursor_end_scanline();
        int cached_col = -1;
        uint8_t ch = 0, bits = 0;
        bool is_cursor = false, hidden = false;
        uint8_t fr = 0, fg = 0, fb = 0, br = 0, bg = 0, bb = 0;
        for (int x = 0; x < W; ++x) {
            const int sx = x + line_pan;
            const int col = sx / cw, c = sx % cw;
            if (col != cached_col) {
                cached_col = col;
                const uint32_t cell = (row_base + uint32_t(col)) & 0xFFFF;
                ch = ega.vram[(cell << 2) + 0];
                const uint8_t attr = ega.vram[(cell << 2) + 1];
                const uint32_t map = (attr & 0x08) ? map_a : map_b;
                bits = ega.vram[((map + uint32_t(ch) * 32 + uint32_t(pos.row_scan)) << 2) + 2];
                // Underline is attribute x0x1 (foreground 1, background 0) on
                // the Underline Location row, as DOSBox decodes it. It fills
                // dots 1-8, so column 9 keeps its own rule: dashed across 9-dot
                // cells, solid for line-graphics characters (IBM VGA Technical
                // Reference, "Programming Considerations").
                if ((attr & 0x77) == 0x01 && pos.row_scan == underline_row) bits = 0xFF;
                uint8_t bg_idx = blink_enabled ? uint8_t((attr >> 4) & 0x07) : uint8_t(attr >> 4);
                hidden = blink_enabled && (attr & 0x80) && !chars_on;
                is_cursor = cursor_row && cell == cursor_off;
                DecodeAttrColor(ega, uint8_t(attr & 0x0F & plane_enable), fr, fg, fb);
                DecodeAttrColor(ega, uint8_t(bg_idx & plane_enable), br, bg, bb);
            }
            bool set;
            if (c < 8) set = (bits & (0x80 >> c)) != 0;
            else set = line_graphics && ch >= 0xC0 && ch <= 0xDF && (bits & 0x01);
            if (hidden) set = false;
            if (is_cursor) set = true;
            if (set) PutPixel(rgba, W, x, y, fr, fg, fb);
            else PutPixel(rgba, W, x, y, br, bg, bb);
        }
    }
}

void RenderCgaGraphics4Screen(const Ega &ega, std::vector<uint8_t> &rgba) {
    constexpr int W = 320, H = 200;
    rgba.assign(std::size_t(W) * std::size_t(H) * 4, 0);

    // A CRTC nobody has programmed (Vertical Display End 0) gets mode 4's
    // own layout: two scan lines per row, 80 bytes per row.
    const bool unprogrammed = ega.crtc_vertical_display_end() == 0;
    const int lines_per_row = unprogrammed ? 2 : int(ega.crtc_max_scan_line()) + 1;
    const int dbl = ega.crtc_scan_doubling() ? 2 : 1;
    const uint32_t stride = ega.crtc_scanline_stride() > 0 ? uint32_t(ega.crtc_scanline_stride()) : 80u;
    // Word mode: the start address and byte panning count 2-byte units of
    // the flat CGA-style offset.
    const uint32_t top_base = (uint32_t(ega.start_offset()) + uint32_t(ega.crtc_byte_pan())) * 2u;
    const int pan = PelPan(ega, 8, false);
    const bool cga_banks = ega.crtc_cga_banks();

    for (int y = 0; y < H; ++y) {
        ScanPos pos = LocateRaster(ega, y * dbl, lines_per_row, top_base);
        const int line_pan = pos.lower && ega.attr_pan_split_reset() ? 0 : pan;
        const uint32_t row_base = pos.base + uint32_t(pos.row) * stride;
        for (int x = 0; x < W; ++x) {
            const int sx = x + line_pan;
            // The flat CGA-style byte offset a CGA-unaware program would
            // have written to; odd/even chaining splits it across planes 0/1.
            uint32_t linear_offset = row_base + uint32_t(sx >> 2);
            if (cga_banks) linear_offset = (linear_offset & ~0x2000u) | (uint32_t(pos.row_scan & 1) << 13);
            const uint32_t plane = linear_offset & 1;
            const uint32_t plane_offset = (linear_offset >> 1) & 0xFFFF;
            const uint8_t byte = ega.vram[(plane_offset << 2) + plane];
            const uint8_t pixel2 = uint8_t((byte >> (6 - 2 * (sx & 3))) & 0x3);
            uint8_t r, g, b;
            DecodeAttrColor(ega, GraphicsAttr(ega, pixel2), r, g, b);
            PutPixel(rgba, W, x, y, r, g, b);
        }
    }
}

void RenderEgaNative16Screen(const Ega &ega, std::vector<uint8_t> &rgba, int &width, int &height) {
    // A 4bpp DISPI mode still paints through the planar engine, but its
    // geometry lives in the extension registers -- the ROM's
    // vga_compat_setup does reprogram the CRTC, yet this card's VDE
    // accessor deliberately reads only EGA's 9-bit overflow (see ega.h),
    // which cannot express 600 or 768 lines. Trust the DISPI registers the
    // same way RenderVga256Screen does for 8bpp, matching Bochs's bpp=4
    // path taking vbe.xres/yres/line_offset for the tall modes.
    const bool dispi = ega.vbe_planar_banked();
    int row_stride;
    int dbl = 1;
    if (dispi) {
        width = ega.vbe_reg(Ega::kVbeRegXres);
        height = ega.vbe_reg(Ega::kVbeRegYres);
        int virt = ega.vbe_reg(Ega::kVbeRegVirtWidth);
        if (virt < width) virt = width;
        row_stride = virt / 8;  // 1 bit/pixel/plane; Bochs line_offset = xres>>3
    } else {
        width = (ega.crtc_horizontal_display_end() + 1) * 8;
        height = ega.crtc_vertical_display_end() + 1;
        // Scan Doubling (see crtc_scan_doubling() in ega.h): the CRTC's own
        // vertical counters describe the full doubled raster (e.g. 400 lines for
        // a 200-line picture), but VRAM only ever holds one copy of each row --
        // the second physical scanline of every pair is a hardware-side repeat,
        // not distinct data. Render at the logical (halved) height directly.
        if (ega.crtc_scan_doubling()) { height /= 2; dbl = 2; }
        // The real per-scanline VRAM stride comes from the CRTC's own Offset
        // Register, NOT from the displayed width -- see crtc_scanline_stride()
        // in ega.h. A freshly-reset/never-programmed Offset register reads 0
        // -- fall back to the displayed width in that case.
        int real_stride = ega.crtc_scanline_stride();
        row_stride = real_stride > 0 ? real_stride : width / 8;
    }
    if (width <= 0 || height <= 0) { width = height = 0; rgba.clear(); return; }
    rgba.assign(std::size_t(width) * std::size_t(height) * 4, 0);

    const uint32_t top_base = dispi ? 0 : uint32_t(ega.start_offset()) + uint32_t(ega.crtc_byte_pan());
    const int lines_per_row = int(ega.crtc_max_scan_line()) + 1;
    const int pan = dispi ? 0 : PelPan(ega, 8, false);
    for (int y = 0; y < height; ++y) {
        uint32_t row_base;
        int line_pan = pan;
        if (dispi) {
            row_base = uint32_t(y) * uint32_t(row_stride);
        } else {
            ScanPos pos = LocateRaster(ega, y * dbl, lines_per_row, top_base);
            row_base = pos.base + uint32_t(pos.row * row_stride);
            if (pos.lower && ega.attr_pan_split_reset()) line_pan = 0;
        }
        for (int x = 0; x < width; ++x) {
            const int sx = x + line_pan;
            uint32_t plane_offset = row_base + uint32_t(sx >> 3);
            if (!dispi) plane_offset &= 0xFFFF;
            // Past the card's interleaved VRAM there is nothing to show --
            // a banked 4bpp frame can ask for plane_off past 256KB of
            // groups when VirtWidth is oversized; leave those pixels black.
            if ((plane_offset << 2) + 3 >= ega.vram.size()) continue;
            const int shift = 7 - (sx & 7);
            const uint8_t* p = &ega.vram[plane_offset << 2];
            uint8_t nibble = uint8_t(((p[0] >> shift) & 1) | (((p[1] >> shift) & 1) << 1) |
                                      (((p[2] >> shift) & 1) << 2) | (((p[3] >> shift) & 1) << 3));
            uint8_t r, g, b;
            DecodeAttrColor(ega, GraphicsAttr(ega, nibble), r, g, b);
            PutPixel(rgba, width, x, y, r, g, b);
        }
    }
}

void RenderVga256Screen(const Ega &ega, std::vector<uint8_t> &rgba, int &width, int &height) {
    // See this function's header comment in ega_render.h for why each of
    // these comes from the register it comes from.
    int stride;
    uint32_t base;
    // Chain-4 makes the flat CPU byte address and the interleaved vram[]
    // index the same number (mem_read()/mem_write()'s file-header comment),
    // which is what lets this function walk vram[] with a plain byte
    // stride below. "Unchained mode 13h" -- chain-4 off, 256-color shift-out
    // still selected -- breaks that equivalence: real DOS software (id's
    // DOOM engine among it) uses this to write one plane at a time via Map
    // Mask for a faster column blit and for page-flipping. Once chain-4 is
    // off, the CRTC's own Start Address / Offset registers still count in
    // the same per-plane-group units mem_write()'s plane_off does --
    // unaffected by the CRTC's byte/word/dword bits, which only ever
    // scaled the *flat* address chain-4 exposes to the CPU -- so the fix
    // is to walk plane_off/plane directly instead of a flat offset. See
    // PC486_REVIEW.md.
    bool chain4 = ega.chain4_enabled();
    if (ega.vbe_mode_active()) {
        // An SVGA mode is described by the card's own extension registers,
        // not by the legacy CRTC -- the ROM programs geometry there and
        // leaves the CRTC to whatever the previous mode left behind, so
        // reading the CRTC here would be reading stale values. The logical
        // line can be wider than the displayed one (VirtWidth), and the
        // X/Y offset registers pan the visible window around inside it --
        // the SVGA equivalent of the CRTC Start Address, and the same
        // mechanism period software double-buffers with.
        width = ega.vbe_reg(Ega::kVbeRegXres);
        height = ega.vbe_reg(Ega::kVbeRegYres);
        stride = ega.vbe_reg(Ega::kVbeRegVirtWidth);
        if (stride < width) stride = width;
        base = uint32_t(ega.vbe_reg(Ega::kVbeRegYOffset)) * uint32_t(stride) +
               uint32_t(ega.vbe_reg(Ega::kVbeRegXOffset));
        if (width <= 0 || height <= 0) { width = height = 0; rgba.clear(); return; }
        rgba.assign(std::size_t(width) * std::size_t(height) * 4, 0);
    } else {
        width = (ega.crtc_horizontal_display_end() + 1) * 8;
        if (ega.attr_8bit_color()) width /= 2;  // two dot clocks per 256-color pixel

        height = ega.crtc_vertical_display_end() + 1;
        int scan_lines_per_row = int(ega.crtc_max_scan_line()) + 1;
        if (scan_lines_per_row > 1) height /= scan_lines_per_row;
        if (ega.crtc_scan_doubling()) height /= 2;
        if (width <= 0 || height <= 0) { width = height = 0; rgba.clear(); return; }
        rgba.assign(std::size_t(width) * std::size_t(height) * 4, 0);

        if (chain4) {
            stride = ega.crtc_row_byte_stride();
            if (stride <= 0) stride = width;  // never programmed yet -- see RenderEgaNative16Screen
            base = ega.start_byte_offset() + uint32_t(ega.crtc_byte_pan() * ega.crtc_address_unit_bytes());
        } else {
            // Unchained: the CRTC's byte/word/dword bits no longer scale to
            // a valid flat vram[] stride (they only ever scaled the chain-4
            // flat address), so use the raw per-plane-group units directly
            // -- exactly what mem_write()'s plane_off math consumes.
            stride = ega.crtc_scanline_stride();
            if (stride <= 0) stride = (width + 3) / 4;
            base = uint32_t(ega.start_offset()) + uint32_t(ega.crtc_byte_pan());
        }
    }
    const bool vbe = ega.vbe_mode_active();
    const bool flat_addressing = vbe || chain4;
    const int lines_per_row = int(ega.crtc_max_scan_line()) + 1;
    const int raster_per_row = lines_per_row * (ega.crtc_scan_doubling() ? 2 : 1);
    const int pan = vbe ? 0 : PelPan(ega, 8, true);

    for (int y = 0; y < height; ++y) {
        uint32_t row_base;
        int line_pan = pan;
        if (vbe) {
            row_base = base + uint32_t(y) * uint32_t(stride);
        } else {
            ScanPos pos = LocateRaster(ega, y * raster_per_row, lines_per_row, base);
            row_base = pos.base + uint32_t(pos.row) * uint32_t(stride);
            if (pos.lower && ega.attr_pan_split_reset()) line_pan = 0;
        }
        for (int x = 0; x < width; ++x) {
            const uint32_t sx = uint32_t(x + line_pan);
            uint8_t pixel;
            if (flat_addressing) {
                // Chain-4 (or an SVGA linear mode) makes the flat frame-
                // buffer offset and the planar VRAM index the same number --
                // see ega.h's file header. The wrap is the card's own
                // 256KB, the same way a real VGA's address counter wraps
                // rather than reading someone else's RAM.
                pixel = ega.vram[(row_base + sx) % uint32_t(ega.vram.size())];
            } else {
                // Unchained: walk plane_off/plane exactly like mem_write()'s
                // (plane_off << 2) + plane addressing -- one plane_off group
                // covers 4 consecutive displayed pixels, one byte per plane.
                uint32_t plane_off = (row_base + sx / 4) % (1u << 16);
                pixel = ega.vram[(plane_off << 2) + (sx & 3u)];
            }
            uint8_t r, g, b;
            DecodeDacColor(ega, pixel, r, g, b);
            PutPixel(rgba, width, x, y, r, g, b);
        }
    }
}

ScreenMode DetectScreenMode(const Ega &ega) {
    // An SVGA mode is switched on at the card's extension registers and
    // takes over the display outright, whatever the legacy Graphics
    // Controller registers still say -- so it is checked first.
    if (ega.vbe_mode_active()) return ScreenMode::kVga256;
    if (!ega.graphics_mode_active()) return ScreenMode::kText;
    // The Graphics Controller Mode register's Shift Register field is the
    // real CRT controller's own 256-color selector (value 2), so it decides
    // here too -- not a BIOS mode number, and not the Sequencer's Chain-4
    // bit, which is an addressing choice software can make independently.
    if (ega.gc_shift_register_mode() == 2) return ScreenMode::kVga256;
    if (ega.gc_shift_register_mode() == 1) return ScreenMode::kCgaGraphics4;
    if (ega.gc_shift_register_mode() == 0) return ScreenMode::kEgaGraphics16;
    return ScreenMode::kUnsupportedGraphics;
}

void RenderScreen(const Ega &ega, RenderedFrame &out) {
    switch (DetectScreenMode(ega)) {
        case ScreenMode::kCgaGraphics4:
            out.width = 320;
            out.height = 200;
            RenderCgaGraphics4Screen(ega, out.rgba);
            return;
        case ScreenMode::kVga256:
            RenderVga256Screen(ega, out.rgba, out.width, out.height);
            if (out.width > 0 && out.height > 0) return;
            // CRTC not programmed to a sane resolution yet (mid mode-set):
            // fall back to the same honest black placeholder the other
            // graphics paths use, NOT to a different mode's decode.
            break;
        case ScreenMode::kEgaGraphics16:
            RenderEgaNative16Screen(ega, out.rgba, out.width, out.height);
            if (out.width > 0 && out.height > 0) return;
            // CRTC not programmed to a sane resolution yet (mid mode-set)
            // -- fall through to the same honest black placeholder below
            // rather than a zero-size frame.
            break;
        case ScreenMode::kUnsupportedGraphics:
            break;
        case ScreenMode::kText:
        default:
            RenderTextScreen(ega, out.rgba, out.width, out.height);
            return;
    }
    // Honest placeholder -- a plain black frame, not a garbled
    // misinterpretation of graphics VRAM as text glyphs (see the file
    // header). Same footprint as text mode so a caller's canvas/window
    // doesn't need special-casing for "nothing to show yet".
    out.width = kTextRenderWidth;
    out.height = kTextRenderHeight;
    out.rgba.assign(std::size_t(out.width) * std::size_t(out.height) * 4, 0);
    for (std::size_t i = 3; i < out.rgba.size(); i += 4) out.rgba[i] = 255;  // opaque
}

}  // namespace pc486
