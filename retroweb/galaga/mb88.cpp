#include "mb88.h"

#include <utility>

namespace galaga {

namespace {

// EN/DIS operand bits (datasheet Table 5 note *3).
constexpr uint8_t kSerialIrq = 0x01;
constexpr uint8_t kTimerIrq = 0x02;
constexpr uint8_t kExtIrq = 0x04;
constexpr uint8_t kSerialInternal = 0x20;
constexpr uint8_t kCounterTc = 0x40;
constexpr uint8_t kTimerInternal = 0x80;

constexpr int kTimerPrescale = 32;
constexpr int kIrqEntryCycles = 3;

}  // namespace

void Mb88::reset() {
    a = x = y = 0;
    pc = pa = 0;
    st = 1;
    zf = cf = vf = sf = 0;
    pio = 0;
    th = tl = sb = 0;
    o_output = 0xFF;
    r_latch_.fill(0x0F);
    p_latch_ = 0x0F;
    stack_ = {};
    sp_ = 0;
    in_irq = false;
    req_ext_ = req_timer_ = req_serial_ = false;
    prescale_ = 0;
    shifts_ = 0;
    halted_reset = false;
}

void Mb88::set_irq(bool level) {
    if (level && !irq_pin_ && (pio & kExtIrq)) req_ext_ = true;
    irq_pin_ = level;
}

void Mb88::set_tc(bool level) {
    if (!level && tc_pin_ && (pio & kCounterTc)) count_timer();
    tc_pin_ = level;
}

uint8_t Mb88::fetch() {
    uint8_t b = rom[unsigned(pc_full()) % kRomBytes];
    pc = uint8_t((pc + 1) & 0x3F);
    if (pc == 0) pa = uint8_t((pa + 1) & 0x1F);
    return b;
}

void Mb88::push(uint16_t ret) {
    stack_[unsigned(sp_)] = {ret, cf, zf, st};
    sp_ = (sp_ + 1) & 3;
}

void Mb88::count_timer() {
    tl = uint8_t((tl + 1) & 0x0F);
    if (tl != 0) return;
    th = uint8_t((th + 1) & 0x0F);
    if (th != 0) return;
    vf = 1;
    if (pio & kTimerIrq) req_timer_ = true;
}

void Mb88::clock(int cycles) {
    for (int i = 0; i < cycles; i++) {
        if ((pio & kTimerInternal) && ++prescale_ >= kTimerPrescale) {
            prescale_ = 0;
            count_timer();
        }
        if (pio & kSerialInternal) {
            bool in = read_si ? read_si() : (si != 0);
            sb = uint8_t((sb >> 1) | (in ? 8 : 0));
            if (++shifts_ == 4) {
                shifts_ = 0;
                sf = 1;
                if (pio & kSerialIrq) req_serial_ = true;
            }
        }
    }
}

int Mb88::take_interrupt() {
    if (in_irq) return 0;
    uint8_t vector;
    if (req_ext_ && (pio & kExtIrq)) {
        req_ext_ = false;
        vector = 0x02;
    } else if (req_timer_ && (pio & kTimerIrq)) {
        req_timer_ = false;
        vector = 0x04;
    } else if (req_serial_ && (pio & kSerialIrq)) {
        req_serial_ = false;
        vector = 0x06;
    } else {
        return 0;
    }
    push(uint16_t(pc_full()));
    in_irq = true;
    pa = 0;
    pc = vector;
    return kIrqEntryCycles;
}

uint8_t Mb88::r_in(int port) const {
    if (read_r) return uint8_t(read_r(port) & 0x0F);
    return r_latch_[unsigned(port)];
}

void Mb88::r_out(int port, uint8_t v) {
    r_latch_[unsigned(port)] = uint8_t(v & 0x0F);
    if (write_r) write_r(port, r_latch_[unsigned(port)]);
}

int Mb88::step() {
    if (halted_reset) return 1;
    int cycles = take_interrupt();
    if (cycles == 0) {
        cycles = 1;
        execute(fetch(), cycles);
    }
    clock(cycles);
    return cycles;
}

void Mb88::execute(uint8_t op, int& cycles) {
    const uint8_t taken = st;
    st = 1;
    auto zero = [this](uint8_t v) { zf = v == 0; };
    auto jump = [this](int addr) {
        pa = uint8_t((addr >> 6) & 0x1F);
        pc = uint8_t(addr & 0x3F);
    };
    auto add = [&](int sum) {
        cf = sum > 15;
        a = uint8_t(sum & 0x0F);
        zero(a);
        st = !cf;
    };
    auto compare = [&](int diff) {
        cf = diff < 0;
        zero(uint8_t(diff & 0x0F));
        st = !zf;
    };

    if (op >= 0xC0) {
        if (taken) pc = uint8_t(op & 0x3F);
        return;
    }
    switch (op >> 4) {
    case 0x4:
        switch (op & 0x0C) {
        case 0x0: r_out(0, uint8_t(r_latch_[0] | (1 << (op & 3)))); return;
        case 0x4: r_out(0, uint8_t(r_latch_[0] & ~(1 << (op & 3)))); return;
        case 0x8: st = !((r_in(2) >> (op & 3)) & 1); return;
        default: st = !((a >> (op & 3)) & 1); return;
        }
    case 0x5:
        if (op < 0x54) {
            uint8_t& m = ram_[op & 3];
            std::swap(a, m);
            zero(a);
        } else if (op < 0x58) {
            uint8_t& m = ram_[op & 7];
            std::swap(y, m);
            zero(y);
        } else {
            x = uint8_t(op & 7);
            zero(x);
        }
        return;
    case 0x6: {
        uint8_t lo = fetch();
        cycles = 2;
        int addr = ((op & 7) << 8) | lo;
        if (!taken) return;
        if (op < 0x68) push(uint16_t(pc_full()));
        jump(addr);
        return;
    }
    case 0x7: add(a + (op & 0x0F)); return;
    case 0x8: y = uint8_t(op & 0x0F); zero(y); return;
    case 0x9: a = uint8_t(op & 0x0F); zero(a); return;
    case 0xA: compare((op & 0x0F) - y); return;
    case 0xB: compare((op & 0x0F) - a); return;
    default: break;
    }
    if (op >= 0x30 && op < 0x3C) {
        uint8_t bit = uint8_t(1 << (op & 3));
        uint8_t& m = mem();
        if (op < 0x34) m = uint8_t(m | bit);
        else if (op < 0x38) m = uint8_t(m & ~bit);
        else st = !(m & bit);
        return;
    }

    uint8_t& m = mem();
    switch (op) {
    case 0x00: return;
    case 0x01: {
        uint8_t mask = cf ? 0xF0 : 0x0F;
        o_output = uint8_t((o_output & ~mask) | (cf ? a << 4 : a));
        if (write_o) write_o(o_output, mask);
        return;
    }
    case 0x02:
        p_latch_ = a;
        if (write_p) write_p(a);
        return;
    case 0x03: r_out(y & 3, a); return;
    case 0x04: y = a; return;
    case 0x05: th = a; return;
    case 0x06: tl = a; return;
    case 0x07: sb = a; return;
    case 0x08: y = uint8_t((y + 1) & 0x0F); zero(y); st = y != 0; return;
    case 0x09: m = uint8_t((m + 1) & 0x0F); zero(m); st = m != 0; return;
    case 0x0A: m = a; y = uint8_t((y + 1) & 0x0F); zero(y); st = y != 0; return;
    case 0x0B: std::swap(a, m); zero(a); return;
    case 0x0C: {
        uint8_t out = a >> 3;
        a = uint8_t(((a << 1) | cf) & 0x0F);
        cf = out;
        zero(a);
        st = !cf;
        return;
    }
    case 0x0D: a = m; zero(a); return;
    case 0x0E: add(a + m + cf); return;
    case 0x0F: a = uint8_t(a & m); zero(a); st = !zf; return;
    case 0x10:
    case 0x11:
        if (a > 9 || cf) {
            a = uint8_t((a + (op == 0x10 ? 6 : 10)) & 0x0F);
            cf = 1;
        }
        st = !cf;
        return;
    case 0x12: a = uint8_t((read_k ? read_k() : 0x0F) & 0x0F); zero(a); return;
    case 0x13: a = r_in(y & 3); zero(a); return;
    case 0x14: a = y; zero(a); return;
    case 0x15: a = th; zero(a); return;
    case 0x16: a = tl; zero(a); return;
    case 0x17: a = sb; zero(a); return;
    case 0x18: st = y != 0; y = uint8_t((y - 1) & 0x0F); return;
    case 0x19: st = m != 0; m = uint8_t((m - 1) & 0x0F); zero(m); return;
    case 0x1A: m = a; st = y != 0; y = uint8_t((y - 1) & 0x0F); zero(y); return;
    case 0x1B: std::swap(a, x); zero(a); return;
    case 0x1C: {
        uint8_t out = a & 1;
        a = uint8_t((a >> 1) | (cf << 3));
        cf = out;
        zero(a);
        st = !cf;
        return;
    }
    case 0x1D: m = a; return;
    case 0x1E: {
        int diff = m - a - cf;
        cf = diff < 0;
        a = uint8_t(diff & 0x0F);
        zero(a);
        st = !cf;
        return;
    }
    case 0x1F: a = uint8_t(a | m); zero(a); st = !zf; return;
    case 0x20: r_out((y >> 2) & 3, uint8_t(r_latch_[(y >> 2) & 3] | (1 << (y & 3)))); return;
    case 0x21: cf = 1; return;
    case 0x22: r_out((y >> 2) & 3, uint8_t(r_latch_[(y >> 2) & 3] & ~(1 << (y & 3)))); return;
    case 0x23: cf = 0; return;
    case 0x24: st = !((r_in((y >> 2) & 3) >> (y & 3)) & 1); return;
    case 0x25: st = !irq_pin_; return;
    case 0x26: st = !vf; vf = 0; return;
    case 0x27: st = !sf; sf = 0; return;
    case 0x28: st = !cf; return;
    case 0x29: st = !zf; return;
    case 0x2A: m = sb; zero(m); return;
    case 0x2B: sb = m; zero(sb); return;
    case 0x2C:
    case 0x3C: {
        sp_ = (sp_ - 1) & 3;
        const Frame& f = stack_[unsigned(sp_)];
        jump(f.pc);
        if (op == 0x3C) {
            cf = f.cf;
            zf = f.zf;
            st = f.st;
            in_irq = false;
        }
        return;
    }
    case 0x2D: a = uint8_t((16 - a) & 0x0F); st = a != 0; return;
    case 0x2E: compare(m - a); return;
    case 0x2F: a = uint8_t(a ^ m); zero(a); st = !zf; return;
    case 0x3D: {
        uint8_t page = fetch();
        cycles = 2;
        pa = uint8_t(page & 0x1F);
        pc = uint8_t((a * 4) & 0x3F);
        return;
    }
    case 0x3E:
    case 0x3F: {
        uint8_t bits = fetch();
        cycles = 2;
        pio = op == 0x3E ? uint8_t(pio | bits) : uint8_t(pio & ~bits);
        return;
    }
    default: return;
    }
}

}  // namespace galaga
