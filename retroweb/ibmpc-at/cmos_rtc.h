// Motorola MC146818 RTC + battery-backed CMOS RAM.
//
// Port 0x70 is a write-only address latch (bit 7 masks NMI), 0x71 is data.
// 0x00-0x09 time/alarm/calendar, 0x0A-0x0D control/status (A: UIP + rate,
// B: mode, C: interrupt flags cleared on read, D: battery good), 0x0E-0x3F
// BIOS Setup config bytes (checksum at 0x2E/0x2F).
//
// No live clock. UIP (register A bit 7) always reads 0 so BIOS wait loops never hang.
#ifndef IBMPCAT_CMOS_RTC_H
#define IBMPCAT_CMOS_RTC_H

#include <cstdint>

namespace ibmpcat {

class CmosRtc {
public:
    void reset();

    bool owns(uint16_t port) const { return port == 0x70 || port == 0x71; }
    uint8_t in(uint16_t port) const;
    void out(uint16_t port, uint8_t v);

    // Host-side preload that bypasses the port protocol.
    void poke(uint8_t reg, uint8_t v) { ram_[reg & 0x3F] = v; }
    uint8_t peek(uint8_t reg) const { return ram_[reg & 0x3F]; }

    bool nmi_masked() const { return nmi_masked_; }

private:
    uint8_t ram_[64] = {};
    uint8_t addr_ = 0;
    bool nmi_masked_ = false;
};

}  // namespace ibmpcat

#endif  // IBMPCAT_CMOS_RTC_H
