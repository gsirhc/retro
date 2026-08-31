// Minimal CP/M host for 8080 diagnostics.
//
// Loads a .COM image at 0x0100 and runs it on the i8080 core, emulating just
// enough of CP/M's BDOS (console functions 2 and 9) to see the messages
// printed by TST8080.COM, 8080PRE.COM, CPUTEST.COM and 8080EXM.COM.
//
//   build:  make cpm                       (from Emulator8080/)
//   run:    ./cpm/cpm_host cpm/TST8080.COM
//
// Two things make a .COM run without a real CP/M:
//
//   1. A .COM file is a raw memory image of the Transient Program Area. It
//      loads at 0x0100 and execution starts there.
//
//   2. Programs reach the operating system with `CALL 0x0005` (the BDOS
//      entry) and exit with `RET` / `JMP 0x0000` (the warm-boot vector).
//      We have no CP/M, so we plant a one-byte port-I/O instruction at each
//      of those addresses. Executing it traps into this host; we service the
//      request straight from the CPU registers, and the real `RET` we also
//      planted at 0x0007 unwinds the caller's stack for us.

#include "../i8080.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

std::array<uint8_t, 0x10000> g_mem{};
bool g_finished = false;

// CP/M BDOS, cut down to what console diagnostics use.
//   C = 0x02  C_WRITE     — write the character in E
//   C = 0x09  C_WRITESTR  — write the '$'-terminated string at DE
void bdos_call(const i8080::Cpu &cpu) {
    switch (cpu.c) {
        case 0x02:
            std::putchar(static_cast<char>(cpu.e));
            break;
        case 0x09: {
            uint16_t addr = cpu.de();
            for (int guard = 0; g_mem[addr] != '$' && guard < 0x10000; ++guard)
                std::putchar(static_cast<char>(g_mem[addr++]));
            break;
        }
        default:
            std::fprintf(stderr, "\n[cpm_host: unhandled BDOS call C=%02X]\n", cpu.c);
            break;
    }
    std::fflush(stdout);
}

} // namespace

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "cpm/TST8080.COM";

    // --- load the .COM image at 0x0100 --------------------------------
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "cpm_host: cannot open %s\n", path);
        return 1;
    }
    const std::vector<char> image((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
    if (image.empty() || image.size() > (0x10000 - 0x0100)) {
        std::fprintf(stderr, "cpm_host: %s has bad size %zu\n", path, image.size());
        return 1;
    }
    for (std::size_t i = 0; i < image.size(); ++i)
        g_mem[0x0100 + i] = static_cast<uint8_t>(image[i]);

    // --- plant the CP/M call traps ----------------------------------
    g_mem[0x0000] = 0xD3;  // OUT 0x00,A  — warm boot: "the program exited"
    g_mem[0x0001] = 0x00;
    g_mem[0x0005] = 0xD3;  // OUT 0x01,A  — BDOS entry: "service a call"
    g_mem[0x0006] = 0x01;
    g_mem[0x0007] = 0xC9;  // RET         — returns to the CALL 0x0005 site

    // --- wire the bus ---------------------------------------------
    i8080::Cpu *cpu_ptr = nullptr;  // set right after the CPU is constructed

    i8080::Bus bus;
    bus.read  = [](uint16_t a)             { return g_mem[a]; };
    bus.write = [](uint16_t a, uint8_t v)  { g_mem[a] = v; };
    bus.in    = [](uint8_t) -> uint8_t     { return 0x00; };
    bus.out   = [&](uint8_t port, uint8_t) {
        if (port == 0x00) g_finished = true;
        else if (port == 0x01) bdos_call(*cpu_ptr);
    };

    i8080::Cpu cpu(bus);
    cpu_ptr = &cpu;
    cpu.reset();
    cpu.pc = 0x0100;  // enter the Transient Program Area

    // --- run ----------------------------------------------------
    const uint64_t kCycleCap = 1'000'000'000;  // safety net for a wedged core
    while (!g_finished && cpu.cycles < kCycleCap)
        cpu.step();

    std::putchar('\n');
    if (!g_finished) {
        std::fprintf(stderr, "[cpm_host: stopped at cycle cap, PC=%04X]\n", cpu.pc);
        return 2;
    }
    std::fprintf(stderr, "[cpm_host: program exited after %llu cycles]\n",
                 static_cast<unsigned long long>(cpu.cycles));
    return 0;
}
