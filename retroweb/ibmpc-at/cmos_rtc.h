// Motorola MC146818 real-time clock + battery-backed CMOS configuration RAM.
//
// Port 0x70 (write-only address latch, bit 7 doubles as the NMI mask) /
// 0x71 (data). 64 bytes total: registers 0x00-0x09 are the time-of-day/
// alarm/calendar fields, 0x0A-0x0D are the chip's own control/status
// registers (A: update-in-progress + rate select; B: mode control; C:
// interrupt-flag, cleared on read; D: valid-RAM/battery-good flag), and
// 0x0E-0x3F are genuine general-purpose battery-backed bytes -- on a real
// AT this is where BIOS Setup stores floppy/fixed-disk types, base/
// extended memory size, and an equipment byte, checksummed at 0x2E/0x2F.
//
// Scope: this core does not run a live ticking clock (DOS/BIOS reads the
// time-of-day registers once during POST; nothing in this machine's early
// phases depends on it advancing). Register A's update-in-progress bit
// (bit 7) always reads 0 so BIOS's "wait for UIP to clear" POST loop can
// never hang. Reference: Motorola MC146818 data sheet; the AT-specific
// CMOS byte layout is documented in the IBM 5170 Technical Reference and
// (once Phase 2's BIOS is wired up) whatever byte map that BIOS expects --
// poke()/peek() let the embedding Machine pre-seed those bytes directly,
// bypassing the port protocol, the way a real factory-configured AT would
// already have them set from Setup.
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

    // Host-side: preload a register directly (BIOS-expected config bytes --
    // memory size, drive types, equipment byte, checksum) without going
    // through the CPU-facing address/data protocol.
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
