// Top-level machine: wires the 80486 core to the chipset and drives both from
// run_cycles(). Same total_cycles_-vs-cpu.cycles convention as ibmpc-at/machine.h.
#ifndef PC486_MACHINE_H
#define PC486_MACHINE_H

#include "chipset.h"
#include "cpu80486.h"

#include <cstdint>

namespace pc486 {

class Machine {
public:
    // Construction models power-on: CPU at the reset vector, factory CMOS burned in
    Machine() : cpu(chipset.make_bus()) {
        cpu.timing = &cache;
        chipset.timing = &cache;
        chipset.timing_clock = &cpu.cycles;
        reset_cpu();
        set_rep_yield();
        configure_factory_cmos();
    }

    void reset() {
        chipset.reset();
        reset_cpu();
        // Reset pulses RESET to CPU and chipset; battery-backed CMOS is kept
    }

    // Seeds CMOS with the factory configuration (memory, floppy, equipment, boot
    // order, fixed-disk geometry). Called once at construction, not on reset().
    void configure_factory_cmos();

    // Runs until at least `cycles` more CPU cycles elapse (CPU internal clock,
    // 66 MHz). Wall-clock pacing is the front end's job.
    void run_cycles(int64_t cycles);

    uint64_t total_cycles() const { return total_cycles_; }

    // CPU clock, passed to devices that pace in CPU cycles (chipset.tick(), set_cpu_hz())
    static constexpr double kCpuHz = 66000000.0;
    double cpu_hz() const { return cpu_hz_; }
    void set_cpu_hz(double hz);
    // Turbo off on the 471 leaves the CPU clock alone: the chipset holds the CPU
    // off the bus 4us of every 12us (reg 58h bit 4, reset default)
    static constexpr double kDeturboPeriodSec = 12e-6;
    static constexpr double kDeturboHoldSec = 4e-6;
    bool turbo() const { return turbo_; }
    void set_turbo(bool on) {
        turbo_ = on;
        cache.set_deturbo(uint32_t(kDeturboPeriodSec * cpu_hz_), on ? 0u : uint32_t(kDeturboHoldSec * cpu_hz_));
    }

    // Diagnostic hook before each instruction, CPU at its CS:EIP. Function pointer
    // plus ctx, not std::function: this loop runs 66M times a second (§8). Null
    // costs about 1% (PC486_REVIEW.md §5.4, §6.5, §9; disks/boom_run_check.cpp).
    void (*on_instruction)(void *ctx, Machine &m) = nullptr;
    void *on_instruction_ctx = nullptr;

    // Declared first: cpu's constructor needs chipset.make_bus()
    Chipset chipset;
    Cache486 cache;
    cpu80486::Cpu cpu;

private:
    uint64_t total_cycles_ = 0;
    // One 1.193182 MHz PIT count, the finest step at which a device can raise
    // an IRQ, so a long REP yields no later than a 486 would take it
    void set_rep_yield() { cpu.rep_yield_cycles = uint32_t(cpu_hz_ / 1193182.0); }
    double cpu_hz_ = kCpuHz;
    bool turbo_ = true;
    void service_kbc_reset() {
        if (!chipset.kbc.reset_requested()) return;
        chipset.kbc.clear_reset_request();
        reset_cpu();
    }
    // The CPU leaves RESET with L1 off (CR0.CD and NW set). Period AMI/Award
    // BIOSes enable it in POST; the Bochs stand-in doesn't, so the board does it on reset.
    void reset_cpu() {
        cpu.reset();
        cpu.enable_cache();
    }
};

}  // namespace pc486

#endif  // PC486_MACHINE_H
