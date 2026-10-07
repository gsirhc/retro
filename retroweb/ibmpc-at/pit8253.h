// Intel 8253/8254 Programmable Interval Timer. Three 16-bit down-counters
// clocked at 1.193182 MHz (14.31818 MHz / 12), independent of the CPU clock.
// Channel 0 drives IRQ0: the BIOS programs divisor 0 = 65536, giving DOS's
// 18.206 Hz tick. Channel 1 (DRAM refresh) is not modeled. Channel 2, gated by
// port 0x61 bit 0, drives the speaker (pcspeaker.h).
//
// Ports 0x40-0x42 are channels 0-2, 0x43 is the control word. Intel 8253/8254
// data sheet, "Programming the 8253".
//
// Every mode is modeled as a symmetric toggle with a full period every
// `reload` clocks (frequency 1193182/reload), not each mode's real waveform
// (Mode 2 is really a one-clock low pulse). The edge rate is what BIOS/DOS timing uses.
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

    // Advances the PIT clock against the absolute CPU cycle count. Returns how
    // many times channel 0's output rose, for the chipset to pulse IRQ0.
    int tick(uint64_t cpu_cycles, double cpu_hz);

    // Port 0x61 bit 0 gates channel 2. Mode 3 hardware freezes the counter and
    // forces the output high when the gate goes low, giving the direct-toggle
    // speaker technique (pcspeaker.h) a high baseline. The gate's rising edge
    // reloads the counter, restarting the period.
    void set_gate2(bool level);
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
        // Counter-latch command (control word with access==0).
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
