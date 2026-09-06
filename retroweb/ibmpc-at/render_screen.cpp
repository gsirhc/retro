// Native diagnostic tool: boots the real BIOS (+ optional vgabios, floppy,
// hard disk) against this machine's chipset for a fixed cycle budget, then
// renders the current EGA text-mode screen to an actual BMP image -- not
// an ASCII reconstruction. Analogous in spirit to bios_host.cpp (a
// headless way to observe real firmware/OS progress), but for *looking at
// the screen* rather than reading POST codes off port 0x80.
//
// The actual pixel decode lives in ega_render.h/.cpp, shared with the WASM
// front end's canvas renderer -- see that file's header for what it does
// and why (real character-generator RAM, real palette registers, no
// hardcoded font/color table) and its scope (text mode only; see
// IBM_PCAT_REVIEW.md §14). This file is just the native harness: boot,
// call the shared renderer, write a BMP.
//
// Usage: render_screen <bios> <vgabios> <out.bmp> [max_cycles] [hdd-image] [floppy-image]

#include "ega_render.h"
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

// Hand-written 24-bit uncompressed BMP -- a real, self-contained image
// format needing no external library or dependency. Takes RGBA input (the
// shared renderer's native format) and drops the alpha byte, since BMP has
// no alpha channel to put it in.
void WriteBmp(const char *path, int w, int h, const std::vector<uint8_t> &rgba) {
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
            std::size_t i = (std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4;
            f.put(char(rgba[i + 2])); f.put(char(rgba[i + 1])); f.put(char(rgba[i + 0]));  // BGR
        }
        f.write(reinterpret_cast<const char *>(padbuf.data()), pad);
    }
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

    std::vector<uint8_t> rgba;
    ibmpcat::RenderTextScreen(m.chipset.ega, rgba, /*blink_on=*/true);
    WriteBmp(out_path, ibmpcat::kTextRenderWidth, ibmpcat::kTextRenderHeight, rgba);
    std::fprintf(stderr, "wrote %s (%dx%d)\n", out_path, ibmpcat::kTextRenderWidth, ibmpcat::kTextRenderHeight);
    return 0;
}
