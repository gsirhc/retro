// Runs cpu65c02::Cpu against Klaus Dormann's 6502/65C02 functional test
// suite (https://github.com/Klaus2m5/6502_65C02_functional_tests) -- the
// same validation role TST8080/CPUTEST/8080EXM play for the Altair's i8080
// core (see retroweb/altair8800/cpm/cpm_host.cpp).
//
// The image is a full 64K flat memory snapshot; the test drives itself by
// self-modifying/branching through every opcode and addressing mode and
// traps into an infinite `jmp *` self-loop -- at a documented "success"
// address if every check passed, at the failing test's own address
// otherwise. `make -C .. dormann` fetches+checksums the two prebuilt
// binaries this program is pointed at and knows their success addresses.

#include "../cpu65c02.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <image.bin> <success_pc_hex>\n", argv[0]);
        return 2;
    }
    std::ifstream f(argv[1], std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "can't open %s\n", argv[1]);
        return 2;
    }
    std::vector<uint8_t> mem(65536, 0);
    f.read(reinterpret_cast<char *>(mem.data()), 65536);
    uint16_t success_pc = uint16_t(std::strtol(argv[2], nullptr, 16));

    cpu65c02::Bus bus;
    bus.read  = [&](uint16_t addr) { return mem[addr]; };
    bus.write = [&](uint16_t addr, uint8_t v) { mem[addr] = v; };
    cpu65c02::Cpu cpu(bus);
    cpu.reset();
    cpu.pc = 0x0400;   // documented entry point for both test images

    constexpr uint64_t kMaxInstructions = 200'000'000;
    for (uint64_t i = 0; i < kMaxInstructions; i++) {
        uint16_t pc_before = cpu.pc;
        cpu.step();
        if (cpu.pc != pc_before) continue;   // only a self-jump traps
        if (cpu.pc == success_pc) {
            std::printf("PASS  %s: trapped at success address $%04X after %llu instructions\n",
                        argv[1], cpu.pc, static_cast<unsigned long long>(i));
            return 0;
        }
        std::printf("FAIL  %s: trapped at $%04X after %llu instructions (expected success at $%04X)\n"
                    "      check the matching .lst for what test that address belongs to\n",
                    argv[1], cpu.pc, static_cast<unsigned long long>(i), success_pc);
        return 1;
    }
    std::printf("FAIL  %s: did not trap within %llu instructions (last pc $%04X)\n",
                argv[1], static_cast<unsigned long long>(kMaxInstructions), cpu.pc);
    return 1;
}
