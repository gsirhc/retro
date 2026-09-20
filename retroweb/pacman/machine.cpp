#include "machine.h"

#include <utility>

namespace pacman {
namespace {

// Address/data lines on U5/U6/U7 are wired scrambled (US 4,525,599). Bit
// orders are a cross-check of MAME init_mspacman, not a behavior source.
uint16_t bitswap(uint16_t val, const int* bits, int n) {
    uint16_t out = 0;
    for (int i = 0; i < n; i++)
        out |= uint16_t((val >> bits[i]) & 1) << (n - 1 - i);
    return out;
}

uint8_t aux_data(uint8_t v) {
    const int b[] = {0, 4, 5, 7, 6, 3, 2, 1};
    return uint8_t(bitswap(v, b, 8));
}

bool in_trap(uint16_t addr, uint16_t base) {
    return addr >= base && addr < uint16_t(base + 8);
}

// Forty 8-byte overlay windows: dest in $0000–$2FFF, source in decrypted
// aux ROM $8000–$81EF. PAL decode, cross-checked against MAME
// mspacman_install_patches.
constexpr std::pair<uint16_t, uint16_t> kPatches[] = {
    {0x0410, 0x8008}, {0x08E0, 0x81D8}, {0x0A30, 0x8118}, {0x0BD0, 0x80D8},
    {0x0C20, 0x8120}, {0x0E58, 0x8168}, {0x0EA8, 0x8198}, {0x1000, 0x8020},
    {0x1008, 0x8010}, {0x1288, 0x8098}, {0x1348, 0x8048}, {0x1688, 0x8088},
    {0x16B0, 0x8188}, {0x16D8, 0x80C8}, {0x16F8, 0x81C8}, {0x19A8, 0x80A8},
    {0x19B8, 0x81A8}, {0x2060, 0x8148}, {0x2108, 0x8018}, {0x21A0, 0x81A0},
    {0x2298, 0x80A0}, {0x23E0, 0x80E8}, {0x2418, 0x8000}, {0x2448, 0x8058},
    {0x2470, 0x8140}, {0x2488, 0x8080}, {0x24B0, 0x8180}, {0x24D8, 0x80C0},
    {0x24F8, 0x81C0}, {0x2748, 0x8050}, {0x2780, 0x8090}, {0x27B8, 0x8190},
    {0x2800, 0x8028}, {0x2B20, 0x8100}, {0x2B30, 0x8110}, {0x2BF0, 0x81D0},
    {0x2CC0, 0x80D0}, {0x2CD8, 0x80E0}, {0x2CF0, 0x81E0}, {0x2D60, 0x8160},
};

}  // namespace

Machine::Machine() : cpu(make_bus()) { reset(); }

z80::Bus Machine::make_bus() {
    z80::Bus b;
    b.read = [this](uint16_t a) { return mem_read(a); };
    b.write = [this](uint16_t a, uint8_t v) { mem_write(a, v); };
    b.in = [](uint8_t) { return uint8_t(0xFF); };
    // Only port 0 is ever exercised by the real ROM, so that's all we decode.
    b.out = [this](uint8_t port, uint8_t v) { if (port == 0) irq_vector = v; };
    b.irq_data = [this] { return irq_vector; };
    return b;
}

void Machine::reset() {
    ram.fill(0);
    irq_enable = false;
    irq_vector = 0xFF;
    watchdog_ = kWatchdogFrames;
    // watchdog_reset is sticky: run_cycles sets it when the 8-frame
    // watchdog elapses, then calls reset() to mimic the real pulse.
    // Clearing the flag here would hide the trip from tests.
    // The aux-board PAL latch is not on the Z80 RESET line.
    frames = 0;
    audio.clear();
    video.reset();
    wsg.reset();
    cpu.reset();
}

void Machine::rebuild_aux() {
    aux_decrypted_.fill(0);
    const int u7a[] = {11, 3, 7, 9, 10, 8, 6, 5, 4, 2, 1, 0};
    for (int i = 0; i < 0x1000; i++) {
        aux_decrypted_[unsigned(0x0000 + i)] = program[unsigned(0x0000 + i)];
        aux_decrypted_[unsigned(0x1000 + i)] = program[unsigned(0x1000 + i)];
        aux_decrypted_[unsigned(0x2000 + i)] = program[unsigned(0x2000 + i)];
        aux_decrypted_[unsigned(0x3000 + i)] =
            aux_data(aux_u7_[bitswap(uint16_t(i), u7a, 12)]);
    }
    const int u5a[] = {8, 7, 5, 9, 10, 6, 3, 4, 2, 1, 0};
    const int u6a[] = {3, 7, 9, 10, 8, 6, 5, 4, 2, 1, 0};
    for (int i = 0; i < 0x800; i++) {
        aux_decrypted_[unsigned(0x8000 + i)] =
            aux_data(aux_u5_[bitswap(uint16_t(i), u5a, 11)]);
        aux_decrypted_[unsigned(0x8800 + i)] =
            aux_data(aux_u6_[0x800 + bitswap(uint16_t(i), u6a, 11)]);
        aux_decrypted_[unsigned(0x9000 + i)] =
            aux_data(aux_u6_[bitswap(uint16_t(i), u6a, 11)]);
        aux_decrypted_[unsigned(0x9800 + i)] = program[unsigned(0x1800 + i)];
    }
    for (int i = 0; i < 0x1000; i++) {
        aux_decrypted_[unsigned(0xA000 + i)] = program[unsigned(0x2000 + i)];
        aux_decrypted_[unsigned(0xB000 + i)] = program[unsigned(0x3000 + i)];
    }
    for (const auto& p : kPatches) {
        for (int i = 0; i < 8; i++)
            aux_decrypted_[p.first + unsigned(i)] = aux_decrypted_[p.second + unsigned(i)];
    }
}

void Machine::load_roms(const RomSet& set) {
    program = set.program;
    video.tile_rom = set.tiles;
    video.sprite_rom = set.sprites;
    video.color_prom = set.color_prom;
    video.lookup_prom = set.lookup_prom;
    wsg.wave_prom = set.wave_prom;
    aux_board = set.aux_board;
    aux_u5_ = set.aux_u5;
    aux_u6_ = set.aux_u6;
    aux_u7_ = set.aux_u7;
    aux_decode = false;
    if (aux_board) rebuild_aux();
    else aux_decrypted_.fill(0);
}

void Machine::aux_trap(uint16_t addr) {
    if (!aux_board) return;
    // Any access (fetch, read, or write) trips the latch. Enable returns
    // decrypted data; disable returns the original Pac-Man byte.
    if (in_trap(addr, 0x3FF8)) {
        aux_decode = true;
        return;
    }
    if (in_trap(addr, 0x0038) || in_trap(addr, 0x03B0) || in_trap(addr, 0x1600) ||
        in_trap(addr, 0x2120) || in_trap(addr, 0x3FF0) || in_trap(addr, 0x8000) ||
        in_trap(addr, 0x97F0)) {
        aux_decode = false;
    }
}

uint8_t Machine::mem_read(uint16_t addr) {
    aux_trap(addr);
    // Stock Pac-Man PCB leaves A15 unconnected, so $8000–$FFFF mirror
    // $0000–$7FFF. The aux board is what actually decodes A15 as extra ROM.
    const uint16_t lo = addr & 0x7FFF;
    if (lo >= 0x4000 && lo < 0x4400) return video.videoram[lo - 0x4000];
    if (lo >= 0x4400 && lo < 0x4800) return video.colorram[lo - 0x4400];
    if (lo >= 0x4800 && lo < 0x5000) return ram[lo - 0x4800];
    if (lo >= 0x5000) {
        switch (lo & 0xFFC0) {
            case 0x5000: return inputs.in0;
            case 0x5040: return inputs.in1;
            case 0x5080: return inputs.dsw1;
            case 0x50C0: return inputs.dsw2;
            default: return 0xFF;
        }
    }
    if (aux_board && aux_decode) return aux_decrypted_[addr];
    return program[lo];
}

void Machine::mem_write(uint16_t addr, uint8_t v) {
    aux_trap(addr);
    const uint16_t lo = addr & 0x7FFF;
    if (lo < 0x4000) return;
    if (lo < 0x4400) {
        video.videoram[lo - 0x4000] = v;
        return;
    }
    if (lo < 0x4800) {
        video.colorram[lo - 0x4400] = v;
        return;
    }
    if (lo < 0x4FF0) {
        ram[lo - 0x4800] = v;
        return;
    }
    if (lo < 0x5000) {
        ram[lo - 0x4800] = v;
        video.spriteram[lo - 0x4FF0] = v;
        return;
    }
    if (lo == 0x5000) {
        irq_enable = (v & 1) != 0;
        return;
    }
    if (lo == 0x5001) {
        wsg.enabled = (v & 1) != 0;
        return;
    }
    if (lo == 0x5003) {
        video.flip_screen = (v & 1) != 0;
        return;
    }
    if (lo >= 0x5040 && lo < 0x5060) {
        wsg.write(lo - 0x5040, v);
        return;
    }
    if (lo >= 0x5060 && lo < 0x5070) {
        video.sprite_xy[lo - 0x5060] = v;
        return;
    }
    if ((lo & 0xFFC0) == 0x50C0) {
        watchdog_ = kWatchdogFrames;
        return;
    }
}

int Machine::run_cycles(int n) {
    int done = 0;
    while (done < n) {
        int t = cpu.step();
        done += t;
        video.advance(t);
        wsg.advance(t, audio_hz, audio);
        if (video.vblank_edge) {
            frames++;
            watchdog_--;
            if (watchdog_ <= 0) {
                watchdog_reset = true;
                reset();
                break;
            }
            if (irq_enable) cpu.interrupt();
        }
    }
    return done;
}

}  // namespace pacman
