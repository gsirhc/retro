#include "cmos_rtc.h"

#include <algorithm>

namespace pc486 {

namespace {

constexpr uint8_t kRegA = 0x0A, kRegB = 0x0B, kRegC = 0x0C, kRegD = 0x0D;
constexpr uint8_t kPF = 0x40, kAF = 0x20, kUF = 0x10, kIRQF = 0x80;
constexpr uint8_t kSet = 0x80, kBinary = 0x04, kHour24 = 0x02;
constexpr double kSecond = 32768.0;

int from_bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
uint8_t to_bcd(int n) { return uint8_t(((n / 10) << 4) | (n % 10)); }

}  // namespace

void CmosRtc::reset() {
    for (auto &b : ram_) b = 0;
    addr_ = 0;
    nmi_masked_ = false;
    uip_ = false;
    ram_[kRegA] = 0x26;  // 32.768 kHz time base, 1024 Hz periodic rate
    ram_[kRegB] = kHour24;
    ram_[kRegD] = 0x80;  // VRT: battery and RAM valid
    set_time(1994, 1, 1, 0, 0, 0, 7);
    credit_ = 0.0;
    second_start_ = 0.0;
    next_periodic_ = periodic_ticks();
    next_event_ = 0.0;
}

void CmosRtc::set_time(int year, int month, int day, int hour, int minute, int second, int weekday) {
    const bool bin = (ram_[kRegB] & kBinary) != 0;
    auto enc = [bin](int n) { return bin ? uint8_t(n) : to_bcd(n); };
    ram_[0x00] = enc(second);
    ram_[0x02] = enc(minute);
    if (ram_[kRegB] & kHour24) {
        ram_[0x04] = enc(hour);
    } else {
        int h12 = hour % 12 == 0 ? 12 : hour % 12;
        ram_[0x04] = uint8_t(enc(h12) | (hour >= 12 ? 0x80 : 0x00));
    }
    ram_[0x06] = enc(weekday);
    ram_[0x07] = enc(day);
    ram_[0x08] = enc(month);
    ram_[0x09] = enc(year % 100);
    ram_[0x32] = to_bcd(year / 100);  // the AT's century byte, always BCD
}

uint8_t CmosRtc::in(uint16_t port) {
    if (port == 0x70) return 0xFF;  // address latch is write-only on real hardware
    uint8_t reg = addr_ & 0x7F;
    if (reg == kRegA) return uint8_t((ram_[kRegA] & 0x7F) | (uip_ ? 0x80 : 0x00));
    if (reg == kRegC) {
        uint8_t v = ram_[kRegC];
        ram_[kRegC] = 0;  // reading C clears every flag and drops IRQ8
        return v;
    }
    return ram_[reg];
}

void CmosRtc::out(uint16_t port, uint8_t v) {
    if (port == 0x70) {
        addr_ = v & 0x7F;
        nmi_masked_ = (v & 0x80) != 0;
        return;
    }
    uint8_t reg = addr_ & 0x7F;
    if (reg == kRegC || reg == kRegD) return;  // read-only
    if (reg == kRegA) {
        bool was = running();
        ram_[kRegA] = uint8_t(v & 0x7F);  // UIP is read-only
        // First update comes half a second after release (MC146818A "Divider Control")
        if (!was && running()) second_start_ = credit_ - kSecond / 2;
        next_periodic_ = credit_ + periodic_ticks();
        next_event_ = credit_;
        return;
    }
    if (reg == kRegB) {
        bool was = running();
        if (v & kSet) v = uint8_t(v & ~kUF);  // SET clears UIE
        ram_[kRegB] = v;
        if (!was && running()) second_start_ = credit_ - kSecond / 2;
        if (v & kSet) uip_ = false;
        uint8_t c = ram_[kRegC];
        ram_[kRegC] = uint8_t((c & 0x70) | ((c & v & 0x70) ? kIRQF : 0x00));
        next_event_ = credit_;
        return;
    }
    ram_[reg] = v;
}

double CmosRtc::periodic_ticks() const {
    // MC146818A Table 3 at 32.768 kHz: RS=1,2 give 256,128 Hz; RS=3-15 give
    // 8192 Hz halving each step; RS=0 off
    int rs = ram_[kRegA] & 0x0F;
    if (rs == 0) return 0.0;
    if (rs == 1) return 128.0;
    if (rs == 2) return 256.0;
    return double(1 << (rs - 1));
}

void CmosRtc::raise_flag(uint8_t flag) {
    ram_[kRegC] |= flag;
    if (ram_[kRegB] & flag) ram_[kRegC] |= kIRQF;
}

void CmosRtc::advance() {
    const bool osc = ((ram_[kRegA] >> 4) & 7) == 2;
    double next = credit_ + kSecond;

    double period = periodic_ticks();
    if (osc && period > 0.0) {
        if (credit_ >= next_periodic_) {
            raise_flag(kPF);
            next_periodic_ += period;
            if (next_periodic_ <= credit_) next_periodic_ = credit_ + period;
        }
        next = std::min(next, next_periodic_);
    } else {
        next_periodic_ = credit_ + period;
    }

    if (running()) {
        const double update_at = second_start_ + kSecond;
        const double uip_at = update_at - kUipLead - kUpdateLen;
        if (credit_ >= update_at) {
            run_update();
            uip_ = false;
            second_start_ = update_at;
            next = std::min(next, second_start_ + kSecond - kUipLead - kUpdateLen);
        } else if (credit_ >= uip_at) {
            uip_ = true;
            next = std::min(next, update_at);
        } else {
            next = std::min(next, uip_at);
        }
    } else {
        second_start_ = credit_;
        uip_ = false;
    }
    next_event_ = next;
}

void CmosRtc::run_update() {
    const bool bin = (ram_[kRegB] & kBinary) != 0;
    const bool h24 = (ram_[kRegB] & kHour24) != 0;
    auto dec = [bin](uint8_t v) { return bin ? int(v) : from_bcd(v); };
    auto enc = [bin](int n) { return bin ? uint8_t(n) : to_bcd(n); };

    int sec = dec(ram_[0x00]), min = dec(ram_[0x02]);
    int hour;
    if (h24) {
        hour = dec(ram_[0x04]);
    } else {
        hour = dec(uint8_t(ram_[0x04] & 0x7F)) % 12 + ((ram_[0x04] & 0x80) ? 12 : 0);
    }
    int wday = dec(ram_[0x06]), day = dec(ram_[0x07]), mon = dec(ram_[0x08]), year = dec(ram_[0x09]);

    if (++sec >= 60) {
        sec = 0;
        if (++min >= 60) {
            min = 0;
            if (++hour >= 24) {
                hour = 0;
                wday = wday >= 7 ? 1 : wday + 1;
                // Every fourth year, no century exception (MC146818A "Time, Calendar and Alarm")
                static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
                int dim = (mon >= 1 && mon <= 12) ? kDays[mon - 1] : 31;
                if (mon == 2 && year % 4 == 0) dim = 29;
                if (++day > dim) {
                    day = 1;
                    if (++mon > 12) {
                        mon = 1;
                        year = (year + 1) % 100;
                    }
                }
            }
        }
    }

    ram_[0x00] = enc(sec);
    ram_[0x02] = enc(min);
    if (h24) {
        ram_[0x04] = enc(hour);
    } else {
        int h12 = hour % 12 == 0 ? 12 : hour % 12;
        ram_[0x04] = uint8_t(enc(h12) | (hour >= 12 ? 0x80 : 0x00));
    }
    ram_[0x06] = enc(wday);
    ram_[0x07] = enc(day);
    ram_[0x08] = enc(mon);
    ram_[0x09] = enc(year);

    raise_flag(kUF);
    // Alarm byte with top two bits set matches anything
    auto match = [](uint8_t alarm, uint8_t now) { return (alarm & 0xC0) == 0xC0 || alarm == now; };
    if (match(ram_[0x01], ram_[0x00]) && match(ram_[0x03], ram_[0x02]) && match(ram_[0x05], ram_[0x04])) {
        raise_flag(kAF);
    }
}

}  // namespace pc486
