#include "machine.h"

namespace galaga {

Machine::Machine() : main(make_bus(0)), sub(make_bus(1)), sound(make_bus(2)) {
    mcu51_.read_k = [this] {
        return uint8_t(((io_ctrl_ & 0x10) ? 8 : 0) | (mcu51_.o_output & 7));
    };
    mcu51_.read_r = [this](int n) { return r_nibble(n); };
    mcu51_.write_o = [this](uint8_t v, uint8_t) { mcu51_.o_output = v; };
    mcu54_.read_k = [this] { return uint8_t(mcu54_.o_output >> 4); };
    mcu54_.read_r = [this](int) { return uint8_t(mcu54_.o_output & 0x0f); };
    mcu54_.write_o = [this](uint8_t v, uint8_t) { mcu54_.o_output = v; };
    reset();
}

z80::Bus Machine::make_bus(int cpu) {
    z80::Bus b;
    b.read = [this, cpu](uint16_t a) { return mem_read(cpu, a); };
    b.write = [this, cpu](uint16_t a, uint8_t v) { mem_write(cpu, a, v); };
    b.in = [](uint8_t) { return uint8_t(0xFF); };
    b.out = [](uint8_t, uint8_t) {};
    b.irq_data = [] { return uint8_t(0xFF); };
    return b;
}

void Machine::reset() {
    ram1.fill(0);
    ram2.fill(0);
    ram3.fill(0);
    irq1_enable = irq2_enable = false;
    nmi_disable = false;
    sub_reset = sound_reset = true;
    irq1_line_ = irq2_line_ = false;
    watchdog_ = kWatchdogFrames;
    frames = 0;
    credits = 0;
    audio.clear();
    audio_acc_ = 0;
    io_ctrl_ = 0;
    io_div_count_ = 0;
    io_stretch_ = false;
    io_phase_ = false;
    mcu51_mode_ = 0;
    mcu51_args_ = 0;
    mcu51_read_i_ = 0;
    prev_in1_ = 0;
    mcu_acc_ = 0;
    mcu54_args_ = 0;
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

void Machine::render(uint32_t* upright) const {
    video.render(upright, ram1.data(), ram2.data(), ram3.data());
}

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
        // Six nibbles from the 51XX mask program's credit mode: BCD credits,
        // then the stick nibbles, then buttons/coins. whiterocker.com's
        // 51XX disassembly is the source for the nibble order.
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
    // Command bytes published with the 51XX (nop, coinage, credit mode,
    // joystick remap, switch mode). namco51.cpp's header is a cross-check.
    if (data == 0x01) {
        mcu51_args_ = 4;
        return;
    }
    if (data == 0x02) {
        mcu51_mode_ = 2;
        mcu51_read_i_ = 0;
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
    if (rose & 0x10) credits++;
    if (rose & 0x20) credits++;
    if (rose & 0x40) credits++;
}

uint8_t Machine::io06_read() {
    if ((io_ctrl_ & 0x10) == 0) return 0;
    uint8_t result = 0xFF;
    if (io_ctrl_ & 0x01) {
        if (mcu51_hle) result = uint8_t(result & mcu51_hle_read());
        else result = uint8_t(result & mcu51_.o_output);
    }
    if (io_ctrl_ & 0x08) {
        if (!mcu54_hle) result = uint8_t(result & mcu54_.o_output);
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
                // 54XX command nibbles from the chip's published command
                // list: 1/2/5 play, 3/4/6 set parameters, 7 sets type-C volume.
                if (hi == 1 || hi == 2 || hi == 5) {
                    noise_amp_ = v & 0x0f;
                    if (noise_amp_ == 0) noise_amp_ = 8;
                    noise_left_ = audio_hz > 0 ? audio_hz / 8 : 6000;
                } else if (hi == 3 || hi == 4) {
                    mcu54_args_ = 4;
                } else if (hi == 6) {
                    mcu54_args_ = 5;
                } else if (hi == 7) {
                    noise_vol_ = v & 0x0f;
                }
            }
        } else {
            mcu54_.o_output = v;
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
        if (bit == 0) {
            irq1_enable = on;
            if (!on) irq1_line_ = false;
        } else if (bit == 1) {
            irq2_enable = on;
            if (!on) irq2_line_ = false;
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
    bool* line = cpu == 0 ? &irq1_line_ : cpu == 1 ? &irq2_line_ : nullptr;
    if (line && *line) {
        int t = c->interrupt();
        if (t > 0) return t;
    }
    return c->step();
}

void Machine::on_vblank() {
    frames++;
    if (irq1_enable) irq1_line_ = true;
    if (irq2_enable) irq2_line_ = true;
    bool tc = true;
    mcu51_.set_tc(false);
    mcu54_.set_tc(false);
    (void)tc;
    mcu51_.set_tc(true);
    mcu54_.set_tc(true);
    watchdog_--;
    if (watchdog_ <= 0) {
        watchdog_reset = true;
        reset();
    }
}

void Machine::service_mcu(int z80_cycles) {
    // MB8843/44 instruction cycle is the 1.536 MHz input divided by 6,
    // which is one instruction per 12 Z80 T-states.
    mcu_acc_ += z80_cycles;
    while (mcu_acc_ >= 12) {
        mcu_acc_ -= 12;
        if (!mcu51_hle && !sub_reset) mcu51_.step();
        if (!mcu54_hle && !sub_reset) mcu54_.step();
    }
    int shift = (io_ctrl_ >> 5) & 7;
    if (shift != 0) {
        // NMI spacing stays one full period, (64 << shift) Z80 cycles, with
        // the first falling edge one period after the control write. The
        // midpoint drops /IO so the next falling edge is a new MCU interrupt.
        // The first read-mode falling edge skips the host NMI so the MCU can
        // drive the data bus before the CPU samples it.
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
            if (!skip_nmi && !sub_reset) main.nmi();
        }
    }
}

int Machine::run_cycles(int n) {
    int done = 0;
    while (done < n && !watchdog_reset) {
        // 08XX gives each CPU its own slot in the 18.432 MHz cycle, so none
        // of them wait on the others. Advance sub and sound by the same
        // T-state count as the main instruction.
        int t = step_cpu(0);
        if (!sub_reset) {
            int acc = 0;
            while (acc < t) acc += step_cpu(1);
        }
        if (!sound_reset) {
            int acc = 0;
            while (acc < t) acc += step_cpu(2);
        }
        done += t;
        size_t audio_before = audio.size();
        video.advance(t);
        wsg.advance(t, audio_hz, audio);
        note_coins();
        service_mcu(t);
        if (video.sound_nmi_edge && !nmi_disable && !sound_reset) sound.nmi();
        if (video.vblank_edge) on_vblank();
        if (watchdog_reset) break;
        float noise = 0;
        if (mcu54_hle && noise_left_ > 0 && noise_amp_ > 0) {
            noise_lfsr_ = (noise_lfsr_ >> 1) ^ ((noise_lfsr_ & 1) ? 0xA300u : 0);
            float s = (noise_lfsr_ & 1) ? 1.f : -1.f;
            noise = s * (float(noise_amp_) / 15.f) * (float(noise_vol_) / 15.f) * 0.35f;
            if (!audio.empty()) noise_left_--;
        } else if (!mcu54_hle) {
            noise = (float(mcu54_.o_output) / 255.f) - 0.5f;
        }
        for (size_t i = audio_before; i < audio.size(); i++) audio[i] += noise;
    }
    return done;
}

}  // namespace galaga
