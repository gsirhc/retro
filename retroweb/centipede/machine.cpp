#include "machine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace centipede {

namespace {

uint16_t mirror(uint16_t addr) {
    // H3 and J2 decode A13-A10; A14/A15 go nowhere (DP-182 sheet 1B).
    return addr & 0x3FFF;
}

}  // namespace

Machine::Machine() : cpu(make_bus()) {
    // AUD into J10, held at +5 V with R69 3.3K || C53 0.01 uF feedback (DP-182 sheet 2B).
    pokey.set_output_transimpedance(3.3e3, 0.01e-6, 5.0, kOpAmpHigh);
    reset();
}

mos6502::Bus Machine::make_bus() {
    mos6502::Bus b;
    b.read = [this](uint16_t a) { return mem_read(a); };
    b.write = [this](uint16_t a, uint8_t v) { mem_write(a, v); };
    return b;
}

void Machine::reset() {
    cpu.rebind_bus(make_bus());
    cpu.reset();
    pokey.reset();
    ram.fill(0);
    videoram.fill(0);
    spriteram.fill(0);
    paletteram.fill(0);
    // EAROM contents survive a soft reset; power-on leaves zeros until restore.
    frames = 0;
    flipscreen = false;
    outlatch = 0;
    line_ = 0;
    line_cycles_ = 0;
    watchdog_frames_ = kWatchdogFrames;
    vblank_ = false;
    irq_line_ = false;
    cpu.irq_line = false;
    earom_data_ = 0;
    earom_addr_ = 0;
    earom_cs1_ = false;
    earom_c1_ = true;
    earom_c2_ = false;
    earom_clk_ = false;
    audio.clear();
    hp_ = 5.0;
}

void Machine::load_roms(const RomSet& set) {
    program = set.program;
    gfx = set.gfx;
    reset();
}

void Machine::set_earom_bytes(const uint8_t* p, size_t n) {
    if (!p) return;
    size_t m = n < earom.size() ? n : earom.size();
    std::memcpy(earom.data(), p, m);
}

void Machine::kick_watchdog() { watchdog_frames_ = kWatchdogFrames; }

void Machine::earom_update() {
    // C1=0 C2=0 writes, C1=0 C2=1 erases (GI ER2055 data sheet); erased reads $FF.
    if (earom_c1_) return;
    uint8_t& cell = earom[earom_addr_ & 0x3F];
    if (earom_c2_) cell = 0xFF;
    else cell &= earom_data_;
}

void Machine::earom_control(uint8_t v) {
    // E4 LS175: CK=DB0, C1=/DB1, C2=DB2, CS1=DB3; CS2 grounded (DP-182 sheet 2B).
    bool cs1 = (v & 0x08) != 0;
    bool c1 = (v & 0x02) == 0;
    bool c2 = (v & 0x04) != 0;
    bool clk = (v & 0x01) != 0;
    bool ctrl_changed = cs1 != earom_cs1_ || c1 != earom_c1_ || c2 != earom_c2_;
    earom_cs1_ = cs1;
    earom_c1_ = c1;
    earom_c2_ = c2;
    if (cs1 && ctrl_changed) earom_update();
    bool fell = earom_clk_ && !clk;
    earom_clk_ = clk;
    if (cs1 && fell) {
        if (c1) earom_data_ = earom[earom_addr_ & 0x3F];
        earom_update();
    }
}

uint8_t Machine::mem_read(uint16_t addr) const {
    addr = mirror(addr);
    if (addr < 0x0400) return ram[addr];
    if (addr < 0x07C0) {
        playfield_touch_ = true;
        return videoram[addr - 0x0400];
    }
    if (addr < 0x0800) return spriteram[addr - 0x07C0];
    if (addr == 0x0800) return inputs.dsw1;
    if (addr == 0x0801) return inputs.dsw2;

    if (addr == 0x0C00) {
        // Trackball X, cocktail, self-test, VBLANK (DP-182 memory map).
        uint8_t v = uint8_t((inputs.track_x & 0x0F) | (inputs.track_x & 0x80));
        v |= uint8_t(inputs.in0_switches & 0x30);
        if (vblank_) v |= 0x40;
        return v;
    }
    if (addr == 0x0C01) return inputs.in1;
    if (addr == 0x0C02) {
        return uint8_t((inputs.track_y & 0x0F) | (inputs.track_y & 0x80));
    }
    if (addr == 0x0C03) return inputs.in3;

    if (addr >= 0x1000 && addr <= 0x100F)
        return pokey.read(uint8_t(addr & 0x0F));

    if (addr >= 0x1700 && addr <= 0x173F)
        return earom_data_;

    if (addr >= 0x2000 && addr <= 0x3FFF)
        return program[addr - 0x2000];
    return 0xFF;
}

