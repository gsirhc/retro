// Top-level machine: wires the 80286 core to the chipset and drives both from
// run_cycles(). Keeps its own monotonic total_cycles_ because a CPU reset
// (the 8042 real-mode-return trick) zeroes cpu.cycles, and device ticks such
// as the PIT must keep running across it.
#ifndef IBMPCAT_MACHINE_H
#define IBMPCAT_MACHINE_H

#include "chipset.h"
#include "cpu80286.h"

#include <cstdint>

namespace ibmpcat {

class Machine {
public:
    // Powering on: the CPU goes to its reset vector (Cpu's default members do
    // not equal it) and the factory CMOS configuration is already set.
    Machine() : cpu(chipset.make_bus()) {
        cpu.reset();
        start_refresh();
        // The BIOS and video BIOS stand-ins use 386 opcodes (IBM_PCAT_REVIEW.md §7).
        cpu.firmware_at = [this](uint32_t addr) { return chipset.is_rom(addr); };
        // One PIT clock, so a long REP doesn't hold off IRQ0.
        cpu.rep_yield_cycles = uint32_t(kCpuHz / 1193182.0);
        configure_factory_cmos();
    }

    void reset() {
        chipset.reset();
        start_refresh();
        cpu.reset();
        // CMOS is battery-backed and survives reset; chipset.reset() leaves it alone.
    }

    // Seeds CMOS as a factory-configured 5170-339 would hold it: base memory,
    // floppy types, equipment byte and boot-device sequence (IBM_PCAT_REVIEW.md §8).
    // Called once at construction.
    void configure_factory_cmos();

    // Channel 1 mode 2, count 18, as IBM's POST does (IBM PC/AT Technical
    // Reference, BIOS listing); the stand-in BIOS never starts refresh.
    void start_refresh() {
        chipset.pit.out(0x43, 0x54);
        chipset.pit.out(0x41, 0x12);
    }

    // Runs instructions until at least `cycles` more CPU cycles have elapsed at
    // the fixed 8 MHz, servicing 8042 reset requests and interrupts between instructions.
    void run_cycles(int64_t cycles);

    uint64_t total_cycles() const { return total_cycles_; }

    // Declared first: cpu's constructor needs chipset.make_bus().
    Chipset chipset;
    cpu80286::Cpu cpu;

    static constexpr double kCpuHz = 8000000.0;  // real 8 MHz 80286

private:
    uint64_t total_cycles_ = 0;
};

}  // namespace ibmpcat

#endif  // IBMPCAT_MACHINE_H
