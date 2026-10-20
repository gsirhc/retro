#include "machine.h"

#include <algorithm>
#include <cmath>

namespace battlezone {

Machine::Machine() : cpu(make_bus()) {
    avg_read_ = [this](uint16_t a) { return mem_read(a); };
    pokey.set_clock(kCpuHz);
    pokey.set_output_rc(10e3, 0.015e-6, 5.0);  // R36, C25 (DP-156 sheet 3B)
    reset();
}

mos6502::Bus Machine::make_bus() {
    mos6502::Bus b;
    b.read  = [this](uint16_t a) { return mem_read(a); };
    b.write = [this](uint16_t a, uint8_t v) { mem_write(a, v); };
    return b;
}

void Machine::reset() {
    cpu.rebind_bus(make_bus());
    cpu.reset();
    avg.reset();
    mathbox.reset();
    pokey.reset();
    sound.reset();
    sound.set_rate(audio_hz);
    audio_hz_set_ = audio_hz;
    ram.fill(0);
    vector_ram.fill(0);
    nmis = 0;
    coin_counters = 0;
    cycles_to_nmi_ = kCyclesPerNmi;
    watchdog_cycles_ = kWatchdogCycles;
    clk3k_ = 0;
    audio.clear();
}

void Machine::load_roms(const RomSet& set) {
    program = set.program;
    vector_rom = set.vector;
    const uint8_t* nib[6];
    for (int i = 0; i < 6; i++) nib[i] = set.mb_ucode[size_t(i)].data();
    mathbox.load_proms(set.mb_map.data(), nib);
    reset();
}

void Machine::tick_watchdog(int cycles) {
    watchdog_cycles_ -= cycles;
    if (watchdog_cycles_ > 0) return;
    reset();
    watchdog_reset = true;
}

uint8_t Machine::mem_read(uint16_t addr) const {
    addr &= 0x7FFF;  // A15 not decoded; $FFFA reads the vectors at $7FFA
    if (addr < 0x0400) return ram[addr];
    switch (addr) {
        case 0x0800: {
            // R11 LS244: D6 HALT, D7 3KHZ (DP-156 sheet 2B).
            uint8_t v = inputs.in0 & 0x3F;
            if (avg.halted()) v |= 0x40;
            if (clk3k_ & 0x100) v |= 0x80;
            return v;
        }
        case 0x0A00: return inputs.dsw0;
        case 0x0C00: return inputs.dsw1;
        case 0x1800: return mathbox.status();
        case 0x1810: return mathbox.lo();
        case 0x1818: return mathbox.hi();
        default: break;
    }
    if (addr >= 0x1820 && addr < 0x1830) return pokey.read(uint8_t(addr & 0x0F));
    if (addr >= 0x2000 && addr < 0x3000) return vector_ram[addr - 0x2000];
    if (addr >= 0x3000 && addr < 0x4000) return vector_rom[addr - 0x3000];
    if (addr >= 0x5000) return program[addr - 0x5000];
    return 0;
}

void Machine::mem_write(uint16_t addr, uint8_t v) {
    addr &= 0x7FFF;
    if (addr < 0x0400) {
        ram[addr] = v;
        return;
    }
    if (addr >= 0x2000 && addr < 0x3000) {
        vector_ram[addr - 0x2000] = v;
        return;
    }
    if (addr >= 0x1820 && addr < 0x1830) {
        pokey.write(uint8_t(addr & 0x0F), v);
        return;
    }
    if (addr >= 0x1860 && addr < 0x1880) {
        mathbox.write(uint8_t(addr & 0x1F), v);
        return;
    }
    switch (addr) {
        case 0x1000: coin_counters = v; break;
        case 0x1200: avg.go(); break;
        case 0x1400: watchdog_cycles_ = kWatchdogCycles; break;
        case 0x1600: avg.vgrst(); break;
        case 0x1840: sound.write(v); break;
        default: break;
    }
}

void Machine::mix_audio(int cycles) {
    if (audio_hz != audio_hz_set_) {
        sound.set_rate(audio_hz);
        audio_hz_set_ = audio_hz;
    }
    pokey_buf_.clear();
    pokey.mix(cycles, audio_hz, pokey_buf_);
    audio.reserve(audio.size() + pokey_buf_.size());
    for (float p : pokey_buf_) audio.push_back(sound.sample(p));
}

int Machine::run_cycles(int n) {
    int left = n;
    while (left > 0) {
        if (cycles_to_nmi_ <= 0) {
            cycles_to_nmi_ += kCyclesPerNmi;
            if (inputs.in0 & 0x10) cpu.nmi();
            nmis++;
        }
        int slice = std::min({left, cycles_to_nmi_, watchdog_cycles_, 512});
        if (slice <= 0) slice = 1;
        int used = 0;
        while (used < slice) {
            int c = cpu.step();
            used += c;
            clk3k_ += uint64_t(c);
            mathbox.run(c);
            avg.run(c, avg_read_);
        }
        pokey.allpot = inputs.in3;
        cycles_to_nmi_ -= used;
        left -= used;
        mix_audio(used);
        tick_watchdog(used);
    }
    return n;
}

void Machine::render(uint32_t* rgba) const {
    std::fill(rgba, rgba + kFbW * kFbH, 0xFF000000u);
    auto plot = [&](int px, int py, int bri) {
        if (unsigned(px) >= unsigned(kFbW) || unsigned(py) >= unsigned(kFbH)) return;
        bool red = py < int(kFbH * kOverlaySplit);
        uint32_t hi = uint32_t(bri), lo = uint32_t(bri) / 8;
        uint32_t col = red ? (hi | (lo << 8) | (lo << 16)) : (lo | (hi << 8) | (lo << 16));
        uint32_t& dst = rgba[py * kFbW + px];
        uint32_t cur = std::max(dst & 0xFF, (dst >> 8) & 0xFF);
        if (uint32_t(bri) > cur) dst = 0xFF000000u | col;
    };
    for (const atari::AvgSeg& s : avg.frame) {
        int bri = std::min(255, 60 + s.z * 13);
        int x0 = int(std::lround(s.x0)), y0 = int(std::lround(s.y0));
        int x1 = int(std::lround(s.x1)), y1 = int(std::lround(s.y1));
        int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        for (;;) {
            plot(x0, y0, bri);
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
}

}  // namespace battlezone
