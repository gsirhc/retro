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
    b.in = [](uint8_t) { return uint8_t(0xFF); };
    b.out = [](uint8_t, uint8_t) {};
    b.irq_data = [] { return uint8_t(0xFF); };
    return b;
}

void Machine::reset() {
    ram.fill(0);
    nmi_enable = false;
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
    if (block == 0x6000) return inputs.in0;
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
        return;
    }
    if (block == 0x6800) {
        sound.sound_w(off, (v & 1) != 0);
        return;
    }
    if (block == 0x7000) {
        switch (off) {
            case 1: nmi_enable = (v & 1) != 0; return;
            case 4: video.stars_enable = (v & 1) != 0; return;
            case 6: video.flip_x = (v & 1) != 0; return;
            case 7: video.flip_y = (v & 1) != 0; return;
            default: return;
        }
    }
    if (block == 0x7800) {
        sound.pitch_w(v);
    }
}

int Machine::run_cycles(int n) {
    int done = 0;
    while (done < n) {
        int t = cpu.step();
        if (t <= 0) t = 4;
        done += t;
        video.advance(t);
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
            frames++;
            watchdog_--;
            if (watchdog_ <= 0) {
                watchdog_reset = true;
                reset();
                break;
            }
            if (nmi_enable) cpu.nmi();
        }
    }
    return done;
}

}  // namespace galaxian
