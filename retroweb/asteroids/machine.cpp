#include "machine.h"

#include <algorithm>
#include <cmath>

namespace asteroids {

namespace {

uint16_t mirror(uint16_t addr) {
    // A15 ignored — $8000–$FFFF aliases $0000–$7FFF.
    return addr & 0x7FFF;
}

// Panel reads return $80 or $7F (MAME asteroid_IN0_r / asteroid_IN1_r).
uint8_t bit7(bool v) { return v ? 0x80 : 0x7F; }

// DP-143 thump 555 (MAME asteroid_thump_dac1 + DISCRETE_555_CC). A higher
// nibble cuts the charge current, so the pitch falls.
constexpr float kThumpHz[16] = {
    81.6f, 83.9f, 85.8f, 86.9f, 87.5f, 87.3f, 86.6f, 85.5f,
    81.9f, 79.5f, 76.3f, 73.1f, 67.8f, 63.7f, 58.4f, 53.5f
};
constexpr float kThumpDuty[16] = {
    0.395f, 0.422f, 0.454f, 0.479f, 0.516f, 0.540f, 0.569f, 0.592f,
    0.642f, 0.664f, 0.692f, 0.714f, 0.748f, 0.770f, 0.797f, 0.819f
};

// Fire 555 duty in percent, past 100 the pulse is DC (MAME asteroid_a.cpp).
float fire_duty(float hz) { return 4500.0f / hz + 67.0f; }

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
    explosion = thump = 0;
    saucer = saucer_fire = saucer_sel = thrust = ship_fire = bonus = false;
    noise_lfsr_ = 1;
    thrust_phase_ = fire_phase_ = saucer_fire_phase_ = 0;
    saucer_phase_ = saucer_warble_ = 0;
    thump_phase_ = thump_lp_ = thrust_lp_ = thrust_bp_ = 0;
    explode_hold_ = explode_lp_ = 0;
    explode_pitch_div_ = 12;
    explode_vol_ = explode_clk_ = 0;
    fire_freq_ = 820.0f;
    saucer_fire_freq_ = 830.0f;
    fire_remain_ = saucer_fire_remain_ = 0;
    bonus_phase_ = 0;
    thump_en_ = false;
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
    // Self-test (IN0 bit 7) blocks the NMI (MAME asteroid.c).
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

    // Halt at $2002: 1 = DVG busy (6502disassembly.com Asteroids).
    if (addr >= 0x2000 && addr <= 0x2007) {
        int bit = addr & 7;
        bool v = false;
        switch (bit) {
            case 0: v = false; break;
            case 1: v = ((clk3k_phase_ / kCyclesPer3kHz) & 1) != 0; break;
            case 2: v = !dvg.halted(); break;
            default: v = (inputs.in0 & (1u << bit)) != 0; break;
        }
        return bit7(v);
    }
    if (addr >= 0x2400 && addr <= 0x2407) {
        int bit = addr & 7;
        return bit7((inputs.in1 & (1u << bit)) != 0);
    }
    if (addr >= 0x2800 && addr <= 0x2803) {
        int pair = addr & 3;
        return uint8_t((inputs.dsw1 >> (2 * pair)) & 0x03);
    }

    if (addr >= 0x4000 && addr < 0x4800) return vector_ram[addr - 0x4000];
    if (addr >= 0x5000 && addr < 0x5800) return vector_rom[addr - 0x5000];
    if (addr >= 0x6800 && addr < 0x8000) return program[addr - 0x6800];
    return 0;
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

