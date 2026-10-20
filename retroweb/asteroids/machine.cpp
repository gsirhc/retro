#include "machine.h"

#include <algorithm>
#include <cmath>

namespace asteroids {

namespace {

uint16_t mirror(uint16_t addr) {
    // A15 is not decoded (DP-143 memory map).
    return addr & 0x7FFF;
}

// The LS251 drives D7 only; R14-R16, R9-R11 pull the rest of the bus high.
uint8_t bit7(bool v) { return v ? 0xFF : 0x7F; }

}  // namespace

Machine::Machine() : cpu(make_bus()) {
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
    dvg.reset();
    ram.fill(0);
    bank1_.fill(0);
    vector_ram.fill(0);
    frames = 0;
    ramsel = false;
    lamps = 0;
    watchdog_cycles_ = kWatchdogCycles;
    cycles_to_nmi_ = kCyclesPerNmi;
    clk3k_phase_ = 0;
    audio_acc_ = 0;
    sound.reset();
    sound.set_rate(audio_hz);
    audio_hz_set_ = audio_hz;
    audio.clear();
}

void Machine::load_roms(const RomSet& set) {
    program = set.program;
    vector_rom = set.vector;
    reset();
}

void Machine::kick_watchdog() { watchdog_cycles_ = kWatchdogCycles; }

void Machine::tick_watchdog(int cycles) {
    watchdog_cycles_ -= cycles;
    if (watchdog_cycles_ > 0) return;
    watchdog_reset = true;
    reset();
    watchdog_reset = true;
}

void Machine::service_nmi() {
    // TEST holds C5's enable low (DP-143 sheet 1B).
    if ((inputs.in0 & 0x80) == 0)
        cpu.nmi();
    frames++;
}

uint8_t Machine::read_banked(uint16_t addr) const {
    if (addr < 0x0200) return ram[addr];
    uint8_t off = uint8_t(addr);
    if (addr < 0x0300)
        return ramsel ? bank1_[off] : ram[0x200 + off];
    return ramsel ? ram[0x200 + off] : bank1_[off];
}

void Machine::write_banked(uint16_t addr, uint8_t v) {
    if (addr < 0x0200) {
        ram[addr] = v;
        return;
    }
    uint8_t off = uint8_t(addr);
    if (addr < 0x0300) {
        if (ramsel) bank1_[off] = v;
        else ram[0x200 + off] = v;
        return;
    }
    if (ramsel) ram[0x200 + off] = v;
    else bank1_[off] = v;
}

void Machine::dvg_go() {
    auto read_word = [this](uint16_t word_addr) -> uint16_t {
        // Words 0000-07FF = vector RAM ($4000), 0800-0FFF = vector ROM ($5000).
        word_addr &= 0x0FFF;
        if (word_addr < 0x0800) {
            uint16_t off = uint16_t(word_addr * 2);
            if (off + 1 < vector_ram.size())
                return uint16_t(vector_ram[off]) | (uint16_t(vector_ram[off + 1]) << 8);
            return 0;
        }
        uint16_t off = uint16_t((word_addr - 0x0800) * 2);
        if (off + 1 < vector_rom.size())
            return uint16_t(vector_rom[off]) | (uint16_t(vector_rom[off + 1]) << 8);
        return 0;
    };
    dvg.go(read_word);
}

uint8_t Machine::mem_read(uint16_t addr) const {
    addr = mirror(addr);
    if (addr < 0x0400) return read_banked(addr);

    // E4 LS139 on A11/A10 picks SINP0, SINP1 or OPTS; A2-A0 pick the switch.
    switch (addr & 0x7C00) {
        case 0x2000: {
            int bit = addr & 7;
            bool v = false;
            if (bit == 1) v = ((clk3k_phase_ / kCyclesPer3kHz) & 1) != 0;
            else if (bit == 2) v = !dvg.halted();
            else if (bit > 2) v = (inputs.in0 & (1u << bit)) != 0;
            return bit7(v);
        }
        case 0x2400:
            return bit7((inputs.in1 & (1u << (addr & 7))) != 0);
        case 0x2800:
            return uint8_t(0xFC | ((inputs.dsw1 >> (2 * (addr & 3))) & 0x03));
        default:
            break;
    }

    if (addr >= 0x4000 && addr < 0x4800) return vector_ram[addr - 0x4000];
    if (addr >= 0x5000 && addr < 0x5800) return vector_rom[addr - 0x5000];
    if (addr >= 0x6800 && addr < 0x8000) return program[addr - 0x6800];
    return 0xFF;
}

void Machine::mem_write(uint16_t addr, uint8_t v) {
    addr = mirror(addr);
    if (addr < 0x0400) {
        write_banked(addr, v);
        return;
    }
    if (addr >= 0x4000 && addr < 0x4800) {
        vector_ram[addr - 0x4000] = v;
        return;
    }
    if ((addr & 0xF000) != 0x3000) return;
    // L6 LS42 decodes A11-A9 into the $3000-$3E00 write strobes.
    switch (addr & 0x0E00) {
        case 0x0000: dvg_go(); break;
        case 0x0200:
            lamps = v;
            ramsel = (v & 0x04) != 0;
            break;
        case 0x0400: kick_watchdog(); break;
        case 0x0600: sound.write_explode(v); break;
        case 0x0A00: sound.write_thump(v); break;
        case 0x0C00: sound.write_latch(addr & 7, (v & 0x80) != 0); break;
        case 0x0E00: sound.noise_reset(); break;
        default: break;
    }
}

void Machine::mix_audio(int cycles) {
    if (audio_hz != audio_hz_set_) {
        sound.set_rate(audio_hz);
        audio_hz_set_ = audio_hz;
    }
    audio_acc_ += int64_t(cycles) * audio_hz;
    while (audio_acc_ >= kCpuHz) {
        audio_acc_ -= kCpuHz;
        audio.push_back(sound.sample());
    }
}

int Machine::run_cycles(int n) {
    int left = n;
    while (left > 0) {
        if (cycles_to_nmi_ <= 0) {
            service_nmi();
            cycles_to_nmi_ += kCyclesPerNmi;
        }
        int slice = std::min({left, cycles_to_nmi_, watchdog_cycles_});
        if (slice <= 0) slice = 1;
        int used = 0;
        while (used < slice) {
            int c = cpu.step();
            used += c;
            clk3k_phase_ += uint64_t(c);
        }
        cycles_to_nmi_ -= used;
        left -= used;
        tick_watchdog(used);
        mix_audio(used);
    }
    return n;
}

void Machine::render(uint32_t* rgba) const {
    std::fill(rgba, rgba + kFbW * kFbH, 0xFF000000u);
    auto plot = [&](int px, int py, int bri) {
        if (unsigned(px) >= 1024 || unsigned(py) >= 1024) return;
        uint32_t& dst = rgba[py * kFbW + px];
        int cur = int(dst & 0xFF);
        if (bri > cur) dst = 0xFF000000u | (uint32_t(bri) * 0x010101u);
    };
    // Dim neighbour glow keeps strokes visible when downscaled.
    auto plot_beam = [&](int px, int py, int bri) {
        plot(px, py, bri);
        int dim = bri / 3;
        if (dim > 0) {
            plot(px - 1, py, dim);
            plot(px + 1, py, dim);
            plot(px, py - 1, dim);
            plot(px, py + 1, dim);
        }
    };
    for (const atari::VectorSeg& seg : dvg.segments) {
        if (seg.intensity == 0) continue;
        int x0 = seg.x0, y0 = 1023 - seg.y0, x1 = seg.x1, y1 = 1023 - seg.y1;
        int bri = 40 + seg.intensity * 14;
        if (bri > 255) bri = 255;
        if (x0 == x1 && y0 == y1) {
            plot_beam(x0, y0, bri);
            plot_beam(x0 - 1, y0, bri / 2);
            plot_beam(x0 + 1, y0, bri / 2);
            plot_beam(x0, y0 - 1, bri / 2);
            plot_beam(x0, y0 + 1, bri / 2);
            continue;
        }
        int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        for (;;) {
            plot_beam(x0, y0, bri);
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
}

}  // namespace asteroids
