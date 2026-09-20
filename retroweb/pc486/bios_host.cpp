// Native diagnostic harness: boots the real, freely-licensed BIOS-bochs-
// legacy image (+ optional vgabios and a floppy image) against this
// machine's chipset and reports what happens -- no browser, no WASM.
// Adapted from ibmpc-at/bios_host.cpp for the 80486 core; same technique
// (POST-code + wrch() character-print capture) applies unchanged since
// this machine boots the identical BIOS-bochs-legacy substitute.
// Machine::configure_factory_cmos() (called automatically by Machine's
// constructor) seeds the CMOS bytes this BIOS needs to actually attempt a
// boot instead of panicking immediately.
//
// Usage: bios_host <BIOS-bochs-legacy> [max_steps] [vgabios] [floppy-image]

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
        std::fprintf(stderr, "usage: %s <BIOS-bochs-legacy> [max_steps] [vgabios] [floppy-image]\n", argv[0]);
        return 2;
    }
    std::vector<uint8_t> bios = ReadFile(argv[1]);
    if (bios.empty()) { std::fprintf(stderr, "cannot open/empty: %s\n", argv[1]); return 2; }
    std::fprintf(stderr, "loaded %s: %zu bytes\n", argv[1], bios.size());

    uint64_t max_steps = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 50'000'000ull;

    pc486::Machine m;  // constructor seeds factory CMOS config, see file header
    m.reset();  // parks CPU at the real reset vector, F000:FFF0
    // BIOS-bochs-legacy is a 64KB image -- lands at the top of the address
    // space, F0000-FFFFF, exactly where the real x86 reset vector
    // (F000:FFF0 -> physical FFFF0) expects it, unchanged since the 8086.
    m.chipset.load_rom(0x100000 - bios.size(), bios.data(), bios.size());

    if (argc > 3) {
        std::vector<uint8_t> vga = ReadFile(argv[3]);
        std::fprintf(stderr, "loaded %s: %zu bytes at 0xC0000\n", argv[3], vga.size());
        m.chipset.load_rom(0xC0000, vga.data(), vga.size());
    }
    if (argc > 4) {
        std::vector<uint8_t> disk = ReadFile(argv[4]);
        std::fprintf(stderr, "mounted %s: %zu bytes on Drive A:\n", argv[4], disk.size());
        m.chipset.fdc.mount(0, disk.data(), disk.size());
    }

    uint8_t last_code = m.chipset.last_post_code();
    int same_code_streak = 0;
    std::string screen;
    uint32_t last_cs_ip = 0xFFFFFFFF;
    const int64_t kStep = 1;  // fine-grained so the wrch() capture below can't miss a call
    for (uint64_t total = 0; total < max_steps; total += kStep) {
        uint32_t cs_ip = (uint32_t(m.cpu.cs) << 16) | uint16_t(m.cpu.eip);
        if (m.cpu.cs == 0xF000 && uint16_t(m.cpu.eip) == 0x0679 && cs_ip != last_cs_ip) {
            char c = char(m.cpu.eax & 0xFF);
            screen.push_back(c == '\r' ? '\n' : ((c >= 32 && c < 127) ? c : '.'));
        }
        last_cs_ip = cs_ip;

        m.run_cycles(kStep);
        uint8_t code = m.chipset.last_post_code();
        if (code != last_code) {
            std::fprintf(stderr, "[cyc %10llu] POST code 0x%02X  CS:IP=%04X:%04X  halted=%d\n",
                         (unsigned long long)m.total_cycles(), code, m.cpu.cs, uint16_t(m.cpu.eip), m.cpu.halted);
            last_code = code;
            same_code_streak = 0;
        } else if (m.cpu.halted) {
            ++same_code_streak;
            if (same_code_streak == 1) {
                std::fprintf(stderr, "[cyc %10llu] CPU halted, CS:IP=%04X:%04X\n",
                             (unsigned long long)m.total_cycles(), m.cpu.cs, uint16_t(m.cpu.eip));
            }
        }
    }
    std::fprintf(stderr, "=== screen text ===\n%s\n=== end ===\n", screen.c_str());
    std::fprintf(stderr, "done: %llu cycles executed, final CS:IP=%04X:%04X, halted=%d, last POST code 0x%02X\n",
                 (unsigned long long)m.total_cycles(), m.cpu.cs, uint16_t(m.cpu.eip), m.cpu.halted, m.chipset.last_post_code());
    return 0;
}
