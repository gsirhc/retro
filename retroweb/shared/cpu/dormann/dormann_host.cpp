// Runs mos6502::Cpu against Klaus Dormann's 6502 functional test
// (github.com/Klaus2m5/6502_65C02_functional_tests).

#include "../cpu_mos6502.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

    mos6502::Bus bus;
    bus.read  = [&](uint16_t addr) { return mem[addr]; };
    bus.write = [&](uint16_t addr, uint8_t v) { mem[addr] = v; };
    mos6502::Cpu cpu(bus);
    cpu.reset();
    cpu.pc = 0x0400;

    constexpr uint64_t kMaxInstructions = 200'000'000;
    for (uint64_t i = 0; i < kMaxInstructions; i++) {
        uint16_t pc_before = cpu.pc;
        cpu.step();
        if (cpu.pc != pc_before) continue;
        if (cpu.pc == success_pc) {
            std::printf("PASS  %s: trapped at success address $%04X after %llu instructions\n",
                        argv[1], cpu.pc, static_cast<unsigned long long>(i));
            return 0;
        }
        std::printf("FAIL  %s: trapped at $%04X after %llu instructions (expected success at $%04X)\n",
                    argv[1], cpu.pc, static_cast<unsigned long long>(i), success_pc);
        return 1;
    }
    std::printf("FAIL  %s: did not trap within %llu instructions (last pc $%04X)\n",
                argv[1], static_cast<unsigned long long>(kMaxInstructions), cpu.pc);
    return 1;
}
