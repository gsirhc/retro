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

uint8_t Machine::tile_pixel(uint8_t code, int x, int y) const {
    int off = int(code) * 8 + (y & 7);
    if (off < 0 || off >= 0x800) return 0;
    uint8_t p0 = gfx[off];
    uint8_t p1 = gfx[0x800 + off];
    int bit = 7 - (x & 7);
    return uint8_t(((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1));
}

uint8_t Machine::sprite_pixel(uint8_t code, int x, int y) const {
    int off = int(code) * 16 + (y & 15);
    if (off < 0 || off >= 0x800) return 0;
    uint8_t p0 = gfx[off];
    uint8_t p1 = gfx[0x800 + off];
    int bit = 7 - (x & 7);
    return uint8_t(((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1));
}

uint32_t Machine::pen_rgb(int pen) const {
    // Playfield colours at $1404, motion objects at $140C (DP-182 memory map).
    // Sprite pens use $140C..$140F via the per-object colour nybbles.
    uint8_t data = 0;
    if (pen < 4) {
        data = paletteram[0x04 | (pen & 3)];
    } else {
        // Sprite pen 1/2/3 → palette slots 0x0C + index.
        int p = pen & 3;
        if (p == 0) return 0;
        data = paletteram[0x0C | ((p - 1) & 3)];
    }
    // Active low, $F is black (CENTST.MAC); bit 3 is the alternate blue or green (sheet 2B).
    int r = 0xff * ((~data >> 0) & 1);
    int g = 0xff * ((~data >> 1) & 1);
    int b = 0xff * ((~data >> 2) & 1);
    if (~data & 0x08) {
        if (b) b = 0xc0;
        else if (g) g = 0xc0;
    }
    return 0xFF000000u | (uint32_t(r)) | (uint32_t(g) << 8) | (uint32_t(b) << 16);
}

void Machine::render(uint32_t* rgba) const {
    // Build machine-space 256×240, then ROT270 into upright 240×256.
    std::array<uint32_t, kFbW * kFbH> fb{};
    fb.fill(pen_rgb(0));

    for (int ty = 0; ty < kVisRows; ty++) {
        for (int tx = 0; tx < 32; tx++) {
            uint8_t data = videoram[ty * 32 + tx];
            uint8_t code = uint8_t((data & 0x3F) + 0x40);
            // Bit 6 flips X, bit 7 flips Y; after ROT270 that is upright Y / X.
            bool flipx = (data & 0x40) != 0;
            bool flipy = (data & 0x80) != 0;
            for (int py = 0; py < 8; py++) {
                for (int px = 0; px < 8; px++) {
                    int sx = flipx ? 7 - px : px;
                    int sy = flipy ? 7 - py : py;
                    uint8_t pix = tile_pixel(code, sx, sy);
                    int x = tx * 8 + px;
                    int y = ty * 8 + py;
                    fb[y * kFbW + x] = pen_rgb(pix);
                }
            }
        }
    }

    // A motion-object pen whose colour pair is 0 is transparent.
    auto penmask = [](uint8_t color) -> uint8_t {
        uint8_t mask = 1;
        if (((color >> 0) & 3) == 0) mask |= 2;
        if (((color >> 2) & 3) == 0) mask |= 4;
        if (((color >> 4) & 3) == 0) mask |= 8;
        return mask;
    };

    for (int offs = 0; offs < 0x10; offs++) {
        uint8_t pic = spriteram[offs];
        int code = ((pic & 0x3E) >> 1) | ((pic & 0x01) << 6);
        int color = spriteram[offs + 0x30];
        bool flipx = (pic & 0x40) != 0;
        bool flipy = (pic & 0x80) != 0;
        int x = spriteram[offs + 0x20];
        int y = 240 - spriteram[offs + 0x10];
        uint8_t mask = penmask(uint8_t(color & 0x3F));

        // Motion objects stop 8 px short of the cocktail edge.
        int x0 = flipscreen ? 8 : 0;
        int x1 = flipscreen ? kFbW : kFbW - 8;

        for (int py = 0; py < 16; py++) {
            for (int px = 0; px < 8; px++) {
                int sx = flipx ? 7 - px : px;
                int sy = flipy ? 15 - py : py;
                uint8_t pix = sprite_pixel(uint8_t(code), sx, sy);
                if (pix == 0) continue;
                if (mask & (1u << pix)) continue;
                // Colour byte pairs pick the $140C slot for pens 1/2/3.
                int pair = (color >> ((pix - 1) * 2)) & 3;
                int dx = x + px;
                int dy = y + py;
                if (dx < x0 || dx >= x1 || dy < 0 || dy >= kFbH) continue;
                // Map sprite pair → palette RAM $140C+pair as pen.
                uint8_t data = paletteram[0x0C | (pair & 3)];
                int r = 0xff * ((~data >> 0) & 1);
                int g = 0xff * ((~data >> 1) & 1);
                int b = 0xff * ((~data >> 2) & 1);
                if (~data & 0x08) {
                    if (b) b = 0xc0;
                    else if (g) g = 0xc0;
                }
                fb[dy * kFbW + dx] =
                    0xFF000000u | uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16);
            }
        }
    }

    // ROT270 CCW: upright(ux,uy) = machine(mx,my) with ux=my, uy=(kFbW-1)-mx.
    for (int my = 0; my < kFbH; my++) {
        for (int mx = 0; mx < kFbW; mx++) {
            int ux = my;
            int uy = (kFbW - 1) - mx;
            rgba[uy * kUprightW + ux] = fb[my * kFbW + mx];
        }
    }
}

}  // namespace centipede
