#include "board.h"

#include <algorithm>

namespace mw8080 {

Board::Board() : cpu(make_bus()) {
    Board::reset();
}

i8080::Bus Board::make_bus() {
    i8080::Bus b;
    b.read = [this](uint16_t a) { return mem_read(a); };
    b.write = [this](uint16_t a, uint8_t v) { mem_write(a, v); };
    b.in = [this](uint8_t p) { return port_in(p); };
    b.out = [this](uint8_t p, uint8_t v) { port_out(p, v); };
    return b;
}

void Board::reset() {
    ram.fill(0);
    beam_.fill(0);
    shown_.fill(0);
    beam_tags_.fill(0);
    shown_tags_.fill(0);
    shifter = Shifter{};
    beam_tag = 0;
    cpu.reset();
    frame_origin_ = cpu.cycles;
    frame_cycle_ = 0;
    fetched_ = 0;
    int_ff_ = false;
    watchdog_ = kWatchdogFrames;
    watchdog_reset = false;
    frames = 0;
}

// A15 is not decoded.
uint8_t Board::mem_read(uint16_t addr) const {
    addr &= 0x7FFF;
    if (addr < 0x2000) return program[addr];
    if (addr < 0x4000) return ram[addr - 0x2000];
    if (addr < 0x6000) return program_hi[addr - 0x4000];
    return ram[addr - 0x6000];
}

void Board::mem_write(uint16_t addr, uint8_t v) {
    addr &= 0x7FFF;
    if (addr >= 0x2000 && addr < 0x4000) ram[addr - 0x2000] = v;
    else if (addr >= 0x6000) ram[addr - 0x6000] = v;
}

// Only A0–A2 decode.
uint8_t Board::port_in(uint8_t port) const {
    switch (port & 3) {
        case 0: return inputs.in0;
        case 1: return inputs.in1;
        case 2: return inputs.in2;
        default: return shifter.result();
    }
}

void Board::port_out(uint8_t port, uint8_t v) {
    switch (port & 7) {
        case 2: shifter.offset_w(v); break;
        case 4: shifter.data_w(v); break;
        case 3:
        case 5: sound_out(port & 7, v); break;
        case 6: if (watchdog_enabled) watchdog_ = kWatchdogFrames; break;
        default: break;
    }
}

uint8_t Board::vcounter(int line) {
    return line < kVblankLine ? uint8_t(line + 0x20) : uint8_t(line - kVblankLine + 0xDA);
}

// Taito SV game board (BPR00003) 6D gates drive D3/D4 at INTA from 64V; ISRs sit at $08 and $10.
uint8_t Board::rst_vector(int line) {
    return (vcounter(line) & 0x40) ? 0xD7 : 0xCF;
}

void Board::line_start(int line) {
    // E3 latches INTE on the rising edges of !64V&128V and VBLANK: lines 96 and 224.
    if (line == kMidLine || line == kVblankLine) int_ff_ = cpu.int_enabled;
    if (line < kNativeH) beam_tags_[size_t(line)] = beam_tag;
    if (line == kVblankLine) {
        shown_ = beam_;
        shown_tags_ = beam_tags_;
        frames++;
        if (watchdog_enabled) watchdog_--;
    }
}

void Board::catch_up() {
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

void Board::watchdog_fire() {
    // The watchdog pulses /RESET; RAM, the beam and the sound latches carry on.
    const uint64_t c = cpu.cycles;
    cpu.reset();
    cpu.cycles = c;
    int_ff_ = false;
    watchdog_ = kWatchdogFrames;
    watchdog_reset = true;
}

void Board::advance(int t) {
    elapsed(t);
    catch_up();
}

int Board::run_cycles(int n) {
    int done = 0;
    while (done < n) {
        const int t = cpu.step();
        done += t;
        advance(t);
        if (int_ff_) {
            // INTA clears E3; the vector reflects 64V at acknowledge time.
            const int a = cpu.interrupt(rst_vector(line()));
            if (a) {
                int_ff_ = false;
                done += a;
                advance(a);
            }
        }
        if (watchdog_enabled && watchdog_ <= 0) watchdog_fire();
    }
    return done;
}

bool Board::lit(int ux, int uy) const {
    const int k = column(uy);
    if (k < 0) return false;
    return (shown_[size_t(ux) * 32 + size_t(k)] >> (7 - (uy & 7))) & 1;
}

}  // namespace mw8080
