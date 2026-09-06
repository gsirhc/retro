// Top-level machine: wires the 80286 core to the AT chipset glue layer and
// drives both from a single run_cycles() call, mirroring the
// altair8800/cg-oac-6502 Machine convention (runCycles-per-frame from JS,
// once the WASM wrapper lands in a later phase).
//
// Keeps its own monotonic total_cycles_, independent of cpu.cycles --
// copying cg-oac-6502's Machine, whose review doc (§10) explains why: a
// mid-run CPU reset (here, the keyboard controller's real-mode-return
// reset trick, cpu80286.h/i8042.h) zeroes cpu.cycles, which would corrupt
// any pacing budget measured directly against it. total_cycles_ instead
// only ever counts up, so the chipset's own device ticks (PIT, etc.) stay
// correctly paced across a CPU reset the same way they would on real
// hardware (the PIT doesn't reset just because the CPU did).
#ifndef IBMPCAT_MACHINE_H
#define IBMPCAT_MACHINE_H

#include "chipset.h"
#include "cpu80286.h"

#include <cstdint>

namespace ibmpcat {

class Machine {
public:
    // Constructing a Machine models powering one on: the CPU lands at its
    // real reset vector immediately (cpu80286::Cpu's own default member
    // values do NOT happen to equal that vector -- forgetting this call
    // is a real, easy-to-hit bug, not hypothetical: see IBM_PCAT_REVIEW.md
    // §8's account of a diagnostic harness that dropped it), and the
    // factory CMOS configuration is already burned in, the way a real
    // 5170-339 would have left IBM's factory (or a Setup-diskette run)
    // already configured.
    Machine() : cpu(chipset.make_bus()) { cpu.reset(); configure_factory_cmos(); }

    void reset() {
        chipset.reset();
        cpu.reset();
        // CMOS/RTC config is battery-backed and does NOT get erased by a
        // reset any more than `mem`/`rom_` do (see chipset.cpp's own
        // reset() comment) -- chipset.reset() already leaves it alone, so
        // there's nothing to redo here.
    }

    // Seeds CMOS with the configuration a real 5170-339 would have left the
    // factory (or a Setup-diskette run) already holding: base memory size,
    // floppy drive types, the equipment byte, and the boot-device sequence
    // (BX_ELTORITO_BOOT-style CMOS 0x3D, which the real BIOS substitute
    // this machine boots reads instead of auto-probing -- see
    // IBM_PCAT_REVIEW.md §8). Called once, at construction, not on every
    // reset() -- matching real non-volatile CMOS behavior.
    void configure_factory_cmos();

    // Executes instructions until at least `cycles` more CPU cycles have
    // elapsed (real 8 MHz -- see kCpuHz -- never sped up, per CLAUDE.md),
    // servicing the keyboard controller's reset-request line and pending
    // interrupts at each instruction boundary.
    void run_cycles(int64_t cycles);

    uint64_t total_cycles() const { return total_cycles_; }

    // Declared before `cpu` so it's constructed first -- `cpu`'s
    // constructor needs chipset.make_bus() already available.
    Chipset chipset;
    cpu80286::Cpu cpu;

    static constexpr double kCpuHz = 8000000.0;  // real 8 MHz 80286

private:
    uint64_t total_cycles_ = 0;
};

}  // namespace ibmpcat

#endif  // IBMPCAT_MACHINE_H
