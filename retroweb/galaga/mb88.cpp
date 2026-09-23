#include "mb88.h"

namespace galaga {

namespace {

constexpr int kTimerPrescale = 32;
constexpr int kIntExternal = 0x04;
constexpr int kIntTimer = 0x02;

}  // namespace

void Mb88::reset() {
    pc = pa = si = 0;
    a = x = y = 0;
    st = 1;
    zf = cf = vf = sf = pio = 0;
    th = tl = sb = 0;
    o_output = 0;
    in_irq = false;
    halted_reset = false;
    sp_.fill(0);
    data_.fill(0);
    tp_ = 0;
    pending_ = 0;
    if_ = false;
    ctr_ = false;
}

void Mb88::set_irq(bool level) {
    bool state = level;
    if (!if_ && state && (pio & kIntExternal)) pending_ |= kIntExternal;
    if_ = state;
}

void Mb88::set_tc(bool level) {
    bool state = level;
    if (ctr_ && !state && (pio & 0x40)) {
        tl = uint8_t((tl + 1) & 0x0f);
        if (tl == 0) {
            th = uint8_t((th + 1) & 0x0f);
            if (th == 0) {
                vf = 1;
                pending_ |= kIntTimer;
            }
        }
    }
    ctr_ = state;
}

uint8_t Mb88::fetch() {
    uint8_t op = rom[unsigned(pc_full()) & (kRomBytes - 1)];
    pc++;
    if (pc >= 0x40) {
        pc = 0;
        pa = uint8_t((pa + 1) & 0x1f);
    }
    return op;
}

uint8_t Mb88::read_data(int addr) const {
    return data_[unsigned(addr) & (kDataNibbles - 1)] & 0x0f;
}

void Mb88::write_data(int addr, uint8_t v) {
    data_[unsigned(addr) & (kDataNibbles - 1)] = uint8_t(v & 0x0f);
}

uint8_t Mb88::in_r(int n) const {
    if (!read_r) return 0;
    return uint8_t(read_r(n & 3) & 0x0f);
}

void Mb88::out_r(int n, uint8_t v) {
    if (write_r) write_r(n & 3, uint8_t(v & 0x0f));
}

void Mb88::out_o(uint8_t index) {
    uint8_t shift = (index & 0x10) ? 4 : 0;
    uint8_t mask = uint8_t(0x0f << shift);
    o_output = uint8_t((o_output & ~mask) | ((index << shift) & mask));
    if (write_o) write_o(o_output, mask);
}

void Mb88::burn(int cycles) {
    if (pio & 0x80) {
        tp_ += cycles;
        while (tp_ >= kTimerPrescale) {
            tp_ -= kTimerPrescale;
            tl = uint8_t((tl + 1) & 0x0f);
            if (tl == 0) {
                th = uint8_t((th + 1) & 0x0f);
                if (th == 0) {
                    vf = 1;
                    pending_ |= kIntTimer;
                }
            }
        }
    }
    if (!in_irq && (pending_ & pio)) {
        in_irq = true;
        int intpc = pc_full();
        sp_[si] = uint16_t(intpc | (cf << 15) | (zf << 14) | (st << 13));
        si = uint8_t((si + 1) & 3);
        if (pending_ & pio & kIntExternal) pc = 0x02;
        else if (pending_ & pio & kIntTimer) pc = 0x04;
        pa = 0;
        st = 1;
        pending_ = 0;
        burn(3);
    }
}

int Mb88::step() {
    if (halted_reset) return 1;
    uint8_t opcode = fetch();
    int oc = 1;
    int ea = (int(x) << 4) + y;
    auto st_c = [&](int v) { st = (v & 0x10) ? 0 : 1; };
    auto st_z = [&](int v) { st = (v == 0) ? 0 : 1; };
    auto set_cf = [&](int v) { cf = ((v & 0x10) == 0) ? 0 : 1; };
    auto set_zf = [&](int v) { zf = (v != 0) ? 0 : 1; };

    if (opcode == 0x00) {
        st = 1;
    } else if (opcode == 0x01) {
        out_o(uint8_t((cf << 4) | a));
        st = 1;
    } else if (opcode == 0x02) {
        if (write_p) write_p(a);
        st = 1;
    } else if (opcode == 0x03) {
        out_r(y, a);
        st = 1;
    } else if (opcode == 0x04) {
        y = a;
        st = 1;
    } else if (opcode == 0x05) {
        th = a;
        st = 1;
    } else if (opcode == 0x06) {
        tl = a;
        st = 1;
    } else if (opcode == 0x07) {
        sb = a;
        st = 1;
    } else if (opcode == 0x08) {
        y = uint8_t(y + 1);
        st_c(y);
        y = uint8_t(y & 0x0f);
        set_zf(y);
    } else if (opcode == 0x09) {
        int arg = read_data(ea) + 1;
        st_c(arg);
        arg &= 0x0f;
        set_zf(arg);
        write_data(ea, uint8_t(arg));
    } else if (opcode == 0x0a) {
        write_data(ea, a);
        y = uint8_t(y + 1);
        st_c(y);
        y = uint8_t(y & 0x0f);
        set_zf(y);
    } else if (opcode == 0x0b) {
        uint8_t arg = read_data(ea);
        write_data(ea, a);
        a = arg;
        set_zf(a);
        st = 1;
    } else if (opcode == 0x0c) {
        a = uint8_t((a << 1) | cf);
        st_c(a);
        cf = uint8_t(st ^ 1);
        a = uint8_t(a & 0x0f);
        set_zf(a);
    } else if (opcode == 0x0d) {
        a = read_data(ea);
        set_zf(a);
        st = 1;
    } else if (opcode == 0x0e) {
        int arg = read_data(ea) + a + cf;
        st_c(arg);
        cf = uint8_t(st ^ 1);
        a = uint8_t(arg & 0x0f);
        set_zf(a);
    } else if (opcode == 0x0f) {
        a = uint8_t(a & read_data(ea));
        set_zf(a);
        st = uint8_t(zf ^ 1);
    } else if (opcode == 0x10) {
        if (cf || a > 9) a = uint8_t(a + 6);
        st_c(a);
        cf = uint8_t(st ^ 1);
        a = uint8_t(a & 0x0f);
    } else if (opcode == 0x11) {
        if (cf || a > 9) a = uint8_t(a + 10);
        st_c(a);
        cf = uint8_t(st ^ 1);
        a = uint8_t(a & 0x0f);
    } else if (opcode == 0x12) {
        a = uint8_t((read_k ? read_k() : 0) & 0x0f);
        set_zf(a);
        st = 1;
    } else if (opcode == 0x13) {
        a = in_r(y);
        set_zf(a);
        st = 1;
    } else if (opcode == 0x14) {
        a = y;
        set_zf(a);
        st = 1;
    } else if (opcode == 0x15) {
        a = th;
        set_zf(a);
        st = 1;
    } else if (opcode == 0x16) {
        a = tl;
        set_zf(a);
        st = 1;
    } else if (opcode == 0x17) {
        a = sb;
        set_zf(a);
        st = 1;
    } else if (opcode == 0x18) {
        y = uint8_t(y - 1);
        st_c(y);
        y = uint8_t(y & 0x0f);
    } else if (opcode == 0x19) {
        int arg = read_data(ea) - 1;
        st_c(arg);
        arg &= 0x0f;
        set_zf(arg);
        write_data(ea, uint8_t(arg));
    } else if (opcode == 0x1a) {
        write_data(ea, a);
        y = uint8_t(y - 1);
        st_c(y);
        y = uint8_t(y & 0x0f);
        set_zf(y);
    } else if (opcode == 0x1b) {
        uint8_t arg = x;
        x = a;
        a = arg;
        set_zf(a);
        st = 1;
    } else if (opcode == 0x1c) {
        int arg = a | (cf << 4);
        st_c(arg << 4);
        cf = uint8_t(st ^ 1);
        a = uint8_t((arg >> 1) & 0x0f);
        set_zf(a);
    } else if (opcode == 0x1d) {
        write_data(ea, a);
        st = 1;
    } else if (opcode == 0x1e) {
        int arg = read_data(ea) - a - cf;
        st_c(arg);
        cf = uint8_t(st ^ 1);
        a = uint8_t(arg & 0x0f);
        set_zf(a);
    } else if (opcode == 0x1f) {
        a = uint8_t(a | read_data(ea));
        set_zf(a);
        st = uint8_t(zf ^ 1);
    } else if (opcode == 0x20) {
        int n = y >> 2;
        out_r(n, uint8_t(in_r(n) | (1 << (y & 3))));
        st = 1;
    } else if (opcode == 0x21) {
        cf = 1;
        st = 1;
    } else if (opcode == 0x22) {
        int n = y >> 2;
        out_r(n, uint8_t(in_r(n) & ~(1 << (y & 3))));
        st = 1;
    } else if (opcode == 0x23) {
        cf = 0;
        st = 1;
    } else if (opcode == 0x24) {
        int n = y >> 2;
        st = (in_r(n) & (1 << (y & 3))) ? 0 : 1;
    } else if (opcode == 0x25) {
        st = uint8_t(if_ ^ 1);
    } else if (opcode == 0x26) {
        st = uint8_t(vf ^ 1);
        vf = 0;
    } else if (opcode == 0x27) {
        st = uint8_t(sf ^ 1);
        sf = 0;
    } else if (opcode == 0x28) {
        st = uint8_t(cf ^ 1);
    } else if (opcode == 0x29) {
        st = uint8_t(zf ^ 1);
    } else if (opcode == 0x2a) {
        write_data(ea, sb);
        set_zf(sb);
        st = 1;
    } else if (opcode == 0x2b) {
        sb = read_data(ea);
        set_zf(sb);
        st = 1;
    } else if (opcode == 0x2c) {
        si = uint8_t((si - 1) & 3);
        pc = uint8_t(sp_[si] & 0x3f);
        pa = uint8_t((sp_[si] >> 6) & 0x1f);
        st = 1;
    } else if (opcode == 0x2d) {
        a = uint8_t(((~a) + 1) & 0x0f);
        st_z(a);
    } else if (opcode == 0x2e) {
        int arg = read_data(ea) - a;
        set_cf(arg);
        arg &= 0x0f;
        st_z(arg);
        zf = uint8_t(st ^ 1);
    } else if (opcode == 0x2f) {
        a = uint8_t(a ^ read_data(ea));
        st_z(a);
        zf = uint8_t(st ^ 1);
    } else if (opcode >= 0x30 && opcode <= 0x33) {
        write_data(ea, uint8_t(read_data(ea) | (1 << (opcode & 3))));
        st = 1;
    } else if (opcode >= 0x34 && opcode <= 0x37) {
        write_data(ea, uint8_t(read_data(ea) & ~(1 << (opcode & 3))));
        st = 1;
    } else if (opcode >= 0x38 && opcode <= 0x3b) {
        st = (read_data(ea) & (1 << (opcode & 3))) ? 0 : 1;
    } else if (opcode == 0x3c) {
        in_irq = false;
        si = uint8_t((si - 1) & 3);
        pc = uint8_t(sp_[si] & 0x3f);
        pa = uint8_t((sp_[si] >> 6) & 0x1f);
        st = uint8_t((sp_[si] >> 13) & 1);
        zf = uint8_t((sp_[si] >> 14) & 1);
        cf = uint8_t((sp_[si] >> 15) & 1);
    } else if (opcode == 0x3d) {
        pa = uint8_t(fetch() & 0x1f);
        pc = uint8_t(a * 4);
        oc++;
        st = 1;
    } else if (opcode == 0x3e) {
        pio = uint8_t(pio | fetch());
        oc++;
        st = 1;
    } else if (opcode == 0x3f) {
        pio = uint8_t(pio & ~fetch());
        oc++;
        st = 1;
    } else if (opcode >= 0x40 && opcode <= 0x43) {
        out_r(0, uint8_t(in_r(0) | (1 << (opcode & 3))));
        st = 1;
    } else if (opcode >= 0x44 && opcode <= 0x47) {
        out_r(0, uint8_t(in_r(0) & ~(1 << (opcode & 3))));
        st = 1;
    } else if (opcode >= 0x48 && opcode <= 0x4b) {
        st = (in_r(2) & (1 << (opcode & 3))) ? 0 : 1;
    } else if (opcode >= 0x4c && opcode <= 0x4f) {
        st = (a & (1 << (opcode & 3))) ? 0 : 1;
    } else if (opcode >= 0x50 && opcode <= 0x53) {
        int n = opcode & 3;
        uint8_t arg = read_data(n);
        write_data(n, a);
        a = arg;
        set_zf(a);
        st = 1;
    } else if (opcode >= 0x54 && opcode <= 0x57) {
        int n = (opcode & 3) + 4;
        uint8_t arg = read_data(n);
        write_data(n, y);
        y = arg;
        set_zf(y);
        st = 1;
    } else if (opcode >= 0x58 && opcode <= 0x5f) {
        x = uint8_t(opcode & 7);
        set_zf(x);
        st = 1;
    } else if (opcode >= 0x60 && opcode <= 0x67) {
        uint8_t arg = fetch();
        oc++;
        if (st & 1) {
            sp_[si] = uint16_t(pc_full());
            si = uint8_t((si + 1) & 3);
            pc = uint8_t(arg & 0x3f);
            pa = uint8_t(((opcode & 7) << 2) | (arg >> 6));
        }
        st = 1;
    } else if (opcode >= 0x68 && opcode <= 0x6f) {
        uint8_t arg = fetch();
        oc++;
        if (st & 1) {
            pc = uint8_t(arg & 0x3f);
            pa = uint8_t(((opcode & 7) << 2) | (arg >> 6));
        }
        st = 1;
    } else if (opcode >= 0x70 && opcode <= 0x7f) {
        int arg = (opcode & 0x0f) + a;
        st_c(arg);
        cf = uint8_t(st ^ 1);
        a = uint8_t(arg & 0x0f);
        set_zf(a);
    } else if (opcode >= 0x80 && opcode <= 0x8f) {
        y = uint8_t(opcode & 0x0f);
        set_zf(y);
        st = 1;
    } else if (opcode >= 0x90 && opcode <= 0x9f) {
        a = uint8_t(opcode & 0x0f);
        set_zf(a);
        st = 1;
    } else if (opcode >= 0xa0 && opcode <= 0xaf) {
        int arg = (opcode & 0x0f) - y;
        set_cf(arg);
        arg &= 0x0f;
        st_z(arg);
        zf = uint8_t(st ^ 1);
    } else if (opcode >= 0xb0 && opcode <= 0xbf) {
        int arg = (opcode & 0x0f) - a;
        set_cf(arg);
        arg &= 0x0f;
        st_z(arg);
        zf = uint8_t(st ^ 1);
    } else if (st & 1) {
        pc = uint8_t(opcode & 0x3f);
        st = 1;
    } else {
        st = 1;
    }
    burn(oc);
    return oc;
}

}  // namespace galaga
