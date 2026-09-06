// Native diagnostic tool: boots the real BIOS (+ optional vgabios, floppy,
// hard disk) against this machine's chipset for a fixed cycle budget, then
// renders the current EGA text-mode screen to an actual BMP image -- not
// an ASCII reconstruction. Analogous in spirit to bios_host.cpp (a
// headless way to observe real firmware/OS progress), but for *looking at
// the screen* rather than reading POST codes off port 0x80.
//
// The render uses only real, already-emulated hardware state, read
// straight out of Ega -- no separate font or palette data of its own:
//   - Glyph bitmaps come from VRAM plane 2 (the character generator RAM),
//     at the standard EGA/VGA convention of 32 bytes reserved per
//     character (offset char*32 + scanline) -- exactly where the real
//     vgabios's own mode-set writes the font, so whatever font vgabios
//     actually loaded is what gets rendered.
//   - Colors come from Ega::attr_palette(), the live Attribute Controller
//     palette registers, decoded via the genuine EGA 6-bit color format
//     (bit0=B, bit1=G, bit2=R, bit3=secondary-B, bit4=secondary-G,
//     bit5=secondary-R; each channel = primary*0xAA + secondary*0x55) --
//     the real DAC-independent 64-color EGA scheme, not a hardcoded
//     16-color table, so a program that reprograms the palette renders
//     correctly too.
//
// Text mode only (80x25, 8x14 cells -> 640x350), matching the scope of
// this machine's current front end (Phase 5 finished the real memory
// engine; a graphics-mode renderer is Phase 7's job, alongside the actual
// <canvas> front end this tool is a stand-in for during development).
// See IBM_PCAT_REVIEW.md §12.
//
// Usage: render_screen <bios> <vgabios> <out.bmp> [max_cycles] [hdd-image] [floppy-image]

#include "machine.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using ibmpcat::Machine;

namespace {

std::vector<uint8_t> ReadFile(const char *path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Real EGA palette-register decode -- see the file header.
void DecodeEgaColor(uint8_t v, uint8_t &r, uint8_t &g, uint8_t &b) {
    auto chan = [](bool lo, bool hi) -> uint8_t { return uint8_t(lo * 0xAA + hi * 0x55); };
    b = chan(v & 0x01, v & 0x08);
    g = chan(v & 0x02, v & 0x10);
    r = chan(v & 0x04, v & 0x20);
}

// Hand-written 24-bit uncompressed BMP -- a real, self-contained image
// format needing no external library or dependency.
void WriteBmp(const char *path, int w, int h, const std::vector<uint8_t> &rgb) {
    int row_bytes = w * 3;
    int pad = (4 - (row_bytes % 4)) % 4;
    uint32_t data_size = uint32_t((row_bytes + pad) * h);
    uint32_t file_size = 54 + data_size;
    std::ofstream f(path, std::ios::binary);
    auto w32 = [&](uint32_t v) { f.put(char(v)); f.put(char(v >> 8)); f.put(char(v >> 16)); f.put(char(v >> 24)); };
    auto w16 = [&](uint16_t v) { f.put(char(v)); f.put(char(v >> 8)); };
    f.put('B'); f.put('M'); w32(file_size); w32(0); w32(54);
    w32(40); w32(uint32_t(w)); w32(uint32_t(h)); w16(1); w16(24); w32(0);
    w32(data_size); w32(2835); w32(2835); w32(0); w32(0);
    std::vector<uint8_t> padbuf(pad, 0);
    for (int y = h - 1; y >= 0; --y) {  // BMP rows are bottom-up
        for (int x = 0; x < w; ++x) {
            int i = (y * w + x) * 3;
            f.put(char(rgb[i + 2])); f.put(char(rgb[i + 1])); f.put(char(rgb[i + 0]));  // BGR
        }
        f.write(reinterpret_cast<const char *>(padbuf.data()), pad);
    }
}

void RenderTextScreen(Machine &m, const char *out_path) {
    // 80x25 chars, 8x14 cells -- standard EGA 80x25 text-mode glyph size.
    const int cw = 8, ch_h = 14, cols = 80, rows = 25;
    const int W = cw * cols, H = ch_h * rows;
    std::vector<uint8_t> rgb(size_t(W) * H * 3, 0);

    auto text_at = [&](int row, int col, uint8_t &ch, uint8_t &attr) {
        uint32_t plane_off = (uint32_t(m.chipset.ega.start_offset()) + uint32_t(row * 80 + col)) & 0xFFFF;
        ch = m.chipset.ega.vram[(plane_off << 2) + 0];
        attr = m.chipset.ega.vram[(plane_off << 2) + 1];
    };

    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
            uint8_t ch, attr;
            text_at(row, col, ch, attr);
            uint8_t fg_idx = attr & 0x0F, bg_idx = uint8_t((attr >> 4) & 0x07);  // bit7 = blink, ignored
            uint8_t fr, fg, fb, br, bg, bb;
            DecodeEgaColor(m.chipset.ega.attr_palette(fg_idx), fr, fg, fb);
            DecodeEgaColor(m.chipset.ega.attr_palette(bg_idx), br, bg, bb);
            for (int r = 0; r < ch_h; ++r) {
                uint32_t glyph_plane_off = uint32_t(ch) * 32 + uint32_t(r);
                uint8_t bits = m.chipset.ega.vram[(glyph_plane_off << 2) + 2];  // plane 2: char generator
                for (int c = 0; c < cw; ++c) {
                    bool set = (bits & (0x80 >> c)) != 0;
                    int px = col * cw + c, py = row * ch_h + r;
                    int i = (py * W + px) * 3;
                    rgb[i + 0] = set ? fr : br;
                    rgb[i + 1] = set ? fg : bg;
                    rgb[i + 2] = set ? fb : bb;
                }
            }
        }
    }
    WriteBmp(out_path, W, H, rgb);
    std::fprintf(stderr, "wrote %s (%dx%d)\n", out_path, W, H);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 4) {
        std::fprintf(stderr,
            "usage: %s <bios> <vgabios> <out.bmp> [max_cycles] [hdd-image] [floppy-image]\n", argv[0]);
        return 2;
    }
    auto bios = ReadFile(argv[1]);
    auto vga = ReadFile(argv[2]);
    const char *out_path = argv[3];
    uint64_t max_cycles = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 400'000'000ull;
    if (bios.empty()) { std::fprintf(stderr, "cannot open/empty bios: %s\n", argv[1]); return 2; }
    std::fprintf(stderr, "bios=%zu vga=%zu\n", bios.size(), vga.size());

    Machine m;
    m.reset();
    m.chipset.load_rom(0x100000 - bios.size(), bios.data(), bios.size());
    if (!vga.empty()) m.chipset.load_rom(0xC0000, vga.data(), vga.size());
    if (argc > 5) {
        auto hdd = ReadFile(argv[5]);
        std::fprintf(stderr, "mounted %s: %zu bytes on HDD\n", argv[5], hdd.size());
        m.chipset.hdd.mount(0, hdd.data(), hdd.size());
    }
    if (argc > 6) {
        auto fd = ReadFile(argv[6]);
        std::fprintf(stderr, "mounted %s: %zu bytes on Drive A:\n", argv[6], fd.size());
        m.chipset.fdc.mount(0, fd.data(), fd.size());
    }

    constexpr uint64_t kChunk = 200000;
    for (uint64_t done = 0; done < max_cycles; done += kChunk) m.run_cycles(kChunk);
    std::fprintf(stderr, "ran %llu cycles\n", (unsigned long long)max_cycles);

    RenderTextScreen(m, out_path);
    return 0;
}