void Machine::mem_write(uint16_t addr, uint8_t v) {
    addr = mirror(addr);
    if (addr < 0x0400) {
        ram[addr] = v;
        return;
    }
    if (addr < 0x07C0) {
        playfield_touch_ = true;
        videoram[addr - 0x0400] = v;
        return;
    }
    if (addr < 0x0800) {
        spriteram[addr - 0x07C0] = v;
        return;
    }
    if (addr >= 0x1000 && addr <= 0x100F) {
        pokey.write(uint8_t(addr & 0x0F), v);
        return;
    }
    if (addr >= 0x1400 && addr <= 0x140F) {
        paletteram[addr - 0x1400] = v;
        return;
    }
    if (addr >= 0x1600 && addr <= 0x163F) {
        earom_addr_ = uint8_t(addr - 0x1600);
        earom_data_ = v;
        return;
    }
    if (addr == 0x1680) {
        earom_control(v);
        return;
    }
    if (addr == 0x1800) {
        irq_line_ = false;
        cpu.irq_line = false;
        return;
    }
    if (addr >= 0x1C00 && addr <= 0x1C07) {
        // LS259: one address per line, value = D7.
        int bit = addr & 7;
        uint8_t mask = uint8_t(1u << bit);
        if (v & 0x80) outlatch |= mask;
        else outlatch = uint8_t(outlatch & ~mask);
        if (bit == 7) flipscreen = (outlatch & 0x80) != 0;
        return;
    }
    if (addr == 0x2000) {
        kick_watchdog();
        return;
    }
}

void Machine::service_video(int cycles) {
    line_cycles_ += cycles;
    while (line_cycles_ >= kCyclesPerLine) {
        line_cycles_ -= kCyclesPerLine;
        line_++;
        // D4 LS74 latches 32V on each 16V edge into /IRQ (DP-182 sheet 1B).
        if (line_ & 16) {
            bool assert_irq = ((line_ - 1) & 32) != 0;
            irq_line_ = assert_irq;
            cpu.irq_line = assert_irq;
        }
        vblank_ = (line_ >= kFbH);
        if (line_ >= kLines) {
            line_ = 0;
            frames++;
            vblank_ = false;
            if (--watchdog_frames_ <= 0) {
                watchdog_reset = true;
                reset();
                watchdog_reset = true;
                return;
            }
        }
    }
}

int Machine::run_cycles(int n) {
    int done = 0;
    while (done < n) {
        playfield_touch_ = false;
        int c = cpu.step();
        // L4 LS107 stretches PHI0 while PF is selected (DP-182 sheet 1B).
        if (playfield_touch_) c *= 2;
        if (c < 1) c = 1;
        service_video(c);
        size_t from = audio.size();
        pokey.mix(c, audio_hz, audio);
        condition_audio(from);
        done += c;
        if (watchdog_reset && frames == 0 && line_ == 0) {
            // reset() cleared the sticky flag path; keep sticky true.
            watchdog_reset = true;
        }
    }
    return done;
}

// C65 0.22 uF into R81 100K couples J10's output to the inverting stage.
void Machine::condition_audio(size_t from) {
    double a = 1 - std::exp(-1.0 / (0.22e-6 * 100e3 * audio_hz));
    for (size_t i = from; i < audio.size(); i++) {
        hp_ += (audio[i] - hp_) * a;
        audio[i] = float(std::clamp(-(audio[i] - hp_) / kAudioScale, -1.0, 1.0));
    }
}

// --- video ---

