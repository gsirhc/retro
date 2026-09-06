#include "cmos_rtc.h"

namespace ibmpcat {

void CmosRtc::reset() {
    for (auto &b : ram_) b = 0;
    addr_ = 0;
    nmi_masked_ = false;
    ram_[0x0A] = 0x26;  // rate select defaults, UIP forced 0 regardless on read (see in())
    ram_[0x0B] = 0x02;  // 24-hour mode
    ram_[0x0D] = 0x80;  // battery/valid-RAM flag good
}

uint8_t CmosRtc::in(uint16_t port) const {
    if (port == 0x70) return 0xFF;  // address latch is write-only on real hardware
    uint8_t reg = addr_ & 0x3F;
    if (reg == 0x0A) return uint8_t(ram_[reg] & 0x7F);  // UIP (bit7) never asserts -- see file header
    if (reg == 0x0C) {
        uint8_t v = ram_[reg];
        const_cast<CmosRtc *>(this)->ram_[reg] = 0;  // reading register C clears its interrupt flags, real chip behavior
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
    ram_[addr_ & 0x3F] = v;
}

}  // namespace ibmpcat
