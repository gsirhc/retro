#include "machine.h"

namespace frogger {

uint8_t swap_d0d1(uint8_t v) {
    return uint8_t((v & 0xFC) | ((v & 1) << 1) | ((v & 2) >> 1));
}

uint8_t sound_timer_port(uint64_t sound_cpu_cycles) {
    // Crystal 14.31818 MHz → ÷8 = sound Z80. The timer chain is
    // 16×16×2×8×5×2 = 40960 crystal clocks (sound T-states × 8).
    constexpr uint32_t kPeriod = 16u * 16u * 2u * 8u * 5u * 2u;  // 40960
    constexpr uint32_t kHalf = 16u * 16u * 2u * 8u * 5u;         // 20480
    uint32_t clocks = uint32_t((sound_cpu_cycles * 8) % kPeriod);
    uint8_t hibit = 0;
    if (clocks >= kHalf) {
        hibit = 1;
        clocks -= kHalf;
    }
    uint8_t v = uint8_t((hibit << 7) | (uint8_t((clocks >> 14) & 1) << 6) |
                        (uint8_t((clocks >> 13) & 1) << 5) |
                        (uint8_t((clocks >> 11) & 1) << 4) | 0x0E);
    // Frogger PCB swaps timer bits 3 and 5.
    return uint8_t((v & 0xD7) | ((v & 0x08) << 2) | ((v & 0x20) >> 2));
}

Machine::Machine() : main(make_main_bus()), sound(make_sound_bus()) {
    wire_ppi();
    reset();
}

z80::Bus Machine::make_main_bus() {
    z80::Bus b;
    b.read = [this](uint16_t a) { return mem_read(a); };
    b.write = [this](uint16_t a, uint8_t v) { mem_write(a, v); };
    b.in = [](uint8_t) { return uint8_t(0xFF); };
    b.out = [](uint8_t, uint8_t) {};
    b.irq_data = [] { return uint8_t(0xFF); };
    return b;
}

z80::Bus Machine::make_sound_bus() {
    z80::Bus b;
    b.read = [this](uint16_t a) { return sound_read(a); };
    b.write = [this](uint16_t a, uint8_t v) { sound_write(a, v); };
    b.in = [this](uint8_t port) { return sound_in(port); };
    b.out = [this](uint8_t port, uint8_t v) { sound_out(port, v); };
    b.irq_data = [] { return uint8_t(0xFF); };
    return b;
}

void Machine::wire_ppi() {
    ppi0.in_a = [this] { return inputs.in0; };
    ppi0.in_b = [this] { return inputs.in1; };
    ppi0.in_c = [this] { return inputs.in2; };
    ppi1.out_a = [this](uint8_t v) { sound_latch = v; };
    ppi1.out_b = [this](uint8_t v) { on_sound_control(v); };
    ay.port_a_r = [this] { return sound_latch; };
    ay.port_b_r = [this] { return sound_timer_port(sound.cycles); };
}

uint8_t Machine::sound_in(uint8_t port) {
    // Bit 6 selects AY data. Konami frogger_ay8910_r.
    if (port & 0x40) return ay.read_data();
    return 0xFF;
}

void Machine::sound_out(uint8_t port, uint8_t v) {
    // Bit 6 = data, else bit 7 = address. Konami frogger_ay8910_w.
    if (port & 0x40) ay.write_data(v);
    else if (port & 0x80) ay.write_addr(v);
}

void Machine::on_sound_control(uint8_t v) {
    // Falling edge of bit 3 raises the sound Z80 /INT. Bit 4 mutes the AY.
    // Computer Archaeology Frogger hardware notes; MAME frogger_sh_irqtrigger_w
    // is a cross-check of the edge, not a source.
    bool prev3 = (sound_control & 0x08) != 0;
    bool now3 = (v & 0x08) != 0;
    sound_control = v;
    ay.mute = (v & 0x10) != 0;
    if (prev3 && !now3) {
        // Hold /INT until the sound Z80 accepts it (it may be in DI).
        if (sound.interrupt() <= 0) sound_irq_ = true;
    }
}

void Machine::reset() {
    ram.fill(0);
    sound_ram.fill(0);
    nmi_enable = false;
    watchdog_ = kWatchdogFrames;
    frames = 0;
    audio.clear();
    sound_latch = 0;
    sound_control = 0;
    sound_credit_ = 0;
    sound_irq_ = false;
    video.reset();
    ay.reset();
    ppi0.reset();
    ppi1.reset();
    // Boot ROM writes the 8255 mode words; after reset both chips are inputs
    // until those writes land. PPI1 must be outputs for the latch to stick,
    // so the program's first control write is what enables it.
    main.reset();
    sound.reset();
}

void Machine::load_roms(const RomSet& set) {
    program = set.program;
    sound_rom = set.sound;
    // First sound ROM (608) and the second gfx ROM (606, mapped at $800):
    // D0↔D1 swapped on the PCB. Computer Archaeology Frogger hardware notes;
    // MAME decode_frogger_sound / decode_frogger_gfx is a cross-check.
    for (int i = 0; i < 0x800; i++)
        sound_rom[unsigned(i)] = swap_d0d1(set.sound[unsigned(i)]);
    video.gfx = set.gfx;
    for (int i = 0x800; i < 0x1000; i++)
        video.gfx[unsigned(i)] = swap_d0d1(set.gfx[unsigned(i)]);
    video.color_prom = set.color_prom;
}

uint8_t Machine::mem_read(uint16_t addr) {
    if (addr < 0x4000) return program[addr];
    if (addr >= 0x8000 && addr < 0x8800) return ram[addr - 0x8000];
    if (addr >= 0x8800 && addr < 0x9000) {
        watchdog_ = kWatchdogFrames;
        return 0xFF;
    }
    if (addr >= 0xA800 && addr < 0xB000) return video.videoram[addr & 0x3FF];
    if (addr >= 0xB000 && addr < 0xB800) return video.objram[addr & 0xFF];
    if (addr >= 0xE000) return ppi0.read((addr >> 1) & 3);
    if (addr >= 0xD000 && addr < 0xE000) return ppi1.read((addr >> 1) & 3);
    return 0xFF;
}

void Machine::mem_write(uint16_t addr, uint8_t v) {
    if (addr >= 0x8000 && addr < 0x8800) {
        ram[addr - 0x8000] = v;
        return;
    }
    if (addr >= 0xA800 && addr < 0xB000) {
        video.videoram[addr & 0x3FF] = v;
        return;
    }
    if (addr >= 0xB000 && addr < 0xC000) {
        // Standalone D0 latches at $B808 / $B80C / $B810. More specific
        // than the $B000–$B7FF objram mirror, matching the board decode.
        switch (addr & 0xFF1F) {
            case 0xB808: nmi_enable = (v & 1) != 0; return;
            case 0xB80C: video.flip_y = (v & 1) != 0; return;
            case 0xB810: video.flip_x = (v & 1) != 0; return;
            case 0xB818:
            case 0xB81C: return;  // coin counters, not modeled
            default: break;
        }
        if (addr < 0xB800) video.objram[addr & 0xFF] = v;
        return;
    }
    if (addr >= 0xE000) {
        ppi0.write((addr >> 1) & 3, v);
        return;
    }
    if (addr >= 0xD000 && addr < 0xE000) {
        ppi1.write((addr >> 1) & 3, v);
        return;
    }
}

uint8_t Machine::sound_read(uint16_t addr) {
    if (addr < 0x1800) return sound_rom[addr];
    // $4000–$43FF, mirrored through $5FFF (mask 0x1C00).
    if (addr >= 0x4000 && addr < 0x6000) return sound_ram[addr & 0x3FF];
    return 0xFF;
}

void Machine::sound_write(uint16_t addr, uint8_t v) {
    if (addr >= 0x4000 && addr < 0x6000) sound_ram[addr & 0x3FF] = v;
}

int Machine::run_cycles(int n) {
    int done = 0;
    while (done < n) {
        int t = main.step();
        done += t;
        video.advance(t);
        // Sound Z80 + AY share 1.789772 MHz. Accumulate fractional T-states
        // so a long main-CPU burst still pays the right sound cycles.
        sound_credit_ += t * kSoundHz;
        int sound_target = sound_credit_ / kCpuHz;
        sound_credit_ %= kCpuHz;
        int sound_done = 0;
        while (sound_done < sound_target) {
            if (sound_irq_) {
                int it = sound.interrupt();
                if (it > 0) {
                    sound_irq_ = false;
                    sound_done += it;
                    ay.advance(it, audio_hz, audio);
                    continue;
                }
            }
            int st = sound.step();
            if (st <= 0) st = 4;
            sound_done += st;
            ay.advance(st, audio_hz, audio);
        }
        if (video.vblank_edge) {
            frames++;
            watchdog_--;
            if (watchdog_ <= 0) {
                watchdog_reset = true;
                reset();
                break;
            }
            if (nmi_enable) main.nmi();
        }
    }
    return done;
}

}  // namespace frogger
