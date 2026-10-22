#include "machine.h"

#include <algorithm>
#include <cmath>

namespace galaga {

Machine::Machine() : main(make_bus(0)), sub(make_bus(1)), sound(make_bus(2)) {
    mcu51_.read_k = [this] {
        // K3 is 06XX pin 1, the R/W line (CPU PCB sheet 7-9).
        return uint8_t((io06_rw() ? 8 : 0) | (mcu51_.o_output & 7));
    };
    mcu51_.read_r = [this](int n) { return r_nibble(n); };
    mcu51_.write_o = [this](uint8_t v, uint8_t) { mcu51_.o_output = v; };
    // P2/P3 drive the coin meters, active low.
    mcu51_.write_p = [this](uint8_t v) {
        coin_counter_w(1, !(v & 0x04));
        coin_counter_w(0, !(v & 0x08));
    };
    // K and R0 read the latched 06XX command; O nibbles and R1 drive the three DACs.
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
    io_nmi_skip_ = 0;
    io_clk_ = false;
    mcu51_mode_ = 0;
    mcu51_args_ = 0;
    mcu51_read_i_ = 0;
    mcu51_started_ = false;
    mcu51_coins_[0] = mcu51_coins_[1] = 0;
    prev_in1_ = 0;
    mcu_next_ = main.cycles;
    io_tick_ = (main.cycles / 64 + 1) * 64;
    mcu51_wait_ = mcu54_wait_ = 0;
    mcu54_args_ = 0;
    mcu54_cmd_ = 0;
    for (uint8_t& d : dac54_) d = 0;
    net_settled_ = false;
    net_sum_ = 0;
    net_n_ = 0;
    net_clock_ = 0;
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
    // Undriven, the data bus floats to $FF on its 2.2K pull-ups (RM19, CPU PCB sheet 7-9).
    if (!(io_ctrl_ & 0x10) || !(io_ctrl_ & 0x01)) return 0xFF;
    // Only the 51XX drives the device bus; the 54XX's pins on it are K and R0 inputs (sheet 7-9).
    return mcu51_hle ? mcu51_hle_read() : mcu51_.o_output;
}

void Machine::io06_write(uint8_t v) {
    // In read mode the device owns the device bus, so a host write reaches nothing (Unsourced).
    if (io_ctrl_ & 0x10) return;
    if (io_ctrl_ & 0x01) {
        if (mcu51_hle) mcu51_command(v);
        else mcu51_.o_output = v;
    }
    if (io_ctrl_ & 0x08) {
        if (mcu54_hle) mcu54_command(v);
        else mcu54_cmd_ = v;
    }
}

void Machine::mcu54_command(uint8_t v) {
    if (mcu54_args_ > 0) {
        mcu54_args_--;
        return;
    }
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

void Machine::io06_ctrl(uint8_t v) {
    io_ctrl_ = v;
    // main $0299 takes 3 bytes of 51xx.bin's 4-byte $24F ring, so a read's first select has no NMI.
    io_nmi_skip_ = (v & 0x10) ? 1 : 0;
}

void Machine::io06_tick() {
    const int div = io_ctrl_ >> 5;
    const uint64_t n = io_tick_ / 64;
    // Divider d taps bit d-1 of a free-running count of 64H (Unsourced).
    const bool tap = div != 0 && ((n >> (div - 1)) & 1);
    const bool tap_was = div != 0 && (((n - 1) >> (div - 1)) & 1);
    if (io_clk_ && !tap) {
        io_clk_ = false;
        mcu_select(0);
        return;
    }
    // A select starts only as the tap rises, never part-way through a high phase.
    if (io_clk_ || !tap || tap_was) return;
    io_clk_ = true;
    mcu_select(uint8_t(io_ctrl_ & 0x0F));
    if (io_nmi_skip_ > 0) {
        io_nmi_skip_--;
        return;
    }
    main.set_nmi(true);
    main.set_nmi(false);
}

void Machine::mcu_select(uint8_t chips) {
    // 06XX pin 13 is the 51XX /IRQ, pin 10 the 54XX /IRQ (CPU PCB sheet 7-9).
    if (!mcu51_hle) mcu51_.set_irq(chips & 0x01);
    if (!mcu54_hle) mcu54_.set_irq(chips & 0x08);
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
        // Clearing a mask also clears its held vblank IRQ (coinop.org GalagaMap: $6820/$6821 clear IRQ1/IRQ2).
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
                mcu51_wait_ = mcu54_wait_ = 0;
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

void Machine::service_mcu() {
    const uint64_t now = main.cycles;
    while (io_tick_ <= now || mcu_next_ <= now) {
        // 06XX pin 17 is 64H (CPU PCB sheet 7-9): a rising edge every 64 T-states.
        if (io_tick_ <= mcu_next_) {
            io06_tick();
            io_tick_ += 64;
            continue;
        }
        // MB8843/44 instruction cycle is 1.536 MHz / 6, one per 12 Z80 T-states.
        mcu_next_ += 12;
        if (!mcu51_hle && !sub_reset && --mcu51_wait_ <= 0) mcu51_wait_ = mcu51_.step();
        if (!mcu54_hle && !sub_reset && --mcu54_wait_ <= 0) mcu54_wait_ = mcu54_.step();
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

// Midway CPU PCB A084-91404-C508 sound section, op-amps at 5P (LM324).
constexpr double kVcc = 5.0;
constexpr double kNetHz = 3072000.0 / 32.0;
// R38 3.3K / R39 2.2K hold the + inputs.
constexpr double kBias = kVcc * 2.2e3 / (3.3e3 + 2.2e3);
// LM324 output swing tops out about 1.5 V under V+ (National LM324 datasheet).
constexpr double kOpAmpTop = kVcc - 1.5;
constexpr double kSummerFeedback = 3.3e3;  // R20

constexpr double parallel(double a, double b) { return a * b / (a + b); }

// 54XX O ladders, bit 0 first: R46-R43 (O0-O3) and R32-R29 (O4-O7).
constexpr double kLadderOhms[4] = {47e3, 22e3, 10e3, 4.7e3};
constexpr double ladder_ohms() {
    double g = 0;
    for (double r : kLadderOhms) g += 1.0 / r;
    return 1.0 / g;
}

// O pins taken to swing the full 5 V rail (Unsourced; MB8840 datasheet guarantees VOH >= 2.4 V).
double ladder_volts(uint8_t nibble) {
    double g = 0;
    for (int b = 0; b < 4; b++)
        if (nibble & (1 << b)) g += 1.0 / kLadderOhms[b];
    return kVcc * g * ladder_ohms();
}

struct Mfb {
    double r_in, r_ground, r_feedback, c_out, c_inv, r_mix;
};

// R42/R41/R40/C27/C26 into R37, and R33/R34/R35/C29/C28 into R36. R21-R28 and C30-C31 are not fitted.
constexpr Mfb kMfb[2] = {
    {150e3, 22e3, 470e3, 0.01e-6, 0.01e-6, 10e3},
    {47e3, 10e3, 150e3, 0.01e-6, 0.01e-6, 33e3},
};

void discretize(double a[2][2], const double bvec[2], double dt, double out_m[2][2], double out_n[2]) {
    double l[2][2], r[2][2];
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
            double eye = i == j ? 1.0 : 0.0;
            l[i][j] = eye - a[i][j] * dt / 2;
            r[i][j] = eye + a[i][j] * dt / 2;
        }
    double det = l[0][0] * l[1][1] - l[0][1] * l[1][0];
    double inv[2][2] = {{l[1][1] / det, -l[0][1] / det}, {-l[1][0] / det, l[0][0] / det}};
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) out_m[i][j] = inv[i][0] * r[0][j] + inv[i][1] * r[1][j];
        out_n[i] = (inv[i][0] * bvec[0] + inv[i][1] * bvec[1]) * dt / 2;
    }
}

// States: x0 across the output capacitor, x1 across the capacitor into the - input, both
// measured from rest with the DAC at 0. The output sits at bias plus x1 - x0.
void mfb_matrices(const Mfb& f, double a[2][2], double bvec[2]) {
    double g1 = 1.0 / (ladder_ohms() + f.r_in), g2 = 1.0 / f.r_ground, g3 = 1.0 / f.r_feedback;
    a[0][0] = -g3 / f.c_out;
    a[0][1] = (g3 - g1 - g2) / f.c_out;
    a[1][0] = g3 / f.c_inv;
    a[1][1] = -g3 / f.c_inv;
    bvec[0] = g1 / f.c_out;
    bvec[1] = 0;
}

// Wave ladder R15-R18 through the 4066 volume legs R10-R13 at full volume, then R19 10K.
constexpr double kWsgSourceOhms =
    1.0 / (1 / 470.0 + 1 / 1e3 + 1 / 2.2e3 + 1 / 4.7e3) + 1.0 / (1 / 10e3 + 1 / 22e3 + 1 / 47e3 + 1 / 100e3);
constexpr double kWsgSumOhms = 10e3;
constexpr double kWsgNodeFarads = 0.0022e-6;  // C43
// A wave sample of 15 against 8 at volume 15 is the chip's unit output.
constexpr double kWsgUnitVolts = kVcc * 8.0 / 15.0;

// SOUND through R51 10K onto VR1 1K, C18 0.1 uF into the MB3730, C19 0.01 uF to ground.
constexpr double kPotTap = 1e3 / (10e3 + 1e3);
constexpr double kPotSourceOhms = parallel(10e3, 1e3);
constexpr double kCouplingFarads = 0.1e-6;
constexpr double kAmpShuntFarads = 0.01e-6;
// MB3730 typical input resistance (Fujitsu 1988 Linear Products Data Book).
constexpr double kAmpInputOhms = 70e3;
// Host full scale: the summer's swing from bias down to ground, seen at the amp input.
constexpr double kHostFullScale = kBias * kPotTap;

struct MfbDiscrete {
    Linear2 d[2];
    MfbDiscrete() {
        for (int ch = 0; ch < 2; ch++) {
            double a[2][2], bvec[2];
            mfb_matrices(kMfb[ch], a, bvec);
            discretize(a, bvec, 1.0 / kNetHz, d[ch].m, d[ch].n);
        }
    }
};

const MfbDiscrete& mfb_discrete() {
    static const MfbDiscrete d;
    return d;
}

}  // namespace

void Linear2::step(double* x, double u_prev, double u) const {
    double x0 = m[0][0] * x[0] + m[0][1] * x[1] + n[0] * (u_prev + u);
    double x1 = m[1][0] * x[0] + m[1][1] * x[1] + n[1] * (u_prev + u);
    x[0] = x0;
    x[1] = x1;
}

void Machine::settle_sound_network() {
    // Power-on finds every capacitor charged to its DAC-off level, so the start is silent.
    for (Section& sec : mfb_) sec = {};
    amp_in_ = {};
    wsg_lp_ = 0;
    wsg_in_ = 0;
    net_settled_ = true;
}

void Machine::step_54xx_network() {
    if (!net_settled_) settle_sound_network();
    double i = 0;
    for (int ch = 0; ch < 2; ch++) {
        Section& s = mfb_[ch];
        double u = ladder_volts(dac54_[ch]);
        mfb_discrete().d[ch].step(s.x, s.u, u);
        s.u = u;
        double v = std::clamp(kBias + s.x[1] - s.x[0], 0.0, kOpAmpTop);
        i += (v - kBias) / kMfb[ch].r_mix;
    }
    net_sum_ += -kSummerFeedback * i;
    net_n_++;
}

float Machine::sound_output(float wsg) {
    if (!net_settled_) settle_sound_network();
    const double dt = 1.0 / audio_hz;
    if (amp_hz_ != audio_hz) {
        double c1 = kCouplingFarads, c2 = kAmpShuntFarads, gs = 1.0 / kPotSourceOhms;
        double a[2][2] = {{-gs / c1, -gs / c1}, {-gs / c2, -(gs + 1.0 / kAmpInputOhms) / c2}};
        double bvec[2] = {gs / c1, gs / c2};
        discretize(a, bvec, dt, amp_disc_.m, amp_disc_.n);
        amp_hz_ = audio_hz;
    }
    // C43 rounds the WSG node with the source and R19 in parallel.
    double tau = parallel(kWsgSourceOhms, kWsgSumOhms) * kWsgNodeFarads;
    double w = double(wsg) * kWsgUnitVolts;
    double alpha = dt / (2 * tau);
    wsg_lp_ = (wsg_lp_ * (1 - alpha) + alpha * (w + wsg_in_)) / (1 + alpha);
    wsg_in_ = w;
    double dev54 = net_n_ ? net_sum_ / net_n_ : 0.0;
    net_sum_ = 0;
    net_n_ = 0;
    double dev_wsg = -kSummerFeedback * wsg_lp_ / (kWsgSourceOhms + kWsgSumOhms);
    double sound = std::clamp(kBias + dev54 + dev_wsg, 0.0, kOpAmpTop);
    double u = (sound - kBias) * kPotTap;
    amp_disc_.step(amp_in_.x, amp_in_.u, u);
    amp_in_.u = u;
    return float(amp_in_.x[1] / kHostFullScale);
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
        service_mcu();
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
        net_clock_ += t;
        while (net_clock_ >= 32) {
            net_clock_ -= 32;
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
        for (size_t i = audio_before; i < audio.size(); i++) audio[i] = sound_output(audio[i]);
    }
    return done;
}

}  // namespace galaga