namespace {

// Output stage on DP-182 sheet 2B: A9 7407 into R66 220 up and R65 330 down,
// Q5 draws the line down through R102 56 and CR1 to the 7407's low level.
constexpr double kLineVcc = 5.0;
constexpr double kLineUp = 220.0;
constexpr double kLineDown = 330.0;
constexpr double kLineSeries = 56.0;
constexpr double kDiodeDrop = 0.8;     // 1N914 at 10 mA
constexpr double kSatDrop = 0.15;      // 2N3904 VCE(sat)
constexpr double kBufferLow = 0.3;     // 7407 VOL

constexpr double alt_level() {
    double voc = kLineVcc * kLineDown / (kLineUp + kLineDown);
    double rth = kLineUp * kLineDown / (kLineUp + kLineDown);
    double drops = kDiodeDrop + kSatDrop + kBufferLow;
    double v = (voc + rth * drops / kLineSeries) / (1.0 + rth / kLineSeries);
    return (v - kBufferLow) / (voc - kBufferLow);
}

constexpr int kAltLevel = int(alt_level() * 255.0 + 0.5);

// 82S25 outputs the complement of the stored bit (Signetics 1975 Bipolar
// Memories); A8 passes it on, so a stored 0 lights the gun (DP-182 sheet 2B).
uint32_t colour_ram_rgb(uint8_t cell) {
    bool red = !(cell & 0x01);
    bool green = !(cell & 0x02);
    bool blue = !(cell & 0x04);
    bool alt = !(cell & 0x08);
    int r = red ? 255 : 0;
    int g = green ? 255 : 0;
    int b = blue ? 255 : 0;
    // Q5 sinks the blue line while blue is lit, Q4 the green line while blue is dark.
    if (alt) {
        if (blue) b = kAltLevel;
        else if (green) g = kAltLevel;
    }
    return 0xFF000000u | uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16);
}

// F7/HJ7 address: A10 and A9 select object or playfield by 256H, A8-A4 are
// PIC5-PIC1, A3-A0 are MGA3-MGA0 (DP-182 sheet 2A).
size_t picture_rom_addr(bool object, uint8_t pic, int mga) {
    if (object) return size_t(((pic & 0x01) << 10) | ((pic & 0x3E) << 3) | (mga & 0x0F));
    return size_t(0x200 | ((pic & 0x3F) << 3) | (mga & 0x07));
}

// F8/H8 mux and the LS166 shift out D7 first, or D0 first when PIC6 is set.
int shifted_pen(const uint8_t* plane0, const uint8_t* plane1, size_t addr, int x, bool flip) {
    int bit = flip ? (x & 7) : 7 - (x & 7);
    return ((plane0[addr] >> bit) & 1) | (((plane1[addr] >> bit) & 1) << 1);
}

}  // namespace

void Machine::render(uint32_t* rgba) const {
    std::array<uint32_t, kFbW * kFbH> fb{};
    const uint8_t* plane0 = gfx.data();
    const uint8_t* plane1 = gfx.data() + 0x800;

    for (int v = 0; v < kFbH; v++) {
        // Line buffer A6/B6: 2-bit indirect colour per pixel, 256 deep.
        std::array<uint8_t, kFbW> mo{};
        for (int n = 0; n < 16; n++) {
            uint8_t pic = spriteram[n];
            int sum = (v + spriteram[0x10 + n]) & 0xFF;
            // A7 matches when the top nibble of V + vertical position is $F.
            if ((sum & 0xF0) != 0xF0) continue;
            int mga = (sum & 0x0F) ^ ((pic & 0x80) ? 0x0F : 0);
            size_t addr = picture_rom_addr(true, pic, mga);
            uint8_t colour = spriteram[0x30 + n];
            int x0 = spriteram[0x20 + n];
            for (int px = 0; px < 8; px++) {
                int pen = shifted_pen(plane0, plane1, addr, px, (pic & 0x40) != 0);
                // C7 passes pen 0 through as the buffer's own contents.
                if (pen == 0) continue;
                mo[size_t((x0 + px) & 0xFF)] = uint8_t((colour >> ((pen - 1) * 2)) & 3);
            }
        }

        int row = v >> 3;
        for (int x = 0; x < kFbW; x++) {
            uint8_t grey = mo[size_t(x)];
            bool in_window = flipscreen ? x >= 8 : x < kFbW - 8;
            if (grey != 0 && in_window) {
                fb[size_t(v * kFbW + x)] = colour_ram_rgb(paletteram[0x0C | grey]);
                continue;
            }
            uint8_t pic = videoram[size_t(row * 32 + (x >> 3))];
            int mga = (v & 7) ^ ((pic & 0x80) ? 0x07 : 0);
            size_t addr = picture_rom_addr(false, pic, mga);
            int pen = shifted_pen(plane0, plane1, addr, x, (pic & 0x40) != 0);
            fb[size_t(v * kFbW + x)] = colour_ram_rgb(paletteram[0x04 | pen]);
        }
    }

    // ROT270 CCW: upright(ux,uy) = machine(mx,my) with ux=my, uy=(kFbW-1)-mx.
    for (int my = 0; my < kFbH; my++) {
        for (int mx = 0; mx < kFbW; mx++) {
            int ux = my;
            int uy = (kFbW - 1) - mx;
            rgba[uy * kUprightW + ux] = fb[size_t(my * kFbW + mx)];
        }
    }
}

}  // namespace centipede