    if (addr == 0x3000) { dvg_go(); return; }
    if (addr == 0x3200) {
        lamps = v;
        ramsel = (v & 0x04) != 0;  // bit 2 = RAMSEL / cocktail flip
        return;
    }
    if (addr == 0x3400) { kick_watchdog(); return; }
    if (addr == 0x3600) {
        // Bits 5:2 volume, 7:6 pitch divider (MAME asteroid_explode_w).
        explosion = (v >> 2) & 0x0F;
        explode_vol_ = explosion;
        switch (v & 0xC0) {
            case 0x00: explode_pitch_div_ = 12; break;
            case 0x40: explode_pitch_div_ = 6; break;
            case 0x80: explode_pitch_div_ = 3; break;
            default:   explode_pitch_div_ = 5; break;
        }
        return;
    }
    if (addr == 0x3A00) {
        // Bit 4 enables the 555 VCO, bits 3:0 set its control voltage.
        thump = v & 0x1F;
        thump_en_ = (thump & 0x10) != 0;
        return;
    }
    // LS259: address selects the latch, D7 is the data (MAME asteroid_sounds_w).
    if (addr >= 0x3C00 && addr <= 0x3C05) {
        bool on = (v & 0x80) != 0;
        switch (addr & 7) {
            case 0: saucer = on; break;
            case 1:
                if (on && !saucer_fire) {
                    saucer_fire_remain_ = int(audio_hz * 0.28f);
                    saucer_fire_freq_ = 830.0f;
                }
                saucer_fire = on;
                break;
            case 2: saucer_sel = on; break;
            case 3: thrust = on; break;
            case 4:
                if (on && !ship_fire) {
                    fire_remain_ = int(audio_hz * 0.28f);
                    fire_freq_ = 820.0f;
                }
                ship_fire = on;
                break;
            case 5:
                bonus = on;
                break;
        }
        return;
    }
    if (addr == 0x3E00) {
        noise_lfsr_ = 1;
        explosion = thump = 0;
        explode_vol_ = 0;
        thump_en_ = false;
        saucer = saucer_fire = thrust = ship_fire = bonus = false;
        return;
    }
}

void Machine::mix_audio(int cycles) {
    // Approximates the DP-143 discrete board; rates follow MAME asteroid_a.cpp.
    int samples = int((int64_t(cycles) * audio_hz) / kCpuHz);
    if (samples <= 0) return;
    const float dt = 1.0f / float(audio_hz);
    const int thump_n = thump & 0x0F;
    const float thump_hz = kThumpHz[thump_n];
    const float thump_duty = kThumpDuty[thump_n];
    const int fire_total = std::max(1, int(float(audio_hz) * 0.28f));
    audio.reserve(audio.size() + size_t(samples));
    for (int i = 0; i < samples; i++) {
        noise_lfsr_ = (noise_lfsr_ >> 1) ^ ((noise_lfsr_ & 1) ? 0xA001u : 0);
        float noise = (noise_lfsr_ & 1) ? 1.0f : -1.0f;

        float s = 0;

        if (thump_en_) {
            thump_phase_ += thump_hz * dt;
            if (thump_phase_ >= 1.0f) thump_phase_ -= 1.0f;
            float sq = thump_phase_ < thump_duty ? 1.0f : -1.0f;
            // Pole is a bit brighter than the 3.3k / 0.1uF after the 555 (MAME NODE_32).
            thump_lp_ += (sq - thump_lp_) * 0.2f;
            s += thump_lp_ * 0.22f;
        }

        if (thrust) {
            thrust_lp_ += (noise - thrust_lp_) * 0.08f;
            thrust_bp_ += (thrust_lp_ - thrust_bp_) * 0.04f;
            s += (thrust_lp_ - thrust_bp_) * 0.10f;
        }

        if (explode_vol_ > 0) {
            // Sample-and-hold noise at 12 kHz / divider.
            int period = std::max(1, int(float(audio_hz) / (12000.0f / float(explode_pitch_div_))));
            if (++explode_clk_ >= period) {
                explode_clk_ = 0;
                explode_hold_ = noise;
            }
            float e = explode_hold_ * (float(explode_vol_) / 15.0f);
            explode_lp_ += (e - explode_lp_) * 0.15f;
            s += explode_lp_ * 0.35f;
        }

        if (ship_fire && fire_remain_ > 0) {
            // 820 to 110 Hz in 0.28 s. RC discharge (tau 81 ms), duty hits 100% near 136 Hz.
            fire_freq_ = std::max(110.0f, fire_freq_ - (820.0f - 110.0f) * dt / 0.28f);
            fire_phase_ += fire_freq_ * dt;
            if (fire_phase_ >= 1.0f) fire_phase_ -= 1.0f;
            float age = 0.28f * (1.0f - float(fire_remain_) / float(fire_total));
            float env = 7.0f + 46.0f * std::exp(-age / 0.081f);
            float duty = fire_duty(fire_freq_);
            if (duty < 100.0f) {
                float amp = env / 53.0f * 0.22f;
                s += (fire_phase_ < duty / 100.0f ? amp : -amp);
            }
            fire_remain_--;
        } else if (!ship_fire) {
            fire_remain_ = 0;
        }

        if (saucer) {
            // Large saucer warbles at 5.75 Hz, 250 Hz lower (MAME NODE_41/42).
            const float warble_hz = saucer_sel ? 5.75f : 8.25f;
            saucer_warble_ += warble_hz * dt;
            if (saucer_warble_ >= 1.0f) saucer_warble_ -= 1.0f;
            float tri = saucer_warble_ < 0.5f
                ? (4.0f * saucer_warble_ - 1.0f)
                : (3.0f - 4.0f * saucer_warble_);
            float f = 1210.0f + 460.0f * tri - (saucer_sel ? 250.0f : 0.0f);
            saucer_phase_ += f * dt;
            if (saucer_phase_ >= 1.0f) saucer_phase_ -= 1.0f;
            float tone = saucer_phase_ < 0.5f
                ? (4.0f * saucer_phase_ - 1.0f)
                : (3.0f - 4.0f * saucer_phase_);
            s += tone * 0.10f;
        }

        if (saucer_fire && saucer_fire_remain_ > 0) {
            // 830 to 630 Hz in 0.28 s.
            saucer_fire_freq_ = std::max(630.0f,
                saucer_fire_freq_ - (830.0f - 630.0f) * dt / 0.28f);
            saucer_fire_phase_ += saucer_fire_freq_ * dt;
            if (saucer_fire_phase_ >= 1.0f) saucer_fire_phase_ -= 1.0f;
            float age = 0.28f * (1.0f - float(saucer_fire_remain_) / float(fire_total));
            float env = 7.0f + 42.5f * std::exp(-age / 0.30f);
            float duty = fire_duty(saucer_fire_freq_);
            if (duty < 100.0f) {
                float amp = env / 49.5f * 0.18f;
                s += (saucer_fire_phase_ < duty / 100.0f ? amp : -amp);
            }
            saucer_fire_remain_--;
        } else if (!saucer_fire) {
            saucer_fire_remain_ = 0;
        }

        if (bonus) {
            // 3 kHz square (MAME asteroid_a.cpp ASTEROID_LIFE_SND).
            bonus_phase_ += 3000.0f * dt;
            if (bonus_phase_ >= 1.0f) bonus_phase_ -= 1.0f;
            s += bonus_phase_ < 0.5f ? 0.12f : -0.12f;
        }

        audio.push_back(std::clamp(s, -1.0f, 1.0f));
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
