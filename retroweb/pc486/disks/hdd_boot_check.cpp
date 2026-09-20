// Verifies that the shipped hard disk image actually boots -- the real bar
// for `make hdd-image` being done, rather than trusting the installer's own
// "complete" message. Cold-boots this machine with NOTHING but the finished
// HDD image mounted (no floppy, no CD, matching a real machine falling
// through to drive C: when drive A: is empty) and requires a genuine,
// interactive `C:\>` prompt on screen before reporting success.
//
// Usage:
//   hdd_boot_check <bios> <vgabios> <hdd.img> [max_cycles]
//
// Exits 0 only if the prompt appears; otherwise prints the last screen and
// exits non-zero. Like build_freedos_hdd.cpp this is a build-time check, not
// the shipped emulator, so taking real wall-clock time here is expected.

#include "../machine.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace pc486;

namespace {

std::vector<uint8_t> ReadFile(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Same planar-VRAM reconstruction build_freedos_hdd.cpp uses -- see that
// file; plane 0 is the character plane, and the CRTC's start address is
// followed so a scroll-by-start-offset stays correct.
std::string ScreenText(Machine &m) {
    const auto &vga = m.chipset.vga;
    std::string out;
    for (int row = 0; row < 25; ++row) {
        std::string line;
        for (int col = 0; col < 80; ++col) {
            uint32_t plane_off = (uint32_t(vga.start_offset()) + uint32_t(row * 80 + col)) & 0xFFFF;
            uint8_t ch = vga.vram[(plane_off << 2) + 0];
            line.push_back((ch >= 32 && ch < 127) ? char(ch) : ' ');
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line;
        out += '\n';
    }
    return out;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <bios> <vgabios> <hdd.img> [max_cycles]\n", argv[0]);
        return 2;
    }
    uint64_t budget = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 40'000'000'000ull;

    Machine m;
    m.reset();
    {
        auto bios = ReadFile(argv[1]);
        auto vga = ReadFile(argv[2]);
        if (bios.empty() || vga.empty()) { std::fprintf(stderr, "cannot open BIOS/VGABIOS\n"); return 2; }
        m.chipset.load_rom(0x100000 - uint32_t(bios.size()), bios.data(), bios.size());
        m.chipset.load_rom(0xC0000, vga.data(), vga.size());
    }
    {
        auto hdd = ReadFile(argv[3]);
        if (hdd.empty()) { std::fprintf(stderr, "cannot open %s\n", argv[3]); return 2; }
        const std::size_t kExpected = 1024ULL * 16 * 63 * 512;  // 528,482,304
        if (hdd.size() != kExpected) {
            std::fprintf(stderr, "FAILED: %s is %zu bytes, expected exactly %zu "
                                 "(1024 cyl / 16 head / 63 sec)\n", argv[3], hdd.size(), kExpected);
            return 1;
        }
        // Boot sector must carry the 55 AA signature or the BIOS will not
        // even attempt it -- checked up front so a bad image fails loudly
        // rather than after a full cycle budget of nothing happening.
        if (hdd[510] != 0x55 || hdd[511] != 0xAA) {
            std::fprintf(stderr, "FAILED: no 55 AA boot signature at offset 510 (got %02X %02X)\n",
                         hdd[510], hdd[511]);
            return 1;
        }
        m.chipset.hdd.mount(0, hdd.data(), hdd.size());
    }
    // Deliberately no floppy and no CD mounted: this proves the image boots
    // on its own, with the BIOS falling through drive A: to drive C: via the
    // CMOS boot sequence configure_factory_cmos() seeds.

    std::string last, prev;
    uint64_t still_since = 0;
    const uint64_t kChunk = 500'000;
    for (uint64_t used = 0; used < budget; used += kChunk) {
        m.run_cycles(int64_t(kChunk));
        std::string s = ScreenText(m);
        if (s != prev) { still_since = m.total_cycles(); prev = s; }
        last = s;
        // A prompt that has been sitting there unchanged for a while is an
        // idle shell waiting for input -- not a prompt string that merely
        // flashed past mid-boot.
        if (s.find("C:\\>") != std::string::npos &&
            m.total_cycles() - still_since > 330'000'000) {  // ~5s of emulated time
            std::fprintf(stderr, "OK: reached an idle C:\\> prompt at cycle %llu\n=== screen ===\n%s",
                         (unsigned long long)m.total_cycles(), s.c_str());
            return 0;
        }
    }
    std::fprintf(stderr, "FAILED: no idle C:\\> prompt within %llu cycles.\nLast screen:\n%s",
                 (unsigned long long)budget, last.c_str());
    return 1;
}
