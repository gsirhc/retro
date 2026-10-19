#include "mathbox.h"

#include <cstddef>

namespace atari {

namespace {

// Am2901 source operand pairs (R, S) by I2-I0.
void operands(int src, uint16_t a, uint16_t b, uint16_t q, uint16_t d,
              uint16_t& r, uint16_t& s) {
    switch (src & 7) {
        case 0: r = a; s = q; break;
        case 1: r = a; s = b; break;
        case 2: r = 0; s = q; break;
        case 3: r = 0; s = b; break;
        case 4: r = 0; s = a; break;
        case 5: r = d; s = a; break;
        case 6: r = d; s = q; break;
        default: r = d; s = 0; break;
    }
}

}  // namespace

void Mathbox::reset() {
    r.fill(0);
    q = y = 0;
    running_ = false;
    upc_ = jump_ = 0;
    d_ = 0;
    q0_ = true;
    ticks_ = 0;
    steps = 0;
}

void Mathbox::load_proms(const uint8_t* map32, const uint8_t* const nibbles[6]) {
    for (int i = 0; i < 32; i++) map[size_t(i)] = map32[i];
    for (int a = 0; a < 256; a++) {
        uint32_t w = 0;
        for (int p = 0; p < 6; p++)
            w |= uint32_t(nibbles[p][a] & 0xF) << (20 - 4 * p);
        ucode[size_t(a)] = w;
    }
}

void Mathbox::write(uint8_t offset, uint8_t data) {
    upc_ = map[offset & 0x1F];
    // D and Y share the host bus; D7 also feeds D15 (MBUDOC).
    d_ = uint16_t(data | (data << 8));
    running_ = true;
}

void Mathbox::run(int cpu_cycles) {
    if (!running_) {
        ticks_ = 0;
        return;
    }
    ticks_ += cpu_cycles * kTicksPerCpuCycle;
    while (ticks_ > 0 && running_) {
        ticks_--;
        if (step()) running_ = false;
    }
    if (!running_) ticks_ = 0;
}

bool Mathbox::step() {
    const uint32_t w = ucode[upc_];
    const int ra = int((w >> 20) & 0xF);
    const int rb = int((w >> 16) & 0xF);
    const int i2hi = int((w >> 15) & 1);
    const int i2lo = int((w >> 14) & 1);
    int i10 = int((w >> 12) & 3);
    const bool stall = (w >> 11) & 1;
    const int func = int((w >> 8) & 7);
    const bool ldab = (w >> 7) & 1;
    const int dest = int((w >> 4) & 7);
    const bool sign = (w >> 3) & 1;
    const bool jmp = (w >> 2) & 1;
    const bool mult = (w >> 1) & 1;
    unsigned c = w & 1;

    // CADD: a 0 shifted out of Q last cycle turns A,B into 0,B.
    if (mult && !q0_) i10 ^= 2;

    const uint16_t av = r[size_t(ra)], bv = r[size_t(rb)];
    uint16_t rh, sh, rl, sl;
    operands((i2hi << 2) | i10, av, bv, q, d_, rh, sh);
    operands((i2lo << 2) | i10, av, bv, q, d_, rl, sl);
    const uint16_t R = uint16_t((rh & 0xFF00) | (rl & 0x00FF));
    const uint16_t S = uint16_t((sh & 0xFF00) | (sl & 0x00FF));

    uint16_t F = 0;
    unsigned ovr = 0;
    for (int k = 0; k < 4; k++) {
        unsigned r4 = (R >> (4 * k)) & 0xF, s4 = (S >> (4 * k)) & 0xF, f4 = 0, c4 = 0;
        switch (func) {
            case 0: case 1: case 2: {
                unsigned rr = func == 1 ? (~r4 & 0xF) : r4;
                unsigned ss = func == 2 ? (~s4 & 0xF) : s4;
                unsigned sum = rr + ss + c;
                unsigned c3 = ((rr & 7) + (ss & 7) + c) >> 3;
                f4 = sum & 0xF;
                c4 = sum >> 4;
                ovr = c3 ^ c4;
                break;
            }
            // Logic carries per the Am2901 P/G table; EXOR/EXNOR left at 0.
            case 3: f4 = r4 | s4; c4 = (f4 != 0xF) | c; ovr = c4; break;
            case 4: f4 = r4 & s4; c4 = (f4 != 0) | c; ovr = c4; break;
            case 5: f4 = ~r4 & s4 & 0xF; c4 = (f4 != 0) | c; ovr = c4; break;
            case 6: f4 = r4 ^ s4; ovr = 0; break;
            default: f4 = ~(r4 ^ s4) & 0xF; ovr = 0; break;
        }
        F = uint16_t(F | (f4 << (4 * k)));
        c = c4;
    }
    // MSB*: what an ASR shifts in, and the JPL condition (N xor V when SIGN).
    const unsigned msb = sign ? (ovr ^ ((F >> 15) & 1)) : 0;

    y = F;
    switch (dest) {
        case 0: q = F; q0_ = true; break;
        case 1: q0_ = true; break;
        case 2: r[size_t(rb)] = F; y = av; q0_ = true; break;
        case 3: r[size_t(rb)] = F; q0_ = true; break;
        case 4:
            r[size_t(rb)] = uint16_t((F >> 1) | (msb << 15));
            q0_ = q & 1;
            q = uint16_t((q >> 1) | ((F & 1) << 15));
            break;
        case 5:
            r[size_t(rb)] = uint16_t((F >> 1) | (msb << 15));
            q0_ = q & 1;
            break;
        case 6:
            // Q0 in is forced low on a left shift; Q15 feeds RAM0.
            r[size_t(rb)] = uint16_t((F << 1) | (q >> 15));
            q = uint16_t(q << 1);
            q0_ = false;
            break;
        default:
            // RAM0 floats on RAMU (MBUDOC); a TTL input floats high.
            r[size_t(rb)] = uint16_t((F << 1) | 1);
            q0_ = false;
            break;
    }

    if (ldab) jump_ = uint8_t(w >> 16);
    upc_ = (jmp && msb == 0) ? jump_ : uint8_t(upc_ + 1);
    steps++;
    return stall;
}

}  // namespace atari
