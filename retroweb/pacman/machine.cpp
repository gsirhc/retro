#include "machine.h"

namespace pacman {

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
    watchdog_reset = false;
    frames = 0;
    audio.clear();
    video.reset();
    wsg.reset();
    cpu.reset();
}

void Machine::load_roms(const RomSet& set) {
    program = set.program;
    video.tile_rom = set.tiles;
    video.sprite_rom = set.sprites;
    video.color_prom = set.color_prom;
    video.lookup_prom = set.lookup_prom;
    wsg.wave_prom = set.wave_prom;
}

uint8_t Machine::mem_read(uint16_t addr) {
    if (addr < 0x4000) return program[addr];
    if (addr < 0x4400) return video.videoram[addr - 0x4000];
    if (addr < 0x4800) return video.colorram[addr - 0x4400];
    if (addr < 0x5000) return ram[addr - 0x4800];
    switch (addr & 0xFFC0) {
        case 0x5000: return inputs.in0;
        case 0x5040: return inputs.in1;
        case 0x5080: return inputs.dsw1;
        case 0x50C0: return inputs.dsw2;
        default: return 0xFF;
    }
}

void Machine::mem_write(uint16_t addr, uint8_t v) {
    if (addr < 0x4000) return;
    if (addr < 0x4400) {
        video.videoram[addr - 0x4000] = v;
        return;
    }
    if (addr < 0x4800) {
        video.colorram[addr - 0x4400] = v;
        return;
    }
    if (addr < 0x4FF0) {
        ram[addr - 0x4800] = v;
        return;
    }
    if (addr < 0x5000) {
        ram[addr - 0x4800] = v;
        video.spriteram[addr - 0x4FF0] = v;
        return;
    }
    if (addr == 0x5000) {
        irq_enable = (v & 1) != 0;
        return;
    }
    if (addr == 0x5001) {
        wsg.enabled = (v & 1) != 0;
        return;
    }
    if (addr == 0x5003) {
        video.flip_screen = (v & 1) != 0;
        return;
    }
    if (addr >= 0x5040 && addr < 0x5060) {
        wsg.write(addr - 0x5040, v);
        return;
    }
    if (addr >= 0x5060 && addr < 0x5070) {
        video.sprite_xy[addr - 0x5060] = v;
        return;
    }
    if ((addr & 0xFFC0) == 0x50C0) {
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
