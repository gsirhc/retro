#include "machine.h"

#include <algorithm>

namespace invaders {

Machine::Machine() : cpu(make_bus()) {
    reset();
}

i8080::Bus Machine::make_bus() {
    i8080::Bus b;
    b.read = [this](uint16_t a) { return mem_read(a); };
    b.write = [this](uint16_t a, uint8_t v) { mem_write(a, v); };
    b.in = [this](uint8_t p) { return port_in(p); };
    b.out = [this](uint8_t p, uint8_t v) { port_out(p, v); };
    return b;
}

void Machine::reset() {
    ram.fill(0);
    beam_.fill(0);
    shown_.fill(0);
    shifter = Mb14241{};
    sound.reset();
    cpu.reset();
    frame_origin_ = cpu.cycles;
    frame_cycle_ = 0;
    fetched_ = 0;
    int_ff_ = false;
    watchdog_ = kWatchdogFrames;
    watchdog_reset = false;
    frames = 0;
    audio.clear();
    audio_acc_ = 0;
}

void Machine::load_roms(const RomSet& set) {
    program = set.program;
}

// A15 is not decoded; $4000–$5FFF is an empty ROM socket (MAME main_map).
uint8_t Machine::mem_read(uint16_t addr) const {
    addr &= 0x7FFF;
    if (addr < 0x2000) return program[addr];
    if (addr < 0x4000) return ram[addr - 0x2000];
    if (addr < 0x6000) return 0x00;
    return ram[addr - 0x6000];
}

void Machine::mem_write(uint16_t addr, uint8_t v) {
    addr &= 0x7FFF;
    if (addr >= 0x2000 && addr < 0x4000) ram[addr - 0x2000] = v;
    else if (addr >= 0x6000) ram[addr - 0x6000] = v;
}

// Only A0–A2 decode (MAME invaders io_map).
uint8_t Machine::port_in(uint8_t port) const {
    switch (port & 3) {
        case 0: return inputs.in0;
        case 1: return inputs.in1;
        case 2: return inputs.in2;
        default: return shifter.result();
    }
}

void Machine::port_out(uint8_t port, uint8_t v) {
    switch (port & 7) {
        case 2: shifter.count_w(v); break;
        case 3: sound.port3_w(v); break;
        case 4: shifter.data_w(v); break;
        case 5: sound.port5_w(v); break;
        case 6: watchdog_ = kWatchdogFrames; break;
        default: break;
    }
}

uint8_t Machine::vcounter(int line) {
    return line < kVblankLine ? uint8_t(line + 0x20) : uint8_t(line - kVblankLine + 0xDA);
}

// 0xC7 | 64V << 4 | !64V << 3, sampled at INTA (MAME mw8080bw.cpp interrupt notes).
uint8_t Machine::rst_vector(int line) {
    const uint8_t v = vcounter(line);
    return uint8_t(0xC7 | ((v & 0x40) >> 2) | ((~v & 0x40) >> 3));
}

void Machine::line_start(int line) {
    // E3 latches INTE on the rising edges of !64V&128V and VBLANK: lines 96 and 224.
    if (line == kMidLine || line == kVblankLine) int_ff_ = cpu.int_enabled;
    if (line == kVblankLine) {
        shown_ = beam_;
        frames++;
        watchdog_--;
    }
}

void Machine::catch_up() {
    uint64_t target = cpu.cycles - frame_origin_;
    while (frame_cycle_ < target) {
        const int ln = int(frame_cycle_ / kLineCycles);
        const uint32_t line_end = uint32_t(ln + 1) * kLineCycles;
        const uint32_t upto = uint32_t(std::min<uint64_t>(target, line_end));
        if (ln < kNativeH) {
            // Byte k loads the shift register at pixel 8k+4; 2.5 pixels per T-state.
            const uint32_t half = (upto - uint32_t(ln) * kLineCycles) * 5;
            const int want = half >= 8 ? std::min(32, int((half - 8) / 16) + 1) : 0;
            const size_t row = size_t(ln) * 32;
            for (; fetched_ < want; ++fetched_)
                beam_[row + size_t(fetched_)] = ram[(kVramBase - kRamBase) + row + size_t(fetched_)];
        }
        frame_cycle_ = upto;
        if (frame_cycle_ == line_end) {
            fetched_ = 0;
            int next = ln + 1;
            if (next == kLines) {
                next = 0;
                frame_origin_ += kFrameCycles;
                frame_cycle_ = 0;
                target -= kFrameCycles;
            }
            line_start(next);
        }
    }
}

void Machine::watchdog_fire() {
    // The watchdog pulses /RESET; RAM, the beam and the sound latches carry on.
    const uint64_t c = cpu.cycles;
    cpu.reset();
    cpu.cycles = c;
    int_ff_ = false;
    watchdog_ = kWatchdogFrames;
    watchdog_reset = true;
}

void Machine::elapse(int t) {
    sound.advance(t);
    if (audio_hz > 0) {
        audio_acc_ += t;
        const double step = double(kCpuHz) / double(audio_hz);
        while (audio_acc_ >= step) {
            audio_acc_ -= step;
            audio.push_back(sound.mix());
        }
    }
    catch_up();
}

int Machine::run_cycles(int n) {
    int done = 0;
    while (done < n) {
        const int t = cpu.step();
        done += t;
        elapse(t);
        if (int_ff_) {
            // INTA clears E3; the vector reflects 64V at acknowledge time.
            const int a = cpu.interrupt(rst_vector(line()));
            if (a) {
                int_ff_ = false;
                done += a;
                elapse(a);
            }
        }
        if (watchdog_ <= 0) watchdog_fire();
    }
    return done;
}

void Machine::render(uint32_t* upright) const {
    for (int uy = 0; uy < kUprightH; uy++) {
        const int x = kNativeW - 1 - uy;
        for (int ux = 0; ux < kUprightW; ux++) {
            bool on = false;
            if (x >= 4) {
                const int k = (x - 4) >> 3;
                on = (shown_[size_t(ux) * 32 + size_t(k)] >> ((x - 4) & 7)) & 1;
            }
            upright[size_t(uy) * kUprightW + size_t(ux)] = on ? 0xFFFFFFu : 0u;
        }
    }
}

}  // namespace invaders
