#include "machine.h"

#include <algorithm>

namespace pacman {
namespace {

// Aux-board EPROM wiring from tools/derive_aux.py; U6 and data match schematic M051-00617-B002.
// Entry k is the EPROM pin that Z80 line k reaches.
constexpr int kAuxDataPin[8] = {1, 2, 3, 6, 7, 5, 4, 0};
constexpr int kU5AddrPin[11] = {0, 1, 2, 4, 3, 8, 5, 9, 10, 7, 6};
constexpr int kU6U7AddrPin[12] = {0, 1, 2, 10, 3, 4, 5, 9, 6, 8, 7, 11};

unsigned eprom_address(unsigned cpu, const int* pin, int lines) {
    unsigned chip = 0;
    for (int k = 0; k < lines; k++)
        if (cpu & (1u << k)) chip |= 1u << pin[k];
    return chip;
}

uint8_t eprom_data(uint8_t chip) {
    uint8_t cpu = 0;
    for (int k = 0; k < 8; k++)
        if (chip & (1u << kAuxDataPin[k])) cpu |= uint8_t(1u << k);
    return cpu;
}

bool in_trap(uint16_t addr, uint16_t base) {
    return addr >= base && addr < uint16_t(base + 8);
}

// U5 $8000-$81EF holds forty 8-byte code windows; the rest of that page is blank $FF.
struct AuxPatch {
    uint16_t u5;
    uint16_t window;
};
constexpr AuxPatch kAuxPatches[] = {
    {0x000, 0x2418}, {0x008, 0x0410}, {0x010, 0x1008}, {0x018, 0x2108},
    {0x020, 0x1000}, {0x028, 0x2800}, {0x048, 0x1348}, {0x050, 0x2748},
    {0x058, 0x2448}, {0x080, 0x2488}, {0x088, 0x1688}, {0x090, 0x2780},
    {0x098, 0x1288}, {0x0A0, 0x2298}, {0x0A8, 0x19A8}, {0x0C0, 0x24D8},
    {0x0C8, 0x16D8}, {0x0D0, 0x2CC0}, {0x0D8, 0x0BD0}, {0x0E0, 0x2CD8},
    {0x0E8, 0x23E0}, {0x100, 0x2B20}, {0x110, 0x2B30}, {0x118, 0x0A30},
    {0x120, 0x0C20}, {0x140, 0x2470}, {0x148, 0x2060}, {0x160, 0x2D60},
    {0x168, 0x0E58}, {0x180, 0x24B0}, {0x188, 0x16B0}, {0x190, 0x27B8},
    {0x198, 0x0EA8}, {0x1A0, 0x21A0}, {0x1A8, 0x19B8}, {0x1C0, 0x24F8},
    {0x1C8, 0x16F8}, {0x1D0, 0x2BF0}, {0x1D8, 0x08E0}, {0x1E0, 0x2CF0},
};

}  // namespace

Machine::Machine() : cpu(make_bus()) { reset(); }

z80::Bus Machine::make_bus() {
    z80::Bus b;
    b.read = [this](uint16_t a) { return mem_read(a); };
    b.write = [this](uint16_t a, uint8_t v) { mem_write(a, v); };
    b.in = [](uint16_t) { return uint8_t(0xFF); };
    // Only A0-A7 are decoded; port 0 latches the vector.
    b.out = [this](uint16_t port, uint8_t v) { if ((port & 0xFF) == 0) irq_vector = v; };
    b.irq_data = [this] { return irq_vector; };
    b.tick = [this](int t) { video.advance(t); };
    return b;
}

void Machine::reset() {
    coin_line_[0] = coin_line_[1] = false;
    ram.fill(0);
    irq_enable = false;
    irq_vector = 0xFF;
    watchdog_ = kWatchdogFrames;
    // watchdog_reset is sticky so tests see the trip. The aux PAL latch is not on Z80 RESET.
    frames = 0;
    audio.clear();
    video.reset();
    wsg.reset();
    cpu.reset();
}

void Machine::rebuild_aux() {
    // U5 decodes A0-A10 at $8000-$87FF, U6 A0-A11 at $8800-$97FF.
    for (unsigned cpu = 0x8000; cpu < 0x9800; cpu++) {
        const unsigned off = cpu - 0x8000;
        aux_high_[off] = cpu < 0x8800
            ? eprom_data(aux_u5_[eprom_address(cpu & 0x7FF, kU5AddrPin, 11)])
            : eprom_data(aux_u6_[eprom_address(cpu & 0xFFF, kU6U7AddrPin, 12)]);
    }
    std::copy(program.begin(), program.begin() + 0x3000, aux_low_.begin());
    for (unsigned off = 0; off < 0x1000; off++)
        aux_low_[0x3000 + off] = eprom_data(aux_u7_[eprom_address(off, kU6U7AddrPin, 12)]);
    for (const AuxPatch& p : kAuxPatches)
        std::copy_n(aux_high_.begin() + p.u5, 8, aux_low_.begin() + p.window);
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
    aux_low_.fill(0);
    aux_high_.fill(0);
    if (aux_board) rebuild_aux();
}

void Machine::aux_trap(uint16_t addr) {
    if (!aux_board) return;
    // Any access (fetch, read, or write) trips the latch.
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
    if (aux_board && aux_decode) {
        if (addr < 0x4000) return aux_low_[addr];
        if (addr >= 0x8000 && addr < 0x9800) return aux_high_[addr - 0x8000u];
    }
    // Stock PCB leaves A15 unconnected, so $8000-$FFFF mirror $0000-$7FFF.
    const uint16_t lo = addr & 0x7FFF;
    if (lo < 0x4000) return program[lo];
    // A13 is not decoded above $4000 (Lomont, Pac-Man hardware notes v0.1: $6000 mirrors $4000).
    const uint16_t m = lo & 0x5FFF;
    if (m < 0x4400) return video.videoram[m - 0x4000];
    if (m < 0x4800) return video.colorram[m - 0x4400];
    // No RAM at $4800-$4BFF; undriven bus reads $BF.
    if (m < 0x4C00) return 0xBF;
    if (m < 0x5000) return ram[m - 0x4C00];
    switch ((m >> 6) & 3) {
        case 0: return inputs.in0;
        case 1: return inputs.in1;
        case 2: return inputs.dsw1;
        default: return inputs.dsw2;
    }
}

void Machine::mem_write(uint16_t addr, uint8_t v) {
    aux_trap(addr);
    const uint16_t lo = addr & 0x7FFF;
    if (lo < 0x4000) return;
    const uint16_t m = lo & 0x5FFF;
    if (m < 0x4400) {
        video.videoram[m - 0x4000] = v;
        return;
    }
    if (m < 0x4800) {
        video.colorram[m - 0x4400] = v;
        return;
    }
    if (m < 0x4C00) return;
    if (m < 0x5000) {
        ram[m - 0x4C00] = v;
        if (m >= 0x4FF0) video.spriteram[m - 0x4FF0] = v;
        return;
    }
    // A8-A11 are not decoded in the I/O page.
    const uint16_t io = m & 0x50FF;
    if (io < 0x5040) {
        // 74LS259 main latch, repeated every 8 bytes.
        switch (io & 7) {
            case 0:
                // Clearing the mask also clears the held vblank request.
                irq_enable = (v & 1) != 0;
                if (!irq_enable) cpu.set_int(false);
                return;
            case 1: wsg.enabled = (v & 1) != 0; return;
            case 3: video.flip_screen = (v & 1) != 0; return;
            case 7: coin_counter_w(0, (v & 1) != 0); return;
            default: return;
        }
    }
    if (io < 0x5060) {
        wsg.write(io - 0x5040, v);
        return;
    }
    if (io < 0x5070) {
        video.sprite_xy[io - 0x5060] = v;
        return;
    }
    if (io >= 0x50C0) watchdog_ = kWatchdogFrames;
}

void Machine::watchdog_fire() {
    // The watchdog pulses /RESET; RAM keeps its contents, unlike a power cycle.
    const auto keep_ram = ram;
    const auto keep_v_videoram = video.videoram;
    const auto keep_v_colorram = video.colorram;
    const auto keep_v_spriteram = video.spriteram;
    const auto keep_v_sprite_xy = video.sprite_xy;
    reset();
    ram = keep_ram;
    video.videoram = keep_v_videoram;
    video.colorram = keep_v_colorram;
    video.spriteram = keep_v_spriteram;
    video.sprite_xy = keep_v_sprite_xy;
    watchdog_reset = true;
}

void Machine::coin_counter_w(int n, bool on) {
    if (on && !coin_line_[n]) coin_counter[unsigned(n)]++;
    coin_line_[n] = on;
}

int Machine::run_cycles(int n) {
    int done = 0;
    while (done < n) {
        int t = cpu.step();
        done += t;
        wsg.advance(t, audio_hz, audio);
        if (video.vblank_edge) {
            video.vblank_edge = false;
            frames++;
            watchdog_--;
            if (watchdog_ <= 0) {
                watchdog_fire();
                break;
            }
            if (irq_enable) cpu.set_int(true);
        }
    }
    return done;
}

}  // namespace pacman
