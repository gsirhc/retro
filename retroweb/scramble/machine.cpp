#include "machine.h"

#include <algorithm>

namespace scramble {

Machine::Machine() : main(make_main_bus()), sound(make_sound_bus()) {
    video.board = galaxian::Board::Scramble;
    konami.ay = {&ay1, &ay2};
    wire_ppi();
    reset();
}

z80::Bus Machine::make_main_bus() {
    z80::Bus b;
    b.read = [this](uint16_t a) { return mem_read(a); };
    b.write = [this](uint16_t a, uint8_t v) { mem_write(a, v); };
    b.in = [](uint16_t) { return uint8_t(0xFF); };
    b.out = [](uint16_t, uint8_t) {};
    b.irq_data = [] { return uint8_t(0xFF); };
    b.tick = [this](int t) { video.advance(t); };
    return b;
}

z80::Bus Machine::make_sound_bus() {
    z80::Bus b;
    b.read = [this](uint16_t a) { return sound_read(a); };
    b.write = [this](uint16_t a, uint8_t v) { sound_write(a, v); };
    b.in = [this](uint16_t port) { return sound_in(uint8_t(port)); };
    b.out = [this](uint16_t port, uint8_t v) { sound_out(uint8_t(port), v); };
    // The INT flip-flop clears on the acknowledge (MAME konami_sound_control_w HOLD_LINE).
    b.irq_data = [this] {
        sound.set_int(false);
        return uint8_t(0xFF);
    };
    return b;
}

uint8_t Machine::in2_with_protection() const {
    // IN2 bits 5/7 are PAL 6J "alt" bits from the high bit of the PAL's nibble (equations unpublished).
    uint8_t alt = uint8_t((protection_result >> 7) & 1);
    return uint8_t((inputs.in2 & ~0xA0) | (alt ? 0xA0 : 0));
}

void Machine::wire_ppi() {
    ppi0.in_a = [this] { return inputs.in0; };
    ppi0.in_b = [this] { return inputs.in1; };
    ppi0.in_c = [this] { return in2_with_protection(); };
    ppi1.out_a = [this](uint8_t v) { sound_latch = v; };
    ppi1.out_b = [this](uint8_t v) { on_sound_control(v); };
    ppi1.out_c = [this](uint8_t v) { pal6j_write(v); };
    ppi1.in_c = [this] { return protection_result; };
    ay1.port_a_r = [this] { return sound_latch; };
    ay1.port_b_r = [this] { return galaxian::konami_sound_timer(sound.cycles); };
}

void Machine::pal6j_write(uint8_t v) {
    // PAL 6J: nibble in/out, equations unpublished. Ops per MAME theend_protection_w.
    protection_state_ = (protection_state_ << 4) | (v & 0x0F);
    uint8_t num1 = uint8_t((protection_state_ >> 8) & 0x0F);
    uint8_t num2 = uint8_t((protection_state_ >> 4) & 0x0F);
    uint8_t op = uint8_t(protection_state_ & 0x0F);
    switch (op) {
        case 0x6:
            protection_result ^= 0x80;
            break;
        case 0x9:
            protection_result = uint8_t(std::min(int(num1) + 1, 0x0F) << 4);
            break;
        case 0xB:
            protection_result = uint8_t(std::max(int(num2) - int(num1), 0) << 4);
            break;
        case 0xA:
            protection_result = 0;
            break;
        case 0xF:
            protection_result = uint8_t(std::max(int(num1) - int(num2), 0) << 4);
            break;
        default:
            break;
    }
}

uint8_t Machine::sound_in(uint8_t port) {
    // konami_ay8910_r: bit 5 = AY2 data, bit 7 = AY1 data.
    uint8_t r = 0xFF;
    if (port & 0x20) r &= ay2.read_data();
    if (port & 0x80) r &= ay1.read_data();
    return r;
}

void Machine::sound_out(uint8_t port, uint8_t v) {
    // konami_ay8910_w: bits 4–5 = AY2 (addr/data), bits 6–7 = AY1 (addr/data).
    if (port & 0x10) ay2.write_addr(v);
    else if (port & 0x20) ay2.write_data(v);
    if (port & 0x40) ay1.write_addr(v);
    else if (port & 0x80) ay1.write_data(v);
}

void Machine::on_sound_control(uint8_t v) {
    bool prev3 = (sound_control & 0x08) != 0;
    bool now3 = (v & 0x08) != 0;
    sound_control = v;
    konami.mute = (v & 0x10) != 0;
    if (prev3 && !now3) sound.set_int(true);
}

void Machine::reset() {
    coin_line_[0] = coin_line_[1] = false;
    ram.fill(0);
    sound_ram.fill(0);
    nmi_enable = false;
    watchdog_ = kWatchdogFrames;
    frames = 0;
    audio.clear();
    sound_latch = 0;
    sound_control = 0;
    sound_credit_ = 0;
    protection_state_ = 0;
    protection_result = 0;
    video.reset();
    video.board = galaxian::Board::Scramble;
    ay1.reset();
    ay2.reset();
    konami.reset();
    ppi0.reset();
    ppi1.reset();
    main.reset();
    sound.reset();
}

void Machine::load_roms(const RomSet& set) {
    program = set.program;
    sound_rom = set.sound;
    video.gfx = set.gfx;
    video.color_prom = set.color_prom;
}

uint8_t Machine::mem_read(uint16_t addr) {
    if (addr < 0x4000) return program[addr];
    if (addr < 0x4800) return ram[addr - 0x4000];
    if (addr < 0x5000) return video.videoram[addr & 0x3FF];
    if (addr < 0x5800) return video.objram[addr & 0xFF];
    if ((addr & 0xF800) == 0x7000) {
        watchdog_ = kWatchdogFrames;
        return 0xFF;
    }
    if (addr >= 0x8000) {
        uint8_t r = 0xFF;
        if (addr & 0x0100) r &= ppi0.read(addr & 3);
        if (addr & 0x0200) r &= ppi1.read(addr & 3);
        return r;
    }
    return 0xFF;
}

void Machine::mem_write(uint16_t addr, uint8_t v) {
    if (addr >= 0x4000 && addr < 0x4800) {
        ram[addr - 0x4000] = v;
        return;
    }
    if (addr >= 0x4800 && addr < 0x5000) {
        video.videoram[addr & 0x3FF] = v;
        return;
    }
    if (addr >= 0x5000 && addr < 0x5800) {
        video.objram[addr & 0xFF] = v;
        return;
    }
    if ((addr & 0xF800) == 0x6800) {
        switch (addr & 7) {
            case 1:
                nmi_enable = (v & 1) != 0;
                if (!nmi_enable) main.set_nmi(false);
                return;
            case 2: coin_counter_w(0, (v & 1) != 0); return;
            case 3: video.background_enable = (v & 1) != 0; return;
            case 4: video.set_stars_enable((v & 1) != 0); return;
            case 5: return;  // POUT2
            case 6: video.flip_x = (v & 1) != 0; return;
            case 7: video.flip_y = (v & 1) != 0; return;
            default: return;
        }
    }
    if (addr >= 0x8000) {
        if (addr & 0x0100) ppi0.write(addr & 3, v);
        if (addr & 0x0200) ppi1.write(addr & 3, v);
    }
}

uint8_t Machine::sound_read(uint16_t addr) {
    if (addr < 0x1800) return sound_rom[addr];
    // $8000–$83FF, mirrored when A15=1 and A12=0 (mask 0x6c00).
    if ((addr & 0x9000) == 0x8000) return sound_ram[addr & 0x3FF];
    return 0xFF;
}

void Machine::sound_write(uint16_t addr, uint8_t v) {
    if ((addr & 0x9000) == 0x8000) sound_ram[addr & 0x3FF] = v;
    else if ((addr & 0x9000) == 0x9000) konami.filter_w(uint16_t(addr & 0x0FFF));
}

void Machine::watchdog_fire() {
    // The watchdog pulses /RESET; RAM keeps its contents, unlike a power cycle.
    const auto keep_ram = ram;
    const auto keep_sound_ram = sound_ram;
    const auto keep_v_videoram = video.videoram;
    const auto keep_v_objram = video.objram;
    reset();
    ram = keep_ram;
    sound_ram = keep_sound_ram;
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
        int t = main.step();
        done += t;
        // Credit in sound-cycle units so an overshooting sound instruction is repaid later.
        sound_credit_ += t * kSoundHz;
        while (sound_credit_ >= kCpuHz) {
            int st = sound.step();
            konami.advance(st, audio_hz, audio);
            sound_credit_ -= st * kCpuHz;
        }
        if (video.vblank_edge) {
            video.vblank_edge = false;
            frames++;
            watchdog_--;
            if (watchdog_ <= 0) {
                watchdog_fire();
                break;
            }
            if (nmi_enable) main.set_nmi(true);
        }
    }
    return done;
}

}  // namespace scramble
