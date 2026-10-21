#include "machine.h"

#include <algorithm>
#include <cmath>

namespace missile {

namespace {

// MAD13..MAD0 for a MADSEL access (DP-147 sheet 2A, L2 K2 J2 H2 M2 muxes, M3 and L3 gates).
uint16_t dram_address(uint16_t ba, bool mushroom) {
    if (!mushroom) return uint16_t((ba >> 2) & 0x3FFF);
    uint16_t a = uint16_t((ba >> 12) & 1);
    a |= uint16_t(((ba >> 3) & 0xFF) << 1);
    a |= uint16_t(((ba >> 11) & 1) << 10);
    a |= uint16_t((~ba >> 11 & 1) << 9);
    return a;
}

// MUSHROOM is BA15 BA14 BA13 all high (N3, sheet 2A); ΦEXTEND adds a cycle there.
bool in_third_colour_range(uint16_t ba) { return (ba & 0xE000) == 0xE000; }

uint32_t palette_rgba(uint8_t cell) {
    // L7 82S25 returns the complement of BD3, BD2, BD1 on UNLRED, UNLGREEN, UNLBLUE.
    uint32_t r = (cell & 0x08) ? 0 : 0xFF;
    uint32_t g = (cell & 0x04) ? 0 : 0xFF;
    uint32_t b = (cell & 0x02) ? 0 : 0xFF;
    return 0xFF000000u | r | (g << 8) | (b << 16);
}

}  // namespace

Machine::Machine() : cpu(make_bus()) {
    set_default_prom();
    pokey.set_clock(kPokeyHz);
    // AUD into R38 10K to +5 V and C70 0.1 uF, buffered by N10 (DP-147 sheet 2B).
    pokey.set_output_rc(10e3, 0.1e-6, 5.0);
    reset();
}

mos6502::Bus Machine::make_bus() {
    mos6502::Bus b;
    b.read = [this](uint16_t a) { return bus_read(a); };
    b.write = [this](uint16_t a, uint8_t v) { bus_write(a, v); };
    return b;
}

void Machine::reset() {
    vram.fill(0);
    palette.fill(0);
    frames = 0;
    watchdog_reset = false;
    flipscreen = false;
    ctrld = false;
    outlatch = 0;
    line_ = 0;
    line_used_ = 0;
    audio.clear();
    hp_ = 5.0;
    soft_reset();
}

void Machine::soft_reset() {
    cpu.rebind_bus(make_bus());
    cpu.reset();
    pokey.reset();
    irq_state_ = false;
    irq_pin_ = false;
    irqh_ = false;
    cpu.irq_line = false;
    madsel_armed_ = false;
    watchdog_frames_ = kWatchdogFrames;
    clock_irq();
}

void Machine::set_default_prom() {
    // L6 outputs WP0-WP7, low enables a chip (write enable table, sheet 2A).
    prom.fill(0x00);
    for (int i = 0; i < 8; i++) {
        prom[size_t(0x10 | i)] = uint8_t(~(0x11 << (i & 3)));
        prom[size_t(0x18 | i)] = uint8_t(~(1 << i));
    }
}

void Machine::load_roms(const RomSet& set) {
    program = set.program;
    if (set.has_prom) prom = set.prom;
    else set_default_prom();
    reset();
}

uint8_t Machine::mem_read(uint16_t addr) const {
    uint16_t a = addr & 0x7FFF;
    if (a < 0x4000) return vram[a];
    if (a < 0x4800) {
        uint8_t reg = uint8_t(a & 0x0F);
        if (reg == 0x08) return inputs.r8;  // ALLPOT, R8 on the pot lines
        return pokey.read(reg);
    }
    if (a < 0x4900) {
        if (!ctrld) return inputs.in0;
        if (flipscreen) return 0;  // cocktail P2 trackball, unmapped
        return uint8_t(((inputs.track_y & 0x0F) << 4) | (inputs.track_x & 0x0F));
    }
    if (a < 0x4A00) return uint8_t((inputs.in1 & 0x67) | (vblank() ? 0x80 : 0));
    if (a < 0x4B00) return inputs.r10;
    if (a >= 0x5000) return program[a - 0x5000];
    return 0xFF;
}

void Machine::mem_write(uint16_t addr, uint8_t v) {
    uint16_t a = addr & 0x7FFF;
    if (a < 0x4000) {
        vram[a] = v;
    } else if (a < 0x4800) {
        pokey.write(uint8_t(a & 0x0F), v);
    } else if (a < 0x4900) {
        outlatch = v;
        ctrld = (v & 0x01) != 0;
        flipscreen = (v & 0x40) == 0;
    } else if (a >= 0x4B00 && a < 0x4C00) {
        palette[a & 7] = v;
    } else if (a >= 0x4C00 && a < 0x4D00) {
        kick_watchdog();
    } else if (a >= 0x4D00 && a < 0x4E00) {
        irq_state_ = false;
    }
}

void Machine::dram_write_cycle(uint16_t ba, bool mushroom, uint8_t d) {
    // L6 address is MADSEL, MUSHROOM, BA2-BA0.
    uint8_t wp = prom[size_t(0x10 | (mushroom ? 0x08 : 0x00) | (ba & 7))];
    // Data selector table: chips 0-3 take D6, 4-7 take D7, or every chip takes D5.
    uint8_t md = mushroom ? uint8_t((d & 0x20) ? 0xFF : 0x00)
                          : uint8_t(((d & 0x80) ? 0xF0 : 0x00) | ((d & 0x40) ? 0x0F : 0x00));
    uint8_t& cell = vram[dram_address(ba, mushroom)];
    cell = uint8_t((cell & wp) | (md & ~wp));
}

void Machine::mad_write(uint16_t ba, uint8_t d) {
    dram_write_cycle(ba, false, d);
    if (in_third_colour_range(ba)) dram_write_cycle(ba, true, d);
}

uint8_t Machine::mad_read(uint16_t ba) const {
    uint8_t byte = vram[dram_address(ba, false)];
    // P7 picks bit BA1-BA0 of each nibble, E7 latches them onto D7 and D6.
    int sel = ba & 3;
    uint8_t latched = uint8_t((((byte >> (4 + sel)) & 1) << 7) | (((byte >> sel) & 1) << 6));
    if (in_third_colour_range(ba)) byte = vram[dram_address(ba, true)];
    // LS251 on D5 picks bit BA2-BA0 of the byte on the DRAM outputs.
    uint8_t d5 = uint8_t(((byte >> (ba & 7)) & 1) << 5);
    return uint8_t(0x1F | latched | d5);
}

// MADSEL lands on the 6th cycle after a ($zz,X) SYNC, which is the data
// access: the core's 5th bus access, since it skips the zp dummy read.
uint8_t Machine::bus_read(uint16_t addr) {
    if (madsel_armed_ && access_++ == 4) {
        madsel_armed_ = false;
        madsel_hits++;
        if ((addr & 0xE000) == 0xE000) extra_cycles_++;
        return mad_read(addr);
    }
    return mem_read(addr);
}

void Machine::bus_write(uint16_t addr, uint8_t v) {
    if (madsel_armed_ && access_++ == 4) {
        madsel_armed_ = false;
        madsel_hits++;
        if ((addr & 0xE000) == 0xE000) extra_cycles_++;
        mad_write(addr, v);
        return;
    }
    mem_write(addr, v);
}

void Machine::next_line() {
    size_t from = audio.size();
    pokey.mix(kCyclesPerLine, audio_hz, audio);
    double a = 1 - std::exp(-2 * M_PI * kAmpCouplingHz / audio_hz);
    for (size_t i = from; i < audio.size(); i++) {
        hp_ += (audio[i] - hp_) * a;
        audio[i] = float(std::clamp((hp_ - audio[i]) / kAudioScale, -1.0, 1.0));
    }
    if (++line_ >= kLines) {
        line_ = 0;
        frames++;
        if (--watchdog_frames_ <= 0) {
            watchdog_reset = true;
            soft_reset();
        }
    }
    clock_irq();
}

void Machine::clock_irq() {
    // F7 clocks 32V on 16V; FLIP selects /16V (DP-147 sheet 1B).
    int v = v_of(line_);
    if ((v & 0x1F) == (flipscreen ? 0x10 : 0x00)) irq_state_ = (v & 0x20) == 0;
}

int Machine::run_cycles(int n) {
    int done = 0;
    while (done < n) {
        // SYNC: F7 latches /IRQ, E7 keeps the previous one as /IRQH.
        uint8_t op = mem_read(cpu.pc);
        cpu.irq_line = irq_pin_;
        irqh_ = irq_pin_;
        irq_pin_ = irq_state_;
        madsel_armed_ = !irqh_ && (op & 0x1F) == 0x01;
        access_ = 0;
        extra_cycles_ = 0;
        madsel_hits = 0;
        int c = cpu.step() + extra_cycles_;
        madsel_armed_ = false;
        done += c;
        line_used_ += c;
        while (line_used_ >= line_cycles(line_)) {
            line_used_ -= line_cycles(line_);
            next_line();
        }
    }
    return done;
}

void Machine::render(uint32_t* rgba) const {
    uint32_t pens[8];
    for (int i = 0; i < 8; i++) pens[i] = palette_rgba(palette[size_t(i)]);
    for (int y = kVisTop; y < kLines; y++) {
        // FLIP counts V down, so the same rows show in reverse.
        int row = flipscreen ? ((255 + kVisTop - y) & 0xFF) : y;
        bool third = row >= kSlowV;
        uint32_t* dst = rgba + (y - kVisTop) * kFbW;
        for (int x = 0; x < kFbW; x++) {
            uint8_t byte = vram[size_t(row * 64 + (x >> 2))];
            // N7 shifts the high nibble, M6 the low nibble, P6 the 3rd-colour byte.
            int pen = (((byte >> (4 + (x & 3))) & 1) << 2) | (((byte >> (x & 3)) & 1) << 1);
            if (third) {
                uint8_t b3 = vram[dram_address(uint16_t((row << 8) | x), true)];
                pen |= (b3 >> (x & 7)) & 1;
            }
            dst[x] = pens[pen];
        }
    }
}

}  // namespace missile
