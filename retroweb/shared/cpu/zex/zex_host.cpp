// Minimal CP/M host for Frank Cringle's Z80 instruction exerciser
// (zexdoc.com). Same role TST8080 / Klaus Dormann play for the 8080 and
// 6502 cores: an independent, documented-opcode functional suite.
//
//   build:  make zexdoc
//   run:    ./zex/zex_host zex/zexdoc.com
//
// A .COM image loads at 0x0100. CALL 0x0005 is BDOS; JMP 0x0000 is warm
// boot. We plant OUT traps at those addresses, matching
// retroweb/altair8800/cpm/cpm_host.cpp.

#include "../cpu_z80.h"

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
std::string g_console;

void emit(char ch) {
    std::putchar(ch);
    g_console.push_back(ch);
}

void bdos_call(const z80::Cpu& cpu) {
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
            std::fprintf(stderr, "\n[zex_host: unhandled BDOS call C=%02X]\n", cpu.c);
            break;
    }
    std::fflush(stdout);
}

bool diagnostic_failed() {
    std::string up = g_console;
    for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return up.find("ERROR") != std::string::npos ||
           up.find("FAIL") != std::string::npos;
}

bool tests_complete() {
    std::string up = g_console;
    for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return up.find("TESTS COMPLETE") != std::string::npos;
}

}  // namespace

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "zex/zexdoc.com";

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "zex_host: cannot open %s\n", path);
        return 1;
    }
    const std::vector<char> image((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
    if (image.empty() || image.size() > (0x10000 - 0x0100)) {
        std::fprintf(stderr, "zex_host: %s has bad size %zu\n", path, image.size());
        return 1;
    }
    for (std::size_t i = 0; i < image.size(); ++i)
        g_mem[0x0100 + i] = static_cast<uint8_t>(image[i]);

    g_mem[0x0000] = 0xD3;  // OUT 0x00,A — warm boot
    g_mem[0x0001] = 0x00;
    g_mem[0x0005] = 0xD3;  // OUT 0x01,A — BDOS
    g_mem[0x0006] = 0x01;
    g_mem[0x0007] = 0xC9;  // RET

    z80::Cpu* cpu_ptr = nullptr;
    z80::Bus bus;
    bus.read = [](uint16_t a) { return g_mem[a]; };
    bus.write = [](uint16_t a, uint8_t v) { g_mem[a] = v; };
    bus.in = [](uint8_t) -> uint8_t { return 0x00; };
    bus.out = [&](uint8_t port, uint8_t) {
        if (port == 0x00) g_finished = true;
        else if (port == 0x01) bdos_call(*cpu_ptr);
    };

    z80::Cpu cpu(bus);
    cpu_ptr = &cpu;
    cpu.reset();
    cpu.pc = 0x0100;

    // zexdoc is a few tens of billions of T-states on a correct core
    // (each case has CRC/setup overhead; alu8r alone is 753k cases).
    const uint64_t kCycleCap = 50'000'000'000;
    while (!g_finished && cpu.cycles < kCycleCap)
        cpu.step();

    std::putchar('\n');
    std::fflush(stdout);
    if (!g_finished) {
        std::fprintf(stderr, "[zex_host: stopped at cycle cap, PC=%04X]\n", cpu.pc);
        return 2;
    }
    std::fprintf(stderr, "[zex_host: program exited after %llu cycles]\n",
                 static_cast<unsigned long long>(cpu.cycles));
    if (diagnostic_failed() || !tests_complete()) {
        std::fprintf(stderr, "[zex_host: diagnostic reported a failure]\n");
        return 3;
    }
    return 0;
}
