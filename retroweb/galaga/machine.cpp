#include "machine.h"

#include <algorithm>
#include <cmath>

namespace galaga {

Machine::Machine() : main(make_bus(0)), sub(make_bus(1)), sound(make_bus(2)) {
    mcu51_.read_k = [this] {
        return uint8_t(((io_ctrl_ & 0x10) ? 8 : 0) | (mcu51_.o_output & 7));
    };
    mcu51_.read_r = [this](int n) { return r_nibble(n); };
    mcu51_.write_o = [this](uint8_t v, uint8_t) { mcu51_.o_output = v; };
    // P2/P3 drive the coin meters, active low (MAME galaga out()).
    mcu51_.write_p = [this](uint8_t v) {
        coin_counter_w(1, !(v & 0x04));
        coin_counter_w(0, !(v & 0x08));
    };
    // K and R0 read the latched 06XX command; O nibbles and R1 drive the three DACs (MAME namco54).
    mcu54_.read_k = [this] { return uint8_t(mcu54_cmd_ >> 4); };
    mcu54_.read_r = [this](int) { return uint8_t(mcu54_cmd_ & 0x0f); };
    mcu54_.write_o = [this](uint8_t v, uint8_t) {
        mcu54_.o_output = v;
        dac54_[0] = uint8_t(v & 0x0f);
        dac54_[1] = uint8_t(v >> 4);
    };
    mcu54_.write_r = [this](int n, uint8_t v) {
        if (n == 1) dac54_[2] = uint8_t(v & 0x0f);
    };
    video.ram1 = ram1.data();
    video.ram2 = ram2.data();
    video.ram3 = ram3.data();
    reset();
}

z80::Bus Machine::make_bus(int cpu) {
    z80::Bus b;
    b.read = [this, cpu](uint16_t a) { return mem_read(cpu, a); };
    b.write = [this, cpu](uint16_t a, uint8_t v) { mem_write(cpu, a, v); };
    b.in = [](uint16_t) { return uint8_t(0xFF); };
    b.out = [](uint16_t, uint8_t) {};
    b.irq_data = [] { return uint8_t(0xFF); };
    if (cpu == 0) b.tick = [this](int t) { video.advance(t); };
    return b;
}

void Machine::reset() {
    ram1.fill(0);
    ram2.fill(0);
    ram3.fill(0);
    irq1_enable = irq2_enable = false;
    nmi_disable = false;
    sub_reset = sound_reset = true;
    watchdog_ = kWatchdogFrames;
    frames = 0;
    credits = 0;
    coin_line_[0] = coin_line_[1] = false;
    audio.clear();
    audio_acc_ = 0;
    sub_credit_ = 0;
    sound_credit_ = 0;
    io_ctrl_ = 0;
    io_div_count_ = 0;
    io_stretch_ = false;
    io_phase_ = false;
    mcu51_mode_ = 0;
    mcu51_args_ = 0;
    mcu51_read_i_ = 0;
    mcu51_started_ = false;
    mcu51_coins_[0] = mcu51_coins_[1] = 0;
    prev_in1_ = 0;
    mcu_acc_ = 0;
    mcu54_args_ = 0;
    mcu54_cmd_ = 0;
    for (int ch = 0; ch < 3; ch++) {
        dac54_[ch] = 0;
        bp54_[ch] = {};
    }
    amp54_cap_ = 0;
    bp54_rest_ = false;
    disc_sum_ = 0;
    disc_n_ = 0;
    disc_clock_ = 0;
    noise_left_ = 0;
    noise_amp_ = 0;
    noise_vol_ = 15;
    noise_lfsr_ = 1;
    video.reset();
    wsg.reset();
    wsg.enabled = true;
    mcu51_.reset();
    mcu54_.reset();
    mcu51_.halted_reset = true;
    mcu54_.halted_reset = true;
    main.reset();
    sub.reset();
    sound.reset();
}

void Machine::load_roms(const RomSet& set) {
    rom_main = set.main;
    rom_sub = set.sub;
    rom_sound = set.sound;
    video.tile_rom = set.tiles;
    video.sprite_rom = set.sprites;
    video.palette = set.palette;
    video.char_lut = set.char_lut;
    video.sprite_lut = set.sprite_lut;
    wsg.wave_prom = set.wave;
    mcu51_hle = !set.has51;
    mcu54_hle = !set.has54;
    if (set.has51) mcu51_.rom = set.mcu51;
    if (set.has54) mcu54_.rom = set.mcu54;
}

void Machine::render(uint32_t* upright) const { video.render(upright); }

uint8_t Machine::r_nibble(int n) const {
    uint8_t raw0 = uint8_t(~inputs.in0);
    uint8_t raw1 = uint8_t(~inputs.in1);
    if (n == 0) return uint8_t(raw0 & 0x0f);
    if (n == 1) return uint8_t((raw0 >> 4) & 0x0f);
    if (n == 2) return uint8_t(raw1 & 0x0f);
    return uint8_t((raw1 >> 4) & 0x0f);
}

uint8_t Machine::mcu51_hle_read() {
    uint8_t r0 = r_nibble(0), r1 = r_nibble(1), r2 = r_nibble(2), r3 = r_nibble(3);
    uint8_t bytes[3];
    if (mcu51_mode_ == 2) {
        int c = credits;
        if (c > 99) c = 99;
        // Credit-mode nibbles: BCD credits, stick, buttons/coins (whiterocker.com 51XX disassembly).
        bytes[0] = uint8_t(((c / 10) << 4) | (c % 10));
        bytes[1] = uint8_t(r0 | (r1 << 4));
        bytes[2] = uint8_t(r2 | (r3 << 4));
    } else {
        bytes[0] = uint8_t(r0 | (r1 << 4));
        bytes[1] = uint8_t(r2 | (r3 << 4));
        bytes[2] = 0xFF;
    }
    uint8_t b = bytes[mcu51_read_i_ % 3];
    mcu51_read_i_++;
    return b;
}

void Machine::mcu51_command(uint8_t data) {
    if (mcu51_args_ > 0) {
        mcu51_coinage_[4 - mcu51_args_] = data;
        mcu51_args_--;
        return;
    }
    // 51XX command bytes: nop, coinage, credit mode, joystick remap, switch mode.
    if (data == 0x01) {
        mcu51_args_ = 4;
        return;
    }
    if (data == 0x02) {
        mcu51_mode_ = 2;
        mcu51_read_i_ = 0;
        mcu51_started_ = false;
        return;
    }
    if (data == 0x05) {
        mcu51_mode_ = 5;
        mcu51_read_i_ = 0;
        return;
    }
}

void Machine::note_coins() {
    if (mcu51_mode_ != 2 || !mcu51_hle) {
        prev_in1_ = inputs.in1;
        return;
    }
    uint8_t rose = uint8_t(inputs.in1 & ~prev_in1_);
    prev_in1_ = inputs.in1;
    // Command 01 sets coins per credit and credits per coin for each chute; 0 coins is free play.
    for (int chute = 0; chute < 2; chute++) {
        if (!(rose & (0x10 << chute))) continue;
        int need = mcu51_coinage_[chute * 2];
        int give = mcu51_coinage_[chute * 2 + 1];
        if (need == 0) continue;
        coin_counter_w(chute, true);
        coin_counter_w(chute, false);
        if (++mcu51_coins_[chute] >= need) {
            mcu51_coins_[chute] = 0;
            credits += give;
        }
    }
    if (rose & 0x40) credits++;
    if (mcu51_coinage_[0] == 0) credits = 100;
    if (credits > 99 && mcu51_coinage_[0] != 0) credits = 99;
    // Until a game starts, START spends one credit per player (genuine 51XX, observed).
    if (!mcu51_started_) {
        if ((rose & 0x04) && credits >= 1) {
            credits -= 1;
            mcu51_started_ = true;
        } else if ((rose & 0x08) && credits >= 2) {
            credits -= 2;
            mcu51_started_ = true;
        }
    }
}

void Machine::coin_counter_w(int n, bool on) {
    if (on && !coin_line_[n]) coin_counter[unsigned(n)]++;
    coin_line_[n] = on;
}

uint8_t Machine::io06_read() {
    if ((io_ctrl_ & 0x10) == 0) return 0;
    uint8_t result = 0xFF;
    if (io_ctrl_ & 0x01) {
        if (mcu51_hle) result = uint8_t(result & mcu51_hle_read());
        else result = uint8_t(result & mcu51_.o_output);
    }
    if (io_ctrl_ & 0x08) {
        if (!mcu54_hle) result = uint8_t(result & mcu54_cmd_);
    }
    return result;
}

void Machine::io06_write(uint8_t v) {
    if (io_ctrl_ & 0x10) return;
    if (io_ctrl_ & 0x01) {
        if (mcu51_hle) mcu51_command(v);
        else mcu51_.o_output = v;
    }
    if (io_ctrl_ & 0x08) {
        if (mcu54_hle) {
            if (mcu54_args_ > 0) {
                mcu54_args_--;
            } else {
                uint8_t hi = uint8_t(v >> 4);
                // 54XX commands: 1/2/5 play, 3/4/6 set parameters, 7 sets type-C volume.
                if (hi == 1 || hi == 2 || hi == 5) {
                    noise_amp_ = v & 0x0f;
                    if (noise_amp_ == 0) noise_amp_ = 8;
                    noise_left_ = 3072000 / 32 / 8;
                } else if (hi == 3 || hi == 4) {
                    mcu54_args_ = 4;
                } else if (hi == 6) {
                    mcu54_args_ = 5;
                } else if (hi == 7) {
                    noise_vol_ = v & 0x0f;
                }
            }
        } else {
            mcu54_cmd_ = v;
        }
    }
}

void Machine::io06_ctrl(uint8_t v) {
    io_ctrl_ = v;
    io_div_count_ = 0;
    io_phase_ = false;
    io_stretch_ = (v & 0x10) != 0;
    if ((v & 0xE0) == 0) {
        io_stretch_ = false;
        mcu_select(false);
    }
}

void Machine::mcu_select(bool level) {
    // 06XX /IO1 and /IO4 are the 51XX and 54XX external IRQ lines.
    if (!mcu51_hle) mcu51_.set_irq(level && (io_ctrl_ & 0x01));
    if (!mcu54_hle) mcu54_.set_irq(level && (io_ctrl_ & 0x08));
}

uint8_t Machine::mem_read(int cpu, uint16_t addr) {
    if (addr < 0x4000) {
        if (cpu == 1) return addr < rom_sub.size() ? rom_sub[addr] : 0xFF;
        if (cpu == 2) return addr < rom_sound.size() ? rom_sound[addr] : 0xFF;
        return rom_main[addr];
    }
    if (addr >= 0x6800 && addr < 0x6808) {
        int bit = addr & 7;
        int b0 = (inputs.dsw_b >> bit) & 1;
        int b1 = (inputs.dsw_a >> bit) & 1;
        return uint8_t(b0 | (b1 << 1));
    }
    if (addr >= 0x7000 && addr <= 0x70FF) return io06_read();
    if (addr == 0x7100) return io_ctrl_;
    if (addr >= 0x8000 && addr < 0x8800) return video.videoram[addr - 0x8000];
    if (addr >= 0x8800 && addr < 0x9000) return ram1[(addr - 0x8800) & 0x3FF];
    if (addr >= 0x9000 && addr < 0x9800) return ram2[(addr - 0x9000) & 0x3FF];
    if (addr >= 0x9800 && addr < 0xA000) return ram3[(addr - 0x9800) & 0x3FF];
    return 0xFF;
}

void Machine::mem_write(int cpu, uint16_t addr, uint8_t v) {
    (void)cpu;
    if (addr >= 0x6800 && addr < 0x6820) {
        wsg.write(addr - 0x6800, v);
        return;
    }
    if (addr >= 0x6820 && addr < 0x6828) {
        int bit = addr & 7;
        bool on = (v & 1) != 0;
        // Clearing a mask also clears its held vblank IRQ (MAME irq1_clear_w / irq2_clear_w).
        if (bit == 0) {
            irq1_enable = on;
            if (!on) main.set_int(false);
        } else if (bit == 1) {
            irq2_enable = on;
            if (!on) sub.set_int(false);
        } else if (bit == 2) {
            nmi_disable = on;
        } else if (bit == 3) {
            sub_reset = !on;
            sound_reset = !on;
            if (!on) {
                sub.reset();
                sound.reset();
                mcu51_.reset();
                mcu54_.reset();
                mcu51_.halted_reset = true;
                mcu54_.halted_reset = true;
            } else {
                if (!mcu51_hle) mcu51_.halted_reset = false;
                if (!mcu54_hle) mcu54_.halted_reset = false;
            }
        }
        return;
    }
    if (addr == 0x6830) {
        watchdog_ = kWatchdogFrames;
        return;
    }
    if (addr >= 0x7000 && addr <= 0x70FF) {
        io06_write(v);
        return;
    }
    if (addr == 0x7100) {
        io06_ctrl(v);
        return;
    }
    if (addr >= 0x8000 && addr < 0x8800) {
        video.videoram[addr - 0x8000] = v;
        return;
    }
    if (addr >= 0x8800 && addr < 0x9000) {
        ram1[(addr - 0x8800) & 0x3FF] = v;
        return;
    }
    if (addr >= 0x9000 && addr < 0x9800) {
        ram2[(addr - 0x9000) & 0x3FF] = v;
        return;
    }
    if (addr >= 0x9800 && addr < 0xA000) {
        ram3[(addr - 0x9800) & 0x3FF] = v;
        return;
    }
    if (addr >= 0xA000 && addr < 0xA008) {
        video.star_latch[addr & 7] = uint8_t(v & 1);
        if ((addr & 7) == 7) video.flip = (v & 1) != 0;
    }
}

int Machine::step_cpu(int cpu) {
    z80::Cpu* c = cpu == 0 ? &main : cpu == 1 ? &sub : &sound;
    return c->step();
}

void Machine::on_vblank() {
    frames++;
    if (irq1_enable) main.set_int(true);
    if (irq2_enable) sub.set_int(true);
    mcu51_.set_tc(false);
    mcu54_.set_tc(false);
    mcu51_.set_tc(true);
    mcu54_.set_tc(true);
    watchdog_--;
    if (watchdog_ <= 0) {
        watchdog_fire();
    }
}

void Machine::service_mcu(int z80_cycles) {
    // MB8843/44 instruction cycle is 1.536 MHz / 6, one per 12 Z80 T-states.
    mcu_acc_ += z80_cycles;
    while (mcu_acc_ >= 12) {
        mcu_acc_ -= 12;
        if (!mcu51_hle && !sub_reset) mcu51_.step();
        if (!mcu54_hle && !sub_reset) mcu54_.step();
    }
    int shift = (io_ctrl_ >> 5) & 7;
    if (shift != 0) {
        // NMI period is (64 << shift) Z80 cycles; the midpoint drops /IO for a new MCU interrupt.
        // The first read-mode edge skips the host NMI so the MCU drives the bus first.
        int half = (64 << shift) / 2;
        io_div_count_ += z80_cycles;
        while (io_div_count_ >= half) {
            io_div_count_ -= half;
            io_phase_ = !io_phase_;
            if (io_phase_) {
                mcu_select(false);
                continue;
            }
            bool skip_nmi = io_stretch_;
            io_stretch_ = false;
            mcu_select(true);
            if (!skip_nmi && !sub_reset) {
                main.set_nmi(true);
                main.set_nmi(false);
            }
        }
    }
}

void Machine::watchdog_fire() {
    // The watchdog pulses /RESET; RAM keeps its contents, unlike a power cycle.
    const auto keep_ram1 = ram1;
    const auto keep_ram2 = ram2;
    const auto keep_ram3 = ram3;
    const auto keep_v_videoram = video.videoram;
    reset();
    ram1 = keep_ram1;
    ram2 = keep_ram2;
    ram3 = keep_ram3;
    video.videoram = keep_v_videoram;
    watchdog_reset = true;
}

namespace {

constexpr double kNetDt = 32.0 / 3072000.0;
constexpr double kDacR = 1.0 / (1.0 / 47e3 + 1.0 / 22e3 + 1.0 / 10e3 + 1.0 / 4.7e3);
constexpr double kVref = 5.0 * 2.2e3 / (3.3e3 + 2.2e3);

struct BpCoef {
    double b0, a1, a2, r1, r2, r_total;
};

// MFB band-pass per channel: DAC impedance + series R, shunt R, feedback R, two caps (MAME galaga_chanlN_filt).
BpCoef bp_coef(double r_series, double r2, double rf, double c) {
    BpCoef o{};
    o.r1 = kDacR + r_series;
    o.r2 = r2;
    o.r_total = 1.0 / (1.0 / o.r1 + 1.0 / r2);
    const double pi = 3.14159265358979323846;
    double fc = 1.0 / (2 * pi * std::sqrt(o.r_total * rf * c * c));
    double d = (2 * c) / std::sqrt(rf / o.r_total * c * c);
    double gain = -rf / o.r_total * 0.5;
    double k = 1.0 / std::tan(pi * fc * kNetDt);
    double den = k * k + d * k + 1.0;
    o.b0 = gain * d * k / den;
    o.a1 = 2.0 * (1.0 - k * k) / den;
    o.a2 = (k * k - d * k + 1.0) / den;
    return o;
}

const BpCoef& coef(int ch) {
    static const BpCoef c[3] = {
        bp_coef(150e3, 22e3, 470e3, 0.01e-6),   // 54XX_0
        bp_coef(47e3, 10e3, 150e3, 0.01e-6),    // 54XX_1
        bp_coef(100e3, 22e3, 220e3, 0.001e-6),  // 54XX_2
    };
    return c[ch];
}

}  // namespace

void Machine::step_54xx_network() {
    static constexpr double kMixR[3] = {10e3, 33e3, 33e3};
    double i = 0;
    for (int ch = 0; ch < 3; ch++) {
        double vdac = 0;
        static constexpr double kLadder[4] = {47e3, 22e3, 10e3, 4.7e3};
        for (int b = 0; b < 4; b++)
            if (dac54_[ch] & (1 << b)) vdac += 4.0 / kLadder[b];
        vdac *= kDacR;
        const BpCoef& c = coef(ch);
        Bandpass54& f = bp54_[ch];
        double v = ((vdac - kVref) / c.r1 + (0.0 - kVref) / c.r2) * c.r_total;
        // Power-on: the filters have settled at their DAC-off input.
        if (!bp54_rest_) f.x1 = f.x2 = ((0.0 - kVref) / c.r1 + (0.0 - kVref) / c.r2) * c.r_total;
        double y = -c.a1 * f.y1 - c.a2 * f.y2 + c.b0 * v - c.b0 * f.x2 + kVref;
        f.x2 = f.x1;
        f.x1 = v;
        f.y2 = f.y1;
        y = std::clamp(y, 0.0, 5.0 - 1.5);
        f.y1 = y - kVref;
        i += (kVref - y) / kMixR[ch];
    }
    bp54_rest_ = true;
    // Inverting summer, R 3.3k feedback, then C 0.1 uF into the amp (MAME galaga_final_mixer, gain 40800).
    double v = i * 3.3e3;
    amp54_cap_ += (v - amp54_cap_) * (1.0 - std::exp(-kNetDt / (100e3 * 0.1e-6)));
    disc_sum_ += (v - amp54_cap_) * 40800.0 / 32768.0;
    disc_n_++;
}

int Machine::run_cycles(int n) {
    int done = 0;
    while (done < n && !watchdog_reset) {
        // 08XX gives each CPU its own bus slot, so sub and sound advance by main's T-states.
        int t = step_cpu(0);
        // Repay instruction overshoot so sub/sound stay locked to main.
        if (!sub_reset) {
            sub_credit_ += t;
            while (sub_credit_ > 0) {
                int st = step_cpu(1);
                if (st <= 0) st = 4;
                sub_credit_ -= st;
            }
        }
        if (!sound_reset) {
            sound_credit_ += t;
            while (sound_credit_ > 0) {
                int st = step_cpu(2);
                if (st <= 0) st = 4;
                sound_credit_ -= st;
            }
        }
        done += t;
        size_t audio_before = audio.size();
        wsg.advance(t, audio_hz, audio);
        note_coins();
        service_mcu(t);
        if (video.sound_nmi_edge) {
            video.sound_nmi_edge = false;
            if (!nmi_disable && !sound_reset) {
                sound.set_nmi(true);
                sound.set_nmi(false);
            }
        }
        if (video.vblank_edge) {
            video.vblank_edge = false;
            on_vblank();
        }
        if (watchdog_reset) break;
        disc_clock_ += t;
        while (disc_clock_ >= 32) {
            disc_clock_ -= 32;
            if (mcu54_hle) {
                // HLE: the noise burst drives 54XX_0, so it still goes through the real filter.
                uint8_t level = 0;
                if (noise_left_ > 0) {
                    noise_left_--;
                    noise_lfsr_ = (noise_lfsr_ >> 1) ^ ((noise_lfsr_ & 1) ? 0xA300u : 0);
                    if (noise_lfsr_ & 1) level = uint8_t(noise_amp_ * noise_vol_ / 15);
                }
                dac54_[0] = level;
            }
            step_54xx_network();
        }
        if (audio.size() > audio_before) {
            // MAME mono: WSG (sum / 1024) at 0.9*10/16 plus the 54XX network at 0.9. Ours sums the WSG to /360.
            float v = disc_n_ ? float(disc_sum_ / disc_n_) : 0.0f;
            disc_sum_ = 0;
            disc_n_ = 0;
            for (size_t i = audio_before; i < audio.size(); i++)
                audio[i] = audio[i] * float(360.0 / 1024.0 * 0.9 * 10.0 / 16.0) + v * 0.9f;
        }
    }
    return done;
}

}  // namespace galaga
