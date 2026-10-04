// Real-time clock + battery-backed CMOS RAM: an MC146818-compatible part
// with 128 bytes, the DS12887 class a period 486 board carries.
//
// Port 0x70 (write-only address latch, bit 7 doubles as the NMI mask) /
// 0x71 (data). Registers 0x00-0x09 are the time-of-day/alarm/calendar
// fields, 0x0A-0x0D the chip's control/status registers (A: update-in-
// progress, divider and rate select; B: mode control; C: interrupt flags,
// cleared on read; D: valid-RAM flag), and 0x0E-0x7F battery-backed bytes.
// On an AT-class board 0x10-0x2F hold the BIOS Setup configuration and
// 0x32 the century.
//
// The clock runs from its own 32.768 kHz crystal, so it advances in guest
// time (CPU cycles over the CPU clock), independent of Turbo. Once a second
// it runs an update cycle: UIP rises 244 us before it, the registers roll
// over 1984 us later, then the update-ended and alarm flags are checked.
// The periodic flag follows register A's rate select. Any enabled flag
// drives IRQ8 until register C is read. Reference: Motorola MC146818A data
// sheet ("Update Cycle", "Interrupts", Table 3 periodic rates); Dallas
// DS12887 data sheet for the 128-byte map.
//
// Not modelled: daylight-saving adjustment (register B DSE), and the
// square-wave output, which an AT board leaves unconnected.
#ifndef PC486_CMOS_RTC_H
#define PC486_CMOS_RTC_H

#include <cstdint>

namespace pc486 {

class CmosRtc {
public:
    CmosRtc() { reset(); }

    // Power-on state of a part whose battery has been keeping time: the
    // calendar is whatever set_time() last loaded (1994-01-01 00:00:00
    // until a host sets it), the divider running, BCD and 24-hour mode.
    void reset();

    bool owns(uint16_t port) const { return port == 0x70 || port == 0x71; }
    uint8_t in(uint16_t port);
    void out(uint16_t port, uint8_t v);

    // Host-side: preload a register directly (BIOS-expected config bytes --
    // memory size, drive types, equipment byte, checksum) without going
    // through the CPU-facing address/data protocol.
    void poke(uint8_t reg, uint8_t v) { ram_[reg & 0x7F] = v; }
    uint8_t peek(uint8_t reg) const { return ram_[reg & 0x7F]; }

    // Sets the calendar the way BIOS Setup would, in the register format
    // register B currently selects. year is the full year (century goes to
    // 0x32), weekday 1-7 with 1 = Sunday.
    void set_time(int year, int month, int day, int hour, int minute, int second, int weekday);

    // Advances the clock against the CPU's running cycle count (absolute,
    // like Pit8253::tick) at the given CPU clock rate.
    void tick(uint64_t cpu_cycles, double cpu_hz) {
        uint64_t d = cpu_cycles - prev_cycles_;
        prev_cycles_ = cpu_cycles;
        if (d == 0) return;
        if (cpu_hz != ratio_hz_) {
            ratio_hz_ = cpu_hz;
            osc_per_cycle_ = kOscHz / cpu_hz;
        }
        credit_ += double(d) * osc_per_cycle_;
        while (credit_ >= next_event_) advance();
    }

    // IRQ8: high while an enabled flag in register C is set.
    bool irq_pending() const { return (ram_[0x0C] & 0x80) != 0; }

    bool nmi_masked() const { return nmi_masked_; }

private:
    static constexpr double kOscHz = 32768.0;
    // 244 us of UIP warning, then a 1984 us update, in oscillator ticks.
    static constexpr double kUipLead = 8.0;
    static constexpr double kUpdateLen = 65.0;

    void advance();
    void run_update();
    void raise_flag(uint8_t flag);
    double periodic_ticks() const;
    bool running() const { return ((ram_[0x0A] >> 4) & 7) == 2 && !(ram_[0x0B] & 0x80); }

    uint8_t ram_[128] = {};
    uint8_t addr_ = 0;
    bool nmi_masked_ = false;
    bool uip_ = false;

    uint64_t prev_cycles_ = 0;
    double ratio_hz_ = 0.0;
    double osc_per_cycle_ = 0.0;
    double credit_ = 0.0;        // oscillator ticks since power-on
    double next_event_ = 0.0;    // earliest credit_ at which advance() has work
    double second_start_ = 0.0;  // credit_ at which the current second began
    double next_periodic_ = 0.0;
};

}  // namespace pc486

#endif  // PC486_CMOS_RTC_H
