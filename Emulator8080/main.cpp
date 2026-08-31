// Minimal harness: a flat 64K RAM Altair-style machine with an 88-2SIO serial
// board on ports 0x10-0x13. Runs a small echo program and shuttles bytes
// through the board's ring buffers the way a web bridge eventually will.

#include "i8080.h"
#include "serial2sio.h"

#include <array>
#include <cstdio>
#include <string>

int main() {
    std::array<uint8_t, 0x10000> mem{};
    altair::Serial2SIO sio(0x10);

    i8080::Bus bus;
    bus.read  = [&](uint16_t a) { return mem[a]; };
    bus.write = [&](uint16_t a, uint8_t v) { mem[a] = v; };
    bus.in    = [&](uint8_t port) { return sio.in(port); };
    bus.out   = [&](uint8_t port, uint8_t v) { sio.out(port, v); };

    // Echo program: reset ACIA, configure 8N1, then loop reading a byte when
    // RDRF is set and writing it back when TDRE is set.
    //   0000  MVI A,03 / OUT 10        master reset channel A
    //   0004  MVI A,11 / OUT 10        /16 clock, 8N1, IRQ off
    //   0008  IN 10 / ANI 01 / JZ 0008 wait for RDRF
    //   000F  IN 11 / MOV B,A          read char
    //   0012  IN 10 / ANI 02 / JZ 0012 wait for TDRE
    //   0019  MOV A,B / OUT 11         echo it
    //   001C  JMP 0008
    const uint8_t prog[] = {
        0x3E, 0x03, 0xD3, 0x10,
        0x3E, 0x11, 0xD3, 0x10,
        0xDB, 0x10, 0xE6, 0x01, 0xCA, 0x08, 0x00,
        0xDB, 0x11, 0x47,
        0xDB, 0x10, 0xE6, 0x02, 0xCA, 0x12, 0x00,
        0x78, 0xD3, 0x11,
        0xC3, 0x08, 0x00,
    };
    for (size_t i = 0; i < sizeof(prog); ++i) mem[i] = prog[i];

    i8080::Cpu cpu(bus);
    cpu.reset();

    // Front end pushes keystrokes into the receive ring buffer.
    const std::string typed = "Hello, Altair!\r";
    sio.host_send(typed);
    std::printf("host typed %zu bytes, rx queue = %zu\n",
                typed.size(), sio.rx_pending());

    // Run until the echo program has transmitted the whole line back.
    int guard = 0;
    while (sio.tx_pending() < typed.size() && guard++ < 2'000'000)
        cpu.step();

    // Front end drains what the CPU transmitted.
    std::vector<uint8_t> echoed = sio.host_drain();
    std::string out(echoed.begin(), echoed.end());
    std::printf("cpu echoed %zu bytes after %llu cycles: \"", echoed.size(),
                (unsigned long long)cpu.cycles);
    for (uint8_t ch : echoed)
        std::printf("%s", ch == '\r' ? "\\r" : std::string(1, char(ch)).c_str());
    std::printf("\"\n");

    bool ok = out == typed;
    std::printf("echo %s\n", ok ? "OK" : "MISMATCH");
    return ok ? 0 : 1;
}
