#include "machine.h"

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
    // The INT flip-flop clears on the M1 + IORQ acknowledge (Stern 33A-105).
    b.irq_data = [this] {
        sound.set_int(false);
        return uint8_t(0xFF);
    };
    return b;
}

namespace {

// Returns the byte the chip drives onto DV, or -1 when it leaves the bus alone.
int ay_io(galaxian::Ay8910& ay, bool rd, bool data_line, bool addr_line, uint8_t v) {
    // 33A-105 3D (6A, 4A) and 3C (5A, 4A): LS27 strobes, LS32 BC2 = rd | BDIR, BC1 = rd | addr.
    bool read = rd && data_line;
    bool bdir = !rd && data_line;
    bool bc2 = read || bdir;
    bool bc1 = read || (!rd && addr_line);
    // GI AY-3-8910 data manual bus control table.
    switch ((bdir << 2) | (bc2 << 1) | int(bc1)) {
        case 0b011: return ay.read_data();
        case 0b110: ay.write_data(v); return -1;
        case 0b001:
        case 0b100:
        case 0b111: ay.write_addr(v); return -1;
        default: return -1;
    }
}

}  // namespace

uint8_t Machine::in2_with_protection() const {
    // 6J's XB7 through custom 10S at 2B to PC5 and PC7 (33A-105). The ROM never reads them.
    uint8_t xb7 = uint8_t(pal_out & 0x80);
    return uint8_t((inputs.in2 & ~0xA0) | (xb7 ? 0xA0 : 0));
}

void Machine::wire_ppi() {
    ppi0.in_a = [this] { return inputs.in0; };
    ppi0.in_b = [this] { return inputs.in1; };
    ppi0.in_c = [this] { return in2_with_protection(); };
    ppi1.out_a = [this](uint8_t v) { sound_latch = v; };
    ppi1.out_b = [this](uint8_t v) { on_sound_control(v); };
    ppi1.out_c = [this](uint8_t v) { pal6j_write(v); };
    ppi1.in_c = [this] { return pal_out; };
    ay1.port_a_r = [this] { return sound_latch; };
    ay1.port_b_r = [this] { return galaxian::scramble_port_b(sound.cycles); };
}

void Machine::pal6j_write(uint8_t v) {
    // Unsourced fit to every 6J check in s1.2d: the nibble two writes back, plus one, held at F.
    pal_history_ = ((pal_history_ << 4) | (v & 0x0F)) & 0x0FFF;
    uint8_t n = uint8_t(pal_history_ >> 8);
    pal_out = uint8_t((n == 0x0F ? 0x0F : n + 1) << 4);
}

uint8_t Machine::sound_in(uint8_t port) {
    int d1 = ay_io(ay1, true, port & 0x80, port & 0x40, 0);
    int d2 = ay_io(ay2, true, port & 0x20, port & 0x10, 0);
    if (d1 < 0 && d2 < 0) return 0xFF;
    if (d1 < 0) return uint8_t(d2);
    if (d2 < 0) return uint8_t(d1);
    return uint8_t(d1 & d2);
}

void Machine::sound_out(uint8_t port, uint8_t v) {
    ay_io(ay1, false, port & 0x80, port & 0x40, v);
    ay_io(ay2, false, port & 0x20, port & 0x10, v);
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
    pal_history_ = 0;
    pal_out = 0;
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
