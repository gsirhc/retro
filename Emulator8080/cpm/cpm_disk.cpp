// CP/M boot smoke test for the 88-DCDD controller.
//
// Builds a full Altair: i8080 + 88-2SIO console + 88-DCDD + the MITS disk
// bootstrap PROM at 0xFF00. Mounts the diskette image given on the command line
// in drive 0, runs the turnkey boot, and passes (exit 0) once the console
// output contains the CP/M drive prompt ("A>"). Optional extra arguments are
// typed at the prompt, one per subsequent "A>".
//
//   build:  make cpmdisk         (from Emulator8080/)
//   run:    ./cpm/cpm_disk DISK01.DSK
//           ./cpm/cpm_disk DISK01.DSK DIR STAT
//
// Standard MITS 8-inch images are 337,568 bytes (77 x 32 x 137). See
// web/disks/README.md for where to get them.

#include "../i8080.h"
#include "../serial2sio.h"
#include "../disk88.h"
#include "../disk_bootrom.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <disk-image> [command ...]\n", argv[0]);
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "cpm_disk: cannot open %s\n", argv[1]);
        return 2;
    }
    std::vector<uint8_t> image((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    std::fprintf(stderr, "cpm_disk: %s (%zu bytes)\n", argv[1], image.size());

    auto mem = std::make_unique<std::array<uint8_t, 0x10000>>();
    altair::Serial2SIO sio(0x10);
    altair::Disk88 disk;
    disk.mount(0, image.data(), image.size());
    for (int i = 0; i < 256; ++i) (*mem)[altair::kDiskBootAddr + i] = altair::kDiskBootRom[i];

    i8080::Bus bus;
    bus.read  = [&](uint16_t a) { return (*mem)[a]; };
    bus.write = [&](uint16_t a, uint8_t v) { (*mem)[a] = v; };
    bus.in    = [&](uint8_t p) -> uint8_t {
        if (sio.owns(p))  return sio.in(p);
        if (disk.owns(p)) return disk.in(p);
        return 0xFF;
    };
    bus.out   = [&](uint8_t p, uint8_t v) {
        if (sio.owns(p))  sio.out(p, v);
        else if (disk.owns(p)) disk.out(p, v);
    };

    i8080::Cpu cpu(bus);
    cpu.reset();
    cpu.pc = altair::kDiskBootAddr;

    std::vector<std::string> cmds;
    for (int i = 2; i < argc; ++i) cmds.push_back(argv[i]);

    std::string out;
    std::size_t prompts_seen = 0, cmd_idx = 0;
    const uint64_t kCycleCap = 2'000'000'000;   // generous: boot is a few 10s of M

    while (cpu.cycles < kCycleCap) {
        cpu.step();
        uint8_t b;
        while (sio.host_recv(b)) { b &= 0x7F; out.push_back(char(b)); std::fputc(b, stdout); }

        std::size_t n = 0, p = 0;
        while ((p = out.find("A>", p)) != std::string::npos) { ++n; p += 2; }
        if (n > prompts_seen && sio.rx_pending() == 0) {
            prompts_seen = n;
            if (cmd_idx < cmds.size())
                sio.host_send(cmds[cmd_idx++] + "\r");
            else if (cmd_idx == cmds.size())
                break;                          // reached the prompt after the last command
        }
    }
    std::fflush(stdout);
    std::putchar('\n');

    if (out.find("A>") == std::string::npos) {
        std::fprintf(stderr, "[cpm_disk: never reached the CP/M prompt after %llu cycles]\n",
                     static_cast<unsigned long long>(cpu.cycles));
        return 1;
    }
    std::fprintf(stderr, "[cpm_disk: booted CP/M in %llu cycles]\n",
                 static_cast<unsigned long long>(cpu.cycles));
    return 0;
}
