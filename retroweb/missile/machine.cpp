#include "machine.h"

namespace missile {

namespace {

uint16_t bit3_addr(uint16_t pixaddr) {
    // Third colour bit scatter, MAME missile.cpp get_bit3_addr (from schematics).
    return uint16_t(((pixaddr & 0x0800) >> 1) | ((~pixaddr & 0x0800) >> 2) |
                    ((pixaddr & 0x07F8) >> 2) | ((pixaddr & 0x1000) >> 12));
}

}  // namespace

Machine::Machine() : cpu(make_bus()) {
    set_default_prom();
    pokey.set_clock(kPokeyHz);
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
    // L6 write masks: keep every pixel but the addressed one (MAME vram_mad_r/w).
    prom.fill(0xFF);
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

uint8_t Machine::vram_mad_read(uint16_t addr) const {
    uint8_t result = 0xFF;
    uint8_t d = vram[addr >> 2] & uint8_t(0x11 << (addr & 3));
    if ((d & 0xF0) == 0) result &= ~0x80;
    if ((d & 0x0F) == 0) result &= ~0x40;
    if ((addr & 0xE000) == 0xE000) {
        if ((vram[bit3_addr(addr)] & (1 << (addr & 7))) == 0) result &= ~0x20;
    }
    return result;
}

void Machine::vram_mad_write(uint16_t addr, uint8_t v) {
    static const uint8_t kSpread[4] = {0x00, 0x0F, 0xF0, 0xFF};
    uint8_t& cell = vram[addr >> 2];
    uint8_t mask = prom[(addr & 7) | 0x10];
    cell = uint8_t((cell & mask) | (kSpread[v >> 6] & ~mask));
    if ((addr & 0xE000) == 0xE000) {
        uint8_t& c3 = vram[bit3_addr(addr)];
        uint8_t m3 = prom[(addr & 7) | 0x18];
        uint8_t d3 = (v & 0x20) ? 0xFF : 0x00;
        c3 = uint8_t((c3 & m3) | (d3 & ~m3));
    }
}

// MADSEL lands on the 6th cycle after a ($zz,X) SYNC, which is the data
// access: the core's 5th bus access, since it skips the zp dummy read.
uint8_t Machine::bus_read(uint16_t addr) {
    if (madsel_armed_ && access_++ == 4) {
        madsel_armed_ = false;
        madsel_hits++;
        if ((addr & 0xE000) == 0xE000) extra_cycles_++;
        return vram_mad_read(addr);
    }
    return mem_read(addr);
}

void Machine::bus_write(uint16_t addr, uint8_t v) {
    if (madsel_armed_ && access_++ == 4) {
        madsel_armed_ = false;
        madsel_hits++;
        if ((addr & 0xE000) == 0xE000) extra_cycles_++;
        vram_mad_write(addr, v);
        return;
    }
    mem_write(addr, v);
}

void Machine::next_line() {
    pokey.mix(kCyclesPerLine, audio_hz, audio);
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
    // F7 clocks 32V on 16V edges; /16V when flipped (MAME schedule_next_irq).
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
    for (int i = 0; i < 8; i++) {
        uint8_t d = uint8_t(~palette[size_t(i)]);
        uint32_t r = (d & 0x08) ? 0xFF : 0;
        uint32_t g = (d & 0x04) ? 0xFF : 0;
        uint32_t b = (d & 0x02) ? 0xFF : 0;
        pens[i] = 0xFF000000u | r | (g << 8) | (b << 16);
    }
    for (int y = kVisTop; y < kLines; y++) {
        int effy = flipscreen ? ((256 + 24 - y) & 0xFF) : y;
        const uint8_t* src = &vram[size_t(effy * 64)];
        const uint8_t* src3 = effy >= kSlowV ? &vram[bit3_addr(uint16_t(effy << 8))] : nullptr;
        uint32_t* dst = rgba + (y - kVisTop) * kFbW;
        for (int x = 0; x < kFbW; x++) {
            uint8_t p = uint8_t(src[x / 4] >> (x & 3));
            int pix = ((p >> 2) & 4) | ((p << 1) & 2);
            if (src3) pix |= (src3[(x / 8) * 2] >> (x & 7)) & 1;
            dst[x] = pens[pix];
        }
    }
}

}  // namespace missile
