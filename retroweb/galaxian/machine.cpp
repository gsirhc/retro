#include "machine.h"

namespace galaxian {

Machine::Machine() : cpu(make_bus()) {
    video.board = Board::Galaxian;
    reset();
}

z80::Bus Machine::make_bus() {
    z80::Bus b;
    b.read = [this](uint16_t a) { return mem_read(a); };
    b.write = [this](uint16_t a, uint8_t v) { mem_write(a, v); };
    b.in = [](uint16_t) { return uint8_t(0xFF); };
    b.out = [](uint16_t, uint8_t) {};
    b.irq_data = [] { return uint8_t(0xFF); };
    b.tick = [this](int t) { video.advance(t); };
    return b;
}

void Machine::reset() {
    ram.fill(0);
    nmi_enable = false;
    coin_lockout = true;
    coin_line_[0] = coin_line_[1] = false;
    watchdog_ = kWatchdogFrames;
    frames = 0;
    audio.clear();
    audio_acc_ = 0;
    video.reset();
    video.board = Board::Galaxian;
    sound.reset();
    cpu.reset();
}

void Machine::load_roms(const RomSet& set) {
    program = set.program;
    video.gfx = set.gfx;
    video.color_prom = set.color_prom;
}

uint8_t Machine::mem_read(uint16_t addr) {
    if (addr < 0x4000) return program[addr];
    if (addr < 0x4800) return ram[(addr - 0x4000) & 0x3FF];
    if (addr >= 0x5000 && addr < 0x5800) return video.videoram[addr & 0x3FF];
    if (addr >= 0x5800 && addr < 0x6000) return video.objram[addr & 0xFF];
    uint16_t block = addr & 0xF800;
    // The lockout coil turns coins away before they reach the COIN1/COIN2 switches.
    if (block == 0x6000) return coin_lockout ? uint8_t(inputs.in0 & ~0x03) : inputs.in0;
    if (block == 0x6800) return inputs.in1;
    if (block == 0x7000) return inputs.in2;
    if (block == 0x7800) {
        watchdog_ = kWatchdogFrames;
        return 0xFF;
    }
    return 0xFF;
}

void Machine::mem_write(uint16_t addr, uint8_t v) {
    if (addr >= 0x4000 && addr < 0x4800) {
        ram[(addr - 0x4000) & 0x3FF] = v;
        return;
    }
    if (addr >= 0x5000 && addr < 0x5800) {
        video.videoram[addr & 0x3FF] = v;
        return;
    }
    if (addr >= 0x5800 && addr < 0x6000) {
        video.objram[addr & 0xFF] = v;
        return;
    }
    uint16_t block = addr & 0xF800;
    int off = addr & 7;
    if (block == 0x6000) {
        if (off >= 4) sound.lfo_freq_w(off - 4, (v & 1) != 0);
        // D0 low locks the coin chute out (MAME coin_lock_w).
        else if (off == 2) coin_lockout = (v & 1) == 0;
        else if (off == 3) coin_counter_w(0, (v & 1) != 0);
        return;
    }
    if (block == 0x6800) {
        sound.sound_w(off, (v & 1) != 0);
        return;
    }
    if (block == 0x7000) {
        switch (off) {
            case 1:
                // D0 holds the NMI flip-flop's CLEAR (MAME irq_enable_w).
                nmi_enable = (v & 1) != 0;
                if (!nmi_enable) cpu.set_nmi(false);
                return;
            case 4: video.set_stars_enable((v & 1) != 0); return;
            case 6: video.flip_x = (v & 1) != 0; return;
            case 7: video.flip_y = (v & 1) != 0; return;
            default: return;
        }
    }
    if (block == 0x7800) {
        sound.pitch_w(v);
    }
}

void Machine::watchdog_fire() {
    // The watchdog pulses /RESET; RAM keeps its contents, unlike a power cycle.
    const auto keep_ram = ram;
    const auto keep_v_videoram = video.videoram;
    const auto keep_v_objram = video.objram;
    reset();
    ram = keep_ram;
    video.videoram = keep_v_videoram;
    video.objram = keep_v_objram;
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
        if (t <= 0) t = 4;
        done += t;
        sound.advance(t);
        audio_acc_ += t;
        if (audio_hz > 0) {
            const double step = double(kCpuHz) / double(audio_hz);
            while (audio_acc_ >= step) {
                audio_acc_ -= step;
                audio.push_back(sound.mix());
            }
        }
        if (video.vblank_edge) {
            video.vblank_edge = false;
            frames++;
            watchdog_--;
            if (watchdog_ <= 0) {
                watchdog_fire();
                break;
            }
            if (nmi_enable) cpu.set_nmi(true);
        }
    }
    return done;
}

}  // namespace galaxian
