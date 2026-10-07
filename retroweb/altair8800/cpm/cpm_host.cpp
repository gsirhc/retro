// Minimal CP/M host for 8080 diagnostics (TST8080, 8080PRE, CPUTEST, 8080EXM).
// A .COM loads at 0x0100. BDOS functions 2 and 9 are emulated by trapping an OUT
// planted at the BDOS entry (0x0005) and warm-boot vector (0x0000).

#include "../i8080.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::array<uint8_t, 0x10000> g_mem{};
bool g_finished = false;
std::string g_console;   // everything the diagnostic printed, for pass/fail scan

void emit(char ch) {
    std::putchar(ch);
    g_console.push_back(ch);
}

// BDOS subset: C=2 writes E, C=9 writes the '$'-terminated string at DE.
void bdos_call(const i8080::Cpu &cpu) {
    switch (cpu.c) {
        case 0x02:
            emit(static_cast<char>(cpu.e));
            break;
        case 0x09: {
            uint16_t addr = cpu.de();
            for (int guard = 0; g_mem[addr] != '$' && guard < 0x10000; ++guard)
                emit(static_cast<char>(g_mem[addr++]));
            break;
        }
        default:
            std::fprintf(stderr, "\n[cpm_host: unhandled BDOS call C=%02X]\n", cpu.c);
            break;
    }
    std::fflush(stdout);
}

// Diagnostics report failure by printing "ERROR" or "CPU HAS FAILED".
bool diagnostic_failed() {
    std::string up = g_console;
    for (char &c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return up.find("ERROR") != std::string::npos ||
           up.find("FAIL")  != std::string::npos;
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
    const uint64_t kCycleCap = 40'000'000'000;  // safety net for a wedged core
    while (!g_finished && cpu.cycles < kCycleCap)
        cpu.step();

    std::putchar('\n');
    if (!g_finished) {
        std::fprintf(stderr, "[cpm_host: stopped at cycle cap, PC=%04X]\n", cpu.pc);
        return 2;
    }
    std::fprintf(stderr, "[cpm_host: program exited after %llu cycles]\n",
                 static_cast<unsigned long long>(cpu.cycles));
    if (diagnostic_failed()) {
        std::fprintf(stderr, "[cpm_host: diagnostic reported a failure]\n");
        return 3;
    }
    return 0;
}
