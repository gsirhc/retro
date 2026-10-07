// Intel 8254 PIT. Three 16-bit down-counters clocked at 1.193182 MHz (14.31818
// MHz / 12), independent of the CPU clock. Channel 0 drives IRQ0 (BIOS divisor 0 =
// 65536 gives the 18.206 Hz DOS tick). Channel 1 refresh is not modeled. Channel 2,
// gated by port 0x61 bit 0, drives the speaker (pcspeaker.h). Gates 0 and 1 are tied high.
// Ports 0x40-0x42 data, 0x43 control. All six modes, BCD, counter latch and
// read-back follow Intel 8254 data sheet (231164), "Mode Definitions" and
// "Read-Back Command". A written count reaches the counting element on the
// next CLK, so mode 0's OUT rises N+1 clocks after the write.
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

    // Advances the PIT clock against the CPU's absolute cycle count (like
    // CassetteACR::tick). Returns channel 0's rising edges for IRQ0. Inline: at
    // 66 MHz most calls have no whole count to step; step_counts() does the rest.
    int tick(uint64_t cpu_cycles, double cpu_hz) {
        uint64_t d = cpu_cycles - prev_cycles_;
        prev_cycles_ = cpu_cycles;
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

    // CPU cycles until this counter can next advance (Chipset::tick). Floored,
    // so never later than the real count.
    uint64_t cycles_to_next_count() const {
        if (cpu_per_pit_count_ <= 0.0) return 0;
        double need = (1.0 - pit_credit_) * cpu_per_pit_count_;
        return need > 0.0 ? uint64_t(need) : 0;
    }

    // Port 0x61 bit 0 gates channel 2. Gate low pauses modes 0, 2, 3, 4 and
    // forces OUT high in modes 2 and 3 (pcspeaker.h direct-toggle relies on it);
    // a rising edge triggers modes 1, 5 and restarts 2, 3 from a full count.
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
    // Memoized ticks-per-CPU-cycle for the last cpu_hz tick() saw
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
