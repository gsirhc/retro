// Intel 8254 Programmable Interval Timer.
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
// speaker (see pcspeaker.h). Gates 0 and 1 are tied high on an AT board.
//
// Ports: 0x40/0x41/0x42 = channel 0/1/2 data, 0x43 = control word.
// All six counter modes, BCD counting, the counter-latch command and the
// 8254 read-back command (count and status) follow Intel's 8254 data sheet
// (order no. 231164), "Mode Definitions" and "Read-Back Command". A count
// written to the counter register reaches the counting element on the next
// CLK pulse, so mode 0's OUT rises N+1 clocks after the write.
#ifndef PC486_PIT8253_H
#define PC486_PIT8253_H

#include <cstdint>

namespace pc486 {

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
    // Inline: at 66 MHz the PIT advances one count every ~55 CPU cycles, so
    // the overwhelming majority of the per-instruction calls have no whole
    // count to step and end here. step_counts() carries the ones that do.
    int tick(uint64_t cpu_cycles, double cpu_hz) {
        uint64_t d = cpu_cycles - prev_cycles_;
        prev_cycles_ = cpu_cycles;
        // cpu_hz can change at run time (front-panel Turbo drops the DX2 from
    // 66 MHz to 33 MHz). Memoize the ratio so the divide is not redone on
    // every instruction. See PC486_REVIEW.md §8.
    if (cpu_hz != ratio_hz_) {
            ratio_hz_ = cpu_hz;
            pit_per_cpu_cycle_ = PIT_HZ / cpu_hz;
            cpu_per_pit_count_ = cpu_hz / PIT_HZ;
        }
        pit_credit_ += double(d) * pit_per_cpu_cycle_;
        int whole = int(pit_credit_);
        if (whole == 0) return 0;
        pit_credit_ -= whole;
        return step_counts(whole);
    }

    // CPU cycles from now until this counter can next advance. The 1.193182
    // MHz clock is the fastest thing in the chipset, so nothing any device
    // here does can become observable sooner than this -- see Chipset::tick.
    // Floored, so the answer is never later than the real count.
    uint64_t cycles_to_next_count() const {
        if (cpu_per_pit_count_ <= 0.0) return 0;
        double need = (1.0 - pit_credit_) * cpu_per_pit_count_;
        return need > 0.0 ? uint64_t(need) : 0;
    }

    // Port 0x61 bit 0 gates channel 2. Gate low pauses modes 0, 2, 3 and 4
    // and forces OUT high in modes 2 and 3, which pcspeaker.h's direct-toggle
    // playback relies on; a rising edge triggers modes 1 and 5 and restarts
    // modes 2 and 3 from a full count.
    void set_gate2(bool level);
    bool channel2_output() const { return ch_[2].output; }

private:
    struct Channel {
        uint8_t control = 0;         // last control word's RW/M/BCD bits, for the status byte
        int mode = 0;                // 0-5 (6 and 7 alias 2 and 3)
        int access = 0;              // 1=LSB only, 2=MSB only, 3=LSB then MSB
        bool bcd = false;
        uint32_t cr = 0;             // count register, decoded: 1..65536 (or 1..10000 BCD)
        bool cr_written = false;
        int32_t ce = 0;              // counting element; 65536 (10000) reads back as 0
        bool write_msb_pending = false;
        uint8_t pending_lsb = 0;
        bool read_msb_pending = false;
        bool load_pending = false;   // CR -> CE on the next CLK
        bool trigger = false;        // gate rising edge, acted on at the next CLK
        bool counting = false;
        bool null_count = true;
        bool fired = false;          // modes 1, 4, 5: terminal count already reached
        bool strobe = false;         // modes 4, 5: OUT is in its one-clock low pulse
        bool odd_extra = false;      // mode 3, odd count: one more CLK high
        bool output = true;
        bool gate = true;
        bool count_latched = false;
        uint16_t latch_value = 0;
        bool status_latched = false;
        uint8_t status = 0;
    };
    Channel ch_[3];

    double pit_credit_ = 0.0;
    uint64_t prev_cycles_ = 0;
    static constexpr double PIT_HZ = 1193182.0;
    // Memoized PIT-ticks-per-CPU-cycle (and its reciprocal) for the last
    // cpu_hz tick() was given.
    double ratio_hz_ = 0.0, pit_per_cpu_cycle_ = 0.0, cpu_per_pit_count_ = 0.0;

    int ch0_rises_ = 0;  // channel 0 rising edges not yet handed to tick()'s caller

    void set_output(int idx, bool level);
    void write_count(int idx, uint32_t raw);
    void latch_count(Channel &ch);
    void latch_status(Channel &ch);
    void step_channel(int idx);
    // Steps all three channels `count` times, returning channel 0's rising edges.
    int step_counts(int count);
};

}  // namespace pc486

#endif  // PC486_PIT8253_H
