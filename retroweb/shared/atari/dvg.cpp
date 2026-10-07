#include "dvg.h"

#include <algorithm>
#include <cmath>

namespace atari {

void Dvg::reset() {
    halt = true;
    x = y = 0;
    scale = 0;
    sp_ = 0;
    pc_ = 0;
    segments.clear();
}

void Dvg::go(const MemRead& read_word) {
    halt = false;
    sp_ = 0;
    pc_ = 0;
    segments.clear();
    run(read_word);
}

int Dvg::apply_scale(int delta, int total_scale) {
    // global + local scale added as 4-bit; 0..9 selects /512../1, beyond that the
    // beam doesn't advance (nmikstas asteroidsHDL "Total Scaling Value")
    total_scale &= 0xF;
    if (total_scale > 9) return 0;
    int div = 512 >> total_scale;
    if (div <= 0) div = 1;
    return delta / div;
}

void Dvg::draw_delta(int dx, int dy, uint8_t intensity) {
    int x0 = x, y0 = y;
    int x1 = x + dx;
    int y1 = y + dy;
    // 12-bit counters keep counting off the 10-bit tube (bit 10 blanks the beam)
    // so SVEC chains from one LABS stay put (MAME avgdvg.cpp dvg_gostrobe).
    // Zero-length bright VECs are photon shots: draw a point.
    if (intensity && dx == 0 && dy == 0) {
        if (unsigned(x0) <= 1023 && unsigned(y0) <= 1023)
            segments.push_back(VectorSeg{x0, y0, x0, y0, intensity});
        x = x1;
        y = y1;
        return;
    }

    if (intensity) {
        // Liang-Barsky against the visible window; cursor is not clamped
        double t0 = 0, t1 = 1;
        auto clip = [&](double p, double q) {
            if (p == 0) return q >= 0;
            double r = q / p;
            if (p < 0) {
                if (r > t1) return false;
                if (r > t0) t0 = r;
            } else {
                if (r < t0) return false;
                if (r < t1) t1 = r;
            }
            return true;
        };
        double dxd = double(x1 - x0), dyd = double(y1 - y0);
        if (clip(-dxd, x0 - 0) && clip(dxd, 1023 - x0) &&
            clip(-dyd, y0 - 0) && clip(dyd, 1023 - y0) && t0 < t1) {
            int sx = int(std::lround(x0 + t0 * dxd));
            int sy = int(std::lround(y0 + t0 * dyd));
            int ex = int(std::lround(x0 + t1 * dxd));
            int ey = int(std::lround(y0 + t1 * dyd));
            sx = std::clamp(sx, 0, 1023);
            sy = std::clamp(sy, 0, 1023);
            ex = std::clamp(ex, 0, 1023);
            ey = std::clamp(ey, 0, 1023);
            segments.push_back(VectorSeg{sx, sy, ex, ey, intensity});
        }
    }
    x = x1;
    y = y1;
}

void Dvg::run(const MemRead& read_word) {
    for (int n = 0; n < kMaxOps && !halt; n++) {
        uint16_t op = read_word(pc_++);
        uint8_t cmd = uint8_t(op >> 12);

        if (cmd <= 0x9) {
            uint16_t op2 = read_word(pc_++);
            int local_scale = (op >> 12) & 0xF;
            int ymag = op & 0x3FF;
            if (op & 0x400) ymag = -ymag;
            int bri = (op2 >> 12) & 0xF;
            int xmag = op2 & 0x3FF;
            if (op2 & 0x400) xmag = -xmag;
            int total = scale + local_scale;
            draw_delta(apply_scale(xmag, total), apply_scale(ymag, total), uint8_t(bri));
            continue;
        }

        switch (cmd) {
            case 0xA: {
                uint16_t op2 = read_word(pc_++);
                y = op & 0x3FF;
                x = op2 & 0x3FF;
                scale = (op2 >> 12) & 0xF;
                break;
            }
            case 0xB:
                halt = true;
                break;
            case 0xC: {
                // JSR: 12-bit word address, stack depth 4
                uint16_t dest = op & 0x0FFF;
                if (sp_ < kStackDepth) {
                    stack_[sp_++] = pc_;
                    pc_ = dest;
                } else {
                    halt = true;  // stack overflow: freeze (real TTL also misbehaves)
                }
                break;
            }
            case 0xD:
                if (sp_ > 0) pc_ = stack_[--sp_];
                else halt = true;
                break;
            case 0xE:
                pc_ = op & 0x0FFF;
                break;
            case 0xF: {
                // SVEC: Ss = ((op&0x800)>>11)|((op&8)>>2) remaps to VEC local scale
                // 2..5, deltas in bits 9:8 (Asteroids HDL; computerarcheology VectorROM)
                int ss = int(((op & 0x800) >> 11) | ((op & 0x8) >> 2));
                int ymag = (op >> 8) & 3;
                if (op & 0x400) ymag = -ymag;
                int xmag = op & 3;
                if (op & 0x4) xmag = -xmag;
                int bri = (op >> 4) & 0xF;
                int local = ss + 2;
                int total = scale + local;
                draw_delta(apply_scale(xmag << 8, total),
                           apply_scale(ymag << 8, total),
                           uint8_t(bri));
                break;
            }
            default:
                halt = true;
                break;
        }
    }
    halt = true;
}

}  // namespace atari
