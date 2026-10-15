// Motorola MC146818A RTC with 64 bytes of battery-backed CMOS RAM (the 5170),
// or a 128-byte DS12887-class part (486 boards); `ram_bytes` picks which.
// Port 0x70 is a write-only address latch (bit 7 = NMI mask), 0x71 data.
// 0x10-0x2F hold the configuration and 0x32 the century (IBM PC/AT Technical
// Reference, CMOS RAM map). Runs from its own 32.768 kHz crystal in guest time.
// Each second UIP rises 244 us before the update, registers roll 1984 us
// later. Any enabled flag drives IRQ8 until register C is read.
// MC146818A data sheet ("Update Cycle", "Interrupts", Table 3); Dallas DS12887
// for the 128-byte map. No DSE or SQW.
#ifndef SHARED_PC_CMOS_RTC_H
#define SHARED_PC_CMOS_RTC_H

#include <cstdint>

namespace pc {

class CmosRtc {
public:
    // The calendar powers on at 1 January of `year`, which falls on `weekday` (1 = Sunday).
    CmosRtc(int ram_bytes, int year, int weekday)
        : mask_(uint8_t(ram_bytes - 1)), year_(year), weekday_(weekday) { reset(); }

    // Power-on state: divider running, BCD and 24-hour mode.
    void reset();

    bool owns(uint16_t port) const { return port == 0x70 || port == 0x71; }
    uint8_t in(uint16_t port);
    void out(uint16_t port, uint8_t v);

    // Host-side preload of BIOS config bytes, bypassing the address/data protocol
    void poke(uint8_t reg, uint8_t v) { ram_[reg & mask_] = v; }
    uint8_t peek(uint8_t reg) const { return ram_[reg & mask_]; }

    // Sets the calendar in the format register B selects. year is the full
    // year (century goes to 0x32), weekday 1-7 with 1 = Sunday.
    void set_time(int year, int month, int day, int hour, int minute, int second, int weekday);

    // Advances against the CPU's absolute cycle count (like Pit8253::tick)
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
    // 244 us UIP warning, then 1984 us update, in oscillator ticks
    static constexpr double kUipLead = 8.0;
    static constexpr double kUpdateLen = 65.0;

    void advance();
    void run_update();
    void raise_flag(uint8_t flag);
    double periodic_ticks() const;
    bool running() const { return ((ram_[0x0A] >> 4) & 7) == 2 && !(ram_[0x0B] & 0x80); }

    uint8_t ram_[128] = {};
    uint8_t mask_;
    int year_, weekday_;
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

}  // namespace pc

#endif  // SHARED_PC_CMOS_RTC_H
