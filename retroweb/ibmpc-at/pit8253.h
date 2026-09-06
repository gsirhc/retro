// Intel 8253/8254 Programmable Interval Timer.
//
// Three independent 16-bit down-counters clocked at a fixed 1.193182 MHz
// (the 14.31818 MHz crystal divided by 12) -- a rate wired straight into
// the AT's motherboard, completely independent of the CPU's own clock.
// Channel 0's output drives PIC IRQ0 (the classic ~18.2 Hz DOS timer tick,
// from the BIOS programming it with a divisor of 0 = 65536: 1193182/65536
// = 18.206 Hz -- this specific fact is why DOS's `TIMER_TICK` runs at that
// rate, and is worth preserving exactly). Channel 1 historically paced
// DRAM refresh requests (not modeled -- this emulator's RAM doesn't need
// refreshing). Channel 2's output, gated by port 0x61 bit 0, drives the PC
// speaker (see pcspeaker.h, a later phase).
//
// Ports: 0x40/0x41/0x42 = channel 0/1/2 data, 0x43 = control word.
// Reference: Intel 8253/8254 data sheet, "Programming the 8253".
//
// Scope/simplification: this core models a uniform symmetric-toggle output
// for every mode (a full high/low period every `reload` PIT clocks, i.e.
// output frequency = 1193182/reload) rather than each mode's exact
// waveform shape (real Mode 2's output is a single-clock-wide low pulse
// per `reload` counts, quite different from a 50% duty square wave). What
// matters for BIOS/DOS timing is the *edge rate*, which this reproduces
// correctly; see IBM_PCAT_REVIEW.md.
#ifndef IBMPCAT_PIT8253_H
#define IBMPCAT_PIT8253_H

#include <cstdint>

namespace ibmpcat {

class Pit8253 {
public:
    Pit8253() { reset(); }

    void reset();

    bool owns(uint16_t port) const { return port >= 0x40 && port <= 0x43; }
    uint8_t in(uint16_t port);
    void out(uint16_t port, uint8_t v);

    // Advance the PIT's own 1.193182 MHz clock against the CPU's running
    // cycle count (absolute, like CassetteACR::tick -- the delta since the
    // last call is computed internally) at the given CPU clock rate.
    // Returns how many times channel 0's output rose during this call, so
    // the chipset can pulse PIC IRQ0 that many times.
    int tick(uint64_t cpu_cycles, double cpu_hz);

    // Port 0x61 bit 0 gates channel 2 (the speaker channel) -- gate low
    // freezes its counter, matching real hardware.
    void set_gate2(bool level) { ch_[2].gate = level; }
    bool channel2_output() const { return ch_[2].output; }

private:
    struct Channel {
        uint16_t reload = 0;         // programmed divisor (0 means 65536)
        uint16_t toggle_period = 0;  // reload/2 (min 1), the actual per-toggle countdown
        uint16_t counter = 0;
        int access = 0;              // 1=LSB only, 2=MSB only, 3=LSB then MSB
        bool msb_pending = false;    // access==3: true after the LSB half has been written/read
        uint8_t pending_lsb = 0;
        bool output = true;
        bool gate = true;
        bool armed = false;
        bool just_rose = false;
        // BIOS occasionally reads the counter back (e.g. diagnostics) via
        // the counter-latch command (control word with access==0).
        bool latched = false;
        uint16_t latch_value = 0;
        bool latch_msb_pending = false;
    };
    Channel ch_[3];

    double pit_credit_ = 0.0;
    uint64_t prev_cycles_ = 0;
    static constexpr double PIT_HZ = 1193182.0;

    void set_reload(int idx, uint16_t v);
    void step_channel(int idx);
};

}  // namespace ibmpcat

#endif  // IBMPCAT_PIT8253_H
