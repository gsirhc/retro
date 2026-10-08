// Native harness: boots BIOS-bochs-legacy (+ optional EGA BIOS and floppy)
// against this chipset and reports POST codes, HLT, and the screen text.
// Screen text is captured by logging each char passed to the BIOS wrch()
// helper (F000:0679 in this build).
//
// Usage: bios_host <BIOS-bochs-legacy> [max_steps] [egabios] [floppy-image]

#include "machine.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {
std::vector<uint8_t> ReadFile(const char *path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <BIOS-bochs-legacy> [max_steps] [egabios] [floppy-image]\n", argv[0]);
        return 2;
    }
    std::vector<uint8_t> bios = ReadFile(argv[1]);
    if (bios.empty()) { std::fprintf(stderr, "cannot open/empty: %s\n", argv[1]); return 2; }
    std::fprintf(stderr, "loaded %s: %zu bytes\n", argv[1], bios.size());

    uint64_t max_steps = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 50'000'000ull;

    ibmpcat::Machine m;
    m.reset();  // reset vector F000:FFF0
    // 64KB image lands at F0000-FFFFF where the 80286 reset vector expects it.
    m.chipset.load_rom(0x100000 - bios.size(), bios.data(), bios.size());

    if (argc > 3) {
        std::vector<uint8_t> vbios = ReadFile(argv[3]);
        std::fprintf(stderr, "loaded %s: %zu bytes at 0xC0000\n", argv[3], vbios.size());
        m.chipset.load_rom(0xC0000, vbios.data(), vbios.size());
    }
    if (argc > 4) {
        std::vector<uint8_t> disk = ReadFile(argv[4]);
        std::fprintf(stderr, "mounted %s: %zu bytes on Drive A:\n", argv[4], disk.size());
        m.chipset.fdc.mount(0, disk.data(), disk.size());
    }

    uint8_t last_code = m.chipset.last_post_code();
    int same_code_streak = 0;
    std::string screen;
    uint16_t last_cs = 0, last_ip = 0;
    const int64_t kStep = 1;  // fine-grained so the wrch() capture below can't miss a call
    for (uint64_t total = 0; total < max_steps; total += kStep) {
        uint16_t cs = m.cpu.cs, ip = m.cpu.ip;
        if (cs == 0xF000 && ip == 0x0679 && !(last_cs == 0xF000 && last_ip == 0x0679)) {
            char c = char(m.cpu.ax & 0xFF);
            screen.push_back(c == '\r' ? '\n' : ((c >= 32 && c < 127) ? c : '.'));
        }
        last_cs = cs; last_ip = ip;

        m.run_cycles(kStep);
        uint8_t code = m.chipset.last_post_code();
        if (code != last_code) {
            std::fprintf(stderr, "[cyc %10llu] POST code 0x%02X  CS:IP=%04X:%04X  halted=%d\n",
                         (unsigned long long)m.total_cycles(), code, m.cpu.cs, m.cpu.ip, m.cpu.halted);
            last_code = code;
            same_code_streak = 0;
        } else if (m.cpu.halted) {
            ++same_code_streak;
            if (same_code_streak == 1) {
                std::fprintf(stderr, "[cyc %10llu] CPU halted, CS:IP=%04X:%04X\n",
                             (unsigned long long)m.total_cycles(), m.cpu.cs, m.cpu.ip);
            }
        }
    }
    std::fprintf(stderr, "=== screen text ===\n%s\n=== end ===\n", screen.c_str());
    std::fprintf(stderr, "done: %llu cycles executed, final CS:IP=%04X:%04X, halted=%d, last POST code 0x%02X\n",
                 (unsigned long long)m.total_cycles(), m.cpu.cs, m.cpu.ip, m.cpu.halted, m.chipset.last_post_code());
    return 0;
}
