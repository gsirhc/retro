#include "pit8253.h"

namespace pc486 {

namespace {

uint32_t bcd_to_bin(uint16_t v) {
    return uint32_t((v & 0xF) + ((v >> 4) & 0xF) * 10 + ((v >> 8) & 0xF) * 100 + ((v >> 12) & 0xF) * 1000);
}

uint16_t bin_to_bcd(uint32_t v) {
    return uint16_t((v % 10) | ((v / 10 % 10) << 4) | ((v / 100 % 10) << 8) | ((v / 1000 % 10) << 12));
}

}  // namespace

void Pit8253::reset() {
    for (auto &ch : ch_) ch = Channel{};
    pit_credit_ = 0.0;
    prev_cycles_ = 0;
    ch0_rises_ = 0;
}

void Pit8253::set_output(int idx, bool level) {
    Channel &ch = ch_[idx];
    if (idx == 0 && level && !ch.output) ++ch0_rises_;
    ch.output = level;
}

void Pit8253::latch_count(Channel &ch) {
    if (ch.count_latched) return;  // a second latch before the first is read is ignored
    uint32_t v = uint32_t(ch.ce < 0 ? 0 : ch.ce);
    ch.latch_value = ch.bcd ? bin_to_bcd(v % 10000) : uint16_t(v & 0xFFFF);
    ch.count_latched = true;
}

void Pit8253::latch_status(Channel &ch) {
    if (ch.status_latched) return;
    ch.status = uint8_t((ch.output ? 0x80 : 0) | (ch.null_count ? 0x40 : 0) | ch.control);
    ch.status_latched = true;
}

uint8_t Pit8253::in(uint16_t port) {
    if (port == 0x43) return 0xFF;  // control word register is write-only on real hardware
    Channel &ch = ch_[port - 0x40];
    if (ch.status_latched) {
        ch.status_latched = false;
        return ch.status;
    }
    uint16_t v;
    if (ch.count_latched) {
        v = ch.latch_value;
    } else {
        uint32_t live = uint32_t(ch.ce < 0 ? 0 : ch.ce);
        v = ch.bcd ? bin_to_bcd(live % 10000) : uint16_t(live & 0xFFFF);
    }
    uint8_t b;
    bool done = true;
    if (ch.access == 1) {
        b = uint8_t(v & 0xFF);
    } else if (ch.access == 2) {
        b = uint8_t(v >> 8);
    } else if (!ch.read_msb_pending) {
        b = uint8_t(v & 0xFF);
        ch.read_msb_pending = true;
        done = false;
    } else {
        b = uint8_t(v >> 8);
        ch.read_msb_pending = false;
    }
    if (done) ch.count_latched = false;
    return b;
}

void Pit8253::write_count(int idx, uint32_t raw) {
    Channel &ch = ch_[idx];
    uint32_t n = ch.bcd ? bcd_to_bin(uint16_t(raw)) : raw;
    if (n == 0) n = ch.bcd ? 10000 : 65536;
    ch.cr = n;
    ch.cr_written = true;
    ch.null_count = true;
    switch (ch.mode) {
        case 0:
            set_output(idx, false);
            ch.counting = false;
            ch.load_pending = true;
            break;
        case 4:
            ch.load_pending = true;
            break;
        case 2: case 3:
            // A running counter picks the new count up at its next reload.
            if (!ch.counting) ch.load_pending = true;
            break;
        default:  // modes 1 and 5 wait for a gate trigger
            break;
    }
}

void Pit8253::out(uint16_t port, uint8_t v) {
    if (port == 0x43) {
        int channel = (v >> 6) & 3;
        if (channel == 3) {  // 8254 read-back: bit 5 low latches count, bit 4 low latches status
            for (int i = 0; i < 3; ++i) {
                if (!(v & (2 << i))) continue;
                if (!(v & 0x20)) latch_count(ch_[i]);
                if (!(v & 0x10)) latch_status(ch_[i]);
            }
            return;
        }
        int rw = (v >> 4) & 3;
        Channel &ch = ch_[channel];
        if (rw == 0) {
            latch_count(ch);
            return;
        }
        ch.control = uint8_t(v & 0x3F);
        ch.access = rw;
        ch.mode = (v >> 1) & 7;
        if (ch.mode > 5) ch.mode -= 4;
        ch.bcd = (v & 1) != 0;
        ch.write_msb_pending = false;
        ch.read_msb_pending = false;
        ch.count_latched = false;
        ch.cr_written = false;
        ch.null_count = true;
        ch.counting = false;
        ch.load_pending = false;
        ch.trigger = false;
        ch.fired = false;
        ch.strobe = false;
        ch.odd_extra = false;
        set_output(channel, ch.mode != 0);
        return;
    }
    int idx = port - 0x40;
    Channel &ch = ch_[idx];
    switch (ch.access) {
        case 1: write_count(idx, v); break;
        case 2: write_count(idx, uint32_t(v) << 8); break;
        case 3:
            if (!ch.write_msb_pending) {
                ch.pending_lsb = v;
                ch.write_msb_pending = true;
                if (ch.mode == 0) {  // mode 0: the first byte stops the count and drops OUT
                    ch.counting = false;
                    set_output(idx, false);
                }
            } else {
                ch.write_msb_pending = false;
                write_count(idx, uint32_t(ch.pending_lsb) | (uint32_t(v) << 8));
            }
            break;
        default:
            break;  // no control word yet
    }
}

void Pit8253::set_gate2(bool level) {
    Channel &ch = ch_[2];
    bool was = ch.gate;
    ch.gate = level;
    if (!level) {
        if (ch.mode == 2 || ch.mode == 3) {
            set_output(2, true);
            ch.odd_extra = false;
        }
    } else if (!was && ch.cr_written) {
        if (ch.mode == 1 || ch.mode == 5) ch.trigger = true;
        else if (ch.mode == 2 || ch.mode == 3) ch.load_pending = true;
    }
}

void Pit8253::step_channel(int idx) {
    Channel &ch = ch_[idx];
    switch (ch.mode) {
        case 0:
        case 4: {
            if (ch.strobe) { ch.strobe = false; set_output(idx, true); }
            if (ch.load_pending) {
                ch.load_pending = false;
                ch.ce = int32_t(ch.cr);
                ch.null_count = false;
                ch.counting = true;
                ch.fired = false;
                return;
            }
            if (!ch.counting || !ch.gate) return;
            ch.ce = ch.ce == 0 ? (ch.bcd ? 9999 : 0xFFFF) : ch.ce - 1;
            if (ch.ce == 0 && !ch.fired) {
                ch.fired = true;
                if (ch.mode == 0) {
                    set_output(idx, true);
                } else {
                    set_output(idx, false);
                    ch.strobe = true;
                }
            }
            return;
        }
        case 1:
        case 5: {
            if (ch.strobe) { ch.strobe = false; set_output(idx, true); }
            if (ch.trigger) {
                ch.trigger = false;
                ch.ce = int32_t(ch.cr);
                ch.null_count = false;
                ch.counting = true;
                ch.fired = false;
                if (ch.mode == 1) set_output(idx, false);
                return;
            }
            if (!ch.counting) return;
            ch.ce = ch.ce == 0 ? (ch.bcd ? 9999 : 0xFFFF) : ch.ce - 1;
            if (ch.ce == 0 && !ch.fired) {
                ch.fired = true;
                if (ch.mode == 1) {
                    set_output(idx, true);
                } else {
                    set_output(idx, false);
                    ch.strobe = true;
                }
            }
            return;
        }
        case 2: {
            if (!ch.gate) return;
            if (ch.load_pending) {
                ch.load_pending = false;
                ch.ce = int32_t(ch.cr);
                ch.null_count = false;
                ch.counting = true;
                set_output(idx, true);
                return;
            }
            if (!ch.counting) return;
            if (ch.ce <= 1) {  // the low clock ends: reload and raise OUT
                ch.ce = int32_t(ch.cr);
                ch.null_count = false;
                set_output(idx, true);
            } else if (--ch.ce == 1) {
                set_output(idx, false);
            }
            return;
        }
        case 3: {
            if (!ch.gate) return;
            // Odd counts load N-1, so OUT is high (N+1)/2 clocks and low (N-1)/2.
            int32_t reload = int32_t(ch.cr & 1 ? ch.cr - 1 : ch.cr);
            if (ch.load_pending) {
                ch.load_pending = false;
                ch.ce = reload;
                ch.null_count = false;
                ch.counting = true;
                ch.odd_extra = false;
                set_output(idx, true);
                return;
            }
            if (!ch.counting) return;
            if (ch.odd_extra) {
                ch.odd_extra = false;
                ch.ce = reload;
                ch.null_count = false;
                set_output(idx, false);
                return;
            }
            ch.ce -= 2;
            if (ch.ce > 0) return;
            if (ch.output && (ch.cr & 1)) {
                ch.ce = 0;
                ch.odd_extra = true;
                return;
            }
            ch.ce = reload;
            ch.null_count = false;
            set_output(idx, !ch.output);
            return;
        }
        default:
            return;
    }
}

int Pit8253::step_counts(int count) {
    for (int i = 0; i < count; ++i) {
        for (int c = 0; c < 3; ++c) step_channel(c);
    }
    int rises = ch0_rises_;
    ch0_rises_ = 0;
    return rises;
}

}  // namespace pc486
