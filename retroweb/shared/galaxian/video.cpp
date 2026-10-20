#include "galaxian/video.h"

#include <vector>

namespace galaxian {

namespace {

// h counts from H = 176, where HSYNC clocks V (Galaxian 6F).
constexpr int kActiveH0 = 80;   // H = 256
constexpr int kActiveH1 = 336;  // H wraps 511 -> 128
constexpr int kFramePixels = 256 * 256;
// Object slots end at H = 128 + 16k + 15; slots 0-2 fall before HSYNC and see the old V.
constexpr int kSlotH[8] = {351, 367, 383, 15, 31, 47, 63, 79};

std::vector<uint32_t> make_lfsr(int tap) {
    std::vector<uint32_t> s(static_cast<size_t>(kStarPeriod));
    uint32_t r = 0;
    for (size_t i = 0; i < s.size(); i++) {
        s[i] = r;
        const uint32_t in = (~(r >> 16) ^ (r >> (tap - 1))) & 1;
        r = ((r << 1) | in) & 0x1FFFF;
    }
    return s;
}

// Bit k-1 holds stage k; stage 1 takes NOT stage 17 XOR the tap.
const std::vector<uint32_t>& lfsr(Board b) {
    static const std::vector<uint32_t> galaxian = make_lfsr(5);   // 3B on A1 QE
    static const std::vector<uint32_t> scramble = make_lfsr(14);  // 5M on 1M QF
    return b == Board::Scramble ? scramble : galaxian;
}

int wrap(long long n) {
    const long long m = n % kStarPeriod;
    return int(m < 0 ? m + kStarPeriod : m);
}

struct Gun {
    double hi = 0;
    double lo = 0;
    void add(double ohms, bool high) { (high ? hi : lo) += 1.0 / ohms; }
    uint32_t level() const { return gun_level(gun_volts(hi, lo)); }
};

uint32_t pack(const Gun& r, const Gun& g, const Gun& b) {
    return (r.level() << 16) | (g.level() << 8) | b.level();
}

void add_prom(uint8_t e, Gun& r, Gun& g, Gun& b) {
    r.add(1000, e & 0x01);
    r.add(470, e & 0x02);
    r.add(220, e & 0x04);
    g.add(1000, e & 0x08);
    g.add(470, e & 0x10);
    g.add(220, e & 0x20);
    b.add(470, e & 0x40);
    b.add(220, e & 0x80);
}

// 74366/LS366 inverting drivers: a star bit of 0 drives its resistor high.
void add_star(uint8_t c, Gun& r, Gun& g, Gun& b) {
    r.add(150, !(c & 0x01));
    r.add(100, !(c & 0x02));
    g.add(150, !(c & 0x04));
    g.add(100, !(c & 0x08));
    b.add(150, !(c & 0x10));
    b.add(100, !(c & 0x20));
}

}  // namespace

void Video::reset() {
    h = 0;
    v = 0;
    vblank = true;
    vblank_edge = false;
    flip_x = false;
    flip_y = false;
    background_enable = false;
    stars_blink_state = 0;
    stars_enable_ = false;
    noise_ = true;
    star_drop_ = true;
    star_base_ = 0;
    star_origin = 0;
    blink_acc_ = 0;
    frame_.fill(0);
    line_buf_.fill(0);
    shell_ = {};
    missile_ = {};
    rgb_prom_ok_.fill(false);
    rgb_dark_ok_.fill(false);
}

void Video::set_stars_enable(bool on) {
    if (on && !stars_enable_) {
        long long next;
        if (v >= 256) next = kFramePixels;
        else if (h < kActiveH0) next = 256LL * v;
        else if (h < kActiveH1) next = 256LL * v + (h - kActiveH0);
        else next = 256LL * (v + 1);
        star_base_ = wrap(-2 * next);
        star_origin = 0;
    }
    stars_enable_ = on;
}

uint8_t Video::tile_pixel(uint8_t code, int x, int y) const {
    const unsigned a = unsigned(code) * 8 + unsigned(y & 7);
    const int bit = 7 - (x & 7);
    return uint8_t((((gfx[a] >> bit) & 1) << 1) | ((gfx[0x800 + a] >> bit) & 1));
}

uint8_t Video::sprite_pixel(uint8_t code, int x, int y) const {
    // 32 bytes per sprite. The x+8 half is the next 8, rows 8-15 follow that.
    const int yy = y & 15;
    unsigned a = unsigned(code & 0x3F) * 32 + unsigned(yy < 8 ? yy : 16 + (yy - 8));
    if (x & 8) a += 8;
    const int bit = 7 - (x & 7);
    return uint8_t((((gfx[a] >> bit) & 1) << 1) | ((gfx[0x800 + a] >> bit) & 1));
}

uint32_t Video::prom_rgb(uint8_t pen) const {
    Gun r, g, b;
    add_prom(color_prom[pen & 31], r, g, b);
    return pack(r, g, b);
}

uint32_t Video::star_rgb(uint8_t color6) {
    Gun r, g, b;
    add_star(color6 & 0x3F, r, g, b);
    return pack(r, g, b);
}

// Frogger 834-0068: HPLA0-2 reach CD2, CD0, CD1 through LS175 43 and LS377 61.
uint8_t Video::remap_color(uint8_t attr) {
    return uint8_t(((attr >> 1) & 3) | ((attr & 1) << 2));
}

// Frogger 834-0068 LS283 76/77 add HPLA0-3 to 16V-128V and HPLA4-7 to 1V-8V.
uint8_t Video::scroll_of(uint8_t value) const {
    if (board == Board::Frogger) return uint8_t((value >> 4) | (value << 4));
    return value;
}

uint8_t Video::color_group(uint8_t attr) const {
    return board == Board::Frogger ? remap_color(attr) : uint8_t(attr & 7);
}

void Video::check_palette() {
    if (prom_seen_ != color_prom || board_seen_ != board) {
        prom_seen_ = color_prom;
        board_seen_ = board;
        rgb_prom_ok_.fill(false);
        rgb_dark_ok_.fill(false);
    }
}

uint32_t Video::mix(int prom_index, int star, int bullet, bool backdrop) {
    size_t key;
    bool* ok;
    uint32_t* slot;
    if (prom_index >= 0) {
        key = size_t(prom_index * 3 + bullet) * 2;
        ok = &rgb_prom_ok_[key];
        slot = &rgb_prom_[key];
    } else {
        key = size_t(((star + 1) * 3 + bullet) * 2 + (backdrop ? 1 : 0));
        ok = &rgb_dark_ok_[key];
        slot = &rgb_dark_[key];
    }
    if (*ok) return *slot;
    Gun r, g, b;
    if (prom_index >= 0) add_prom(color_prom[unsigned(prom_index)], r, g, b);
    if (star >= 0) add_star(uint8_t(star), r, g, b);
    if (backdrop) b.add(board == Board::Scramble ? 390 : 470, true);
    if (bullet) {
        r.add(100, true);
        g.add(100, true);
        // Galaxian 9D drives blue from SHELL. Frogger shells are white the same way.
        if (board != Board::Scramble) b.add(100, bullet == 1);
    }
    *ok = true;
    *slot = pack(r, g, b);
    return *slot;
}

int Video::star_at(int x, int y) const {
    if (!stars_enable_ || board == Board::Frogger) return -1;
    const long long n = 256LL * y + x;
    uint32_t s;
    int xh, vy;
    if (board == Board::Galaxian) {
        s = lfsr(board)[size_t(wrap(star_base_ + 2 * n))];
        xh = flip_x ? 255 - x : x;
        vy = y;
    } else {
        // Scramble clears the LFSR through VBLANK (6P), so every frame starts at line 16.
        s = lfsr(board)[size_t(wrap(2 * (n - 256LL * kVisY0)))];
        xh = flip_x ? 255 - x : x;
        vy = flip_y ? 255 - y : y;
    }
    if ((s & 0xFF) != 0xFF || (s & 0x10000)) return -1;
    if ((((vy & 1) ^ (xh >> 3)) & 1) == 0) return -1;
    if (board == Board::Scramble) {
        bool sel = true;
        switch (stars_blink_state & 3) {
            case 0: sel = (s >> 8) & 1; break;   // LS151 D0 = TF (1M QA)
            case 1: sel = (s >> 10) & 1; break;  // D1 = TG (1M QC)
            case 2: sel = (y >> 1) & 1; break;   // D2 = 2V
            default: break;
        }
        if (!sel) return -1;
    }
    return int((s >> 8) & 0x3F);
}

bool Video::bullet_hit(Bullet& b, int x, int lo, int hi) {
    if (b.load < 0 || b.done) return false;
    const int c = (b.load + x + 1) & 0xFF;
    const bool in = c >= lo && c <= hi;
    // 5P (Galaxian) / 10L (Scramble) stop the counter firing twice on one line.
    if (!in && b.in_window) b.done = true;
    b.in_window = in;
    return in;
}

void Video::object_slot(int slot) {
    const int line = v < 256 ? v : v - 8;
    const int vb = flip_y ? 255 - line : line;

    const uint8_t* o = &objram[unsigned(0x40 + slot * 4)];
    const int sum = (scroll_of(o[0]) + vb) & 0xFF;
    if ((sum & 0xF0) == 0xF0) {
        int row = sum & 0x0F;
        if (o[1] & 0x80) row ^= 15;
        const bool fx = (o[1] & 0x40) != 0;
        const uint8_t code = o[1] & 0x3F;
        const uint8_t group = color_group(o[2]);
        for (int p = 0; p < 16; p++) {
            const uint8_t pen = sprite_pixel(code, fx ? 15 - p : p, row);
            if (!pen) continue;
            uint8_t& cell = line_buf_[unsigned((o[3] + p) & 0xFF)];
            if (!cell) cell = uint8_t(pen | (group << 2));
        }
    }

    const uint8_t* b = &objram[unsigned(0x60 + slot * 4)];
    if (((scroll_of(b[1]) + vb) & 0xFF) != 0xFF) return;
    Bullet* dst = nullptr;
    if (board == Board::Scramble) {
        if (slot != 7) dst = &shell_;  // 10M leaves the slot-7 decode unconnected
    } else {
        dst = slot == 7 ? &missile_ : &shell_;
    }
    if (dst) *dst = Bullet{b[3], false, false};
}

void Video::paint(int x) {
    const int y = v;
    const int yf = flip_y ? 255 - y : y;
    const int xf = flip_x ? 255 - x : x;
    const int col = xf >> 3;
    const int sum = (scroll_of(objram[unsigned(col * 2)]) + yf) & 0xFF;
    const uint8_t code = videoram[unsigned((sum >> 3) * 32 + col)];
    int pen = tile_pixel(code, xf & 7, sum & 7);
    int group = color_group(objram[unsigned(col * 2 + 1)]);

    int r = (x - 1) & 0xFF;
    if (flip_x) r ^= 0xFF;
    const uint8_t sp = line_buf_[unsigned(r)];
    line_buf_[unsigned(r)] = 0;
    if (sp && (r & 0xF0)) {
        pen = sp & 3;
        group = sp >> 2;
    }

    int bullet = 0;
    if (board != Board::Scramble) {
        if (bullet_hit(missile_, x, 0xFC, 0xFF)) bullet = 2;
        if (bullet_hit(shell_, x, 0xFC, 0xFF)) bullet = 1;
    } else {
        if (bullet_hit(shell_, x, 0xFA, 0xFB)) bullet = 1;
    }

    if (y < kVisY0 || y >= kVBlankLine) return;
    uint32_t rgb;
    if (pen) {
        rgb = mix(group * 4 + pen, -1, bullet, false);
    } else {
        bool backdrop = false;
        if (board == Board::Frogger) backdrop = (x < kRiverSplit) != flip_x;
        else if (board == Board::Scramble) backdrop = background_enable;
        rgb = mix(-1, star_at(x, y), bullet, backdrop);
    }
    frame_[unsigned((y - kVisY0) * kVisW + x)] = rgb;
}

void Video::line_start() {
    v = (v + 1) % kVTotal;
    if (v == kVBlankLine) {
        vblank = true;
        vblank_edge = true;
    } else if (v == kVisY0) {
        vblank = false;
    }
    if (v == 0) {
        if (stars_enable_) {
            star_origin = uint32_t(wrap(star_base_ + 2LL * kFramePixels));
            star_base_ = int(star_origin);
        }
        star_drop_ = true;
    }
    // 7474 at 2D clocks on 2V (A082-91352-A000 p.22).
    if ((v & 3) == 2) {
        if (!stars_enable_ || board != Board::Galaxian) {
            noise_ = true;
        } else {
            const long long n = v < 256 ? 256LL * v : kFramePixels;
            noise_ = !((lfsr(board)[size_t(wrap(star_base_ + 2 * n))] >> 16) & 1);
        }
    }
    check_palette();
}

void Video::advance(int cpu_cycles) {
    if (board == Board::Scramble) {
        blink_acc_ += cpu_cycles;
        while (blink_acc_ >= kStarBlinkPeriod) {
            blink_acc_ -= kStarBlinkPeriod;
            stars_blink_state = uint8_t((stars_blink_state + 1) & 3);
        }
    }
    for (int p = 0; p < cpu_cycles * 2; p++) {
        if (h >= kActiveH0 && h < kActiveH1) {
            if (h == kActiveH0 && v == 0) {
                // Galaxian 1C drops the first two star clocks after VSYNC unless HFLIP.
                if (star_drop_ && stars_enable_ && !flip_x) star_base_ = wrap(star_base_ - 2LL);
                star_drop_ = false;
            }
            if (v < 256) paint(h - kActiveH0);
        } else if (h == kActiveH1) {
            line_buf_.fill(0);
            shell_ = {};
            missile_ = {};
        }
        for (int k = 0; k < 8; k++) {
            if (kSlotH[k] == h) {
                object_slot(k);
                break;
            }
        }
        if (++h == kHTotal) {
            h = 0;
            line_start();
        }
    }
}

void Video::render(uint32_t* upright_224x256) const {
    for (int line = 0; line < kVisH; line++) {
        const uint32_t* src = &frame_[unsigned(line * kVisW)];
        const int ux = kUprightW - 1 - line;
        for (int x = 0; x < kVisW; x++) upright_224x256[x * kUprightW + ux] = src[x];
    }
}

}  // namespace galaxian
