// Top-level machine: wires the 80486 core to this machine's chipset glue
// layer and drives both from a single run_cycles() call, mirroring
// ibmpc-at/machine.h's convention exactly (see that file's header comment
// for the total_cycles_-vs-cpu.cycles rationale, which applies unchanged
// here -- a mid-run CPU reset still must not corrupt device pacing).
#ifndef PC486_MACHINE_H
#define PC486_MACHINE_H

#include "chipset.h"
#include "cpu80486.h"

#include <cstdint>

namespace pc486 {

class Machine {
public:
    // Constructing a Machine models powering one on: the CPU lands at its
    // real reset vector immediately, and the factory CMOS configuration is
    // already burned in, matching a real machine as it left the factory
    // (or a BIOS Setup run).
    Machine() : cpu(chipset.make_bus()) {
        cpu.timing = &cache;
        reset_cpu();
        set_rep_yield();
        configure_factory_cmos();
    }

    void reset() {
        chipset.reset();
        reset_cpu();
        // CMOS/RTC config is battery-backed and does NOT get erased by a
        // reset -- chipset.reset() already leaves it alone. A real front-
        // panel Reset button pulses exactly this: the RESET line to the
        // CPU and chipset, not the battery-backed CMOS.
    }

    // Seeds CMOS with the configuration this machine would have left the
    // factory (or a BIOS Setup run) already holding: base + extended memory
    // size, the single 3.5" floppy drive type, the equipment byte, the
    // boot-device sequence, and the 256MB fixed-disk geometry -- see
    // chipset's device defaults and PC486_REVIEW.md for the values' own
    // rationale. Called once, at construction, not on every reset() --
    // matching real non-volatile CMOS behavior.
    void configure_factory_cmos();

    // Executes instructions until at least `cycles` more CPU cycles have
    // elapsed. Cycle units are the CPU's internal clock -- 66 MHz with
    // Turbo on (DX2 clock-doubled), 33 MHz with Turbo off (bus rate; see
    // set_turbo). Wall-clock pacing of that counter is the front end's job
    // (never sped above the selected rate, per CLAUDE.md).
    void run_cycles(int64_t cycles);

    uint64_t total_cycles() const { return total_cycles_; }

    // The CPU's current internal clock. Turbo on = kCpuHz (66 MHz, the DX2's
    // doubled rate); Turbo off = kCpuHzDeturbo (33 MHz bus rate, clock
    // doubling disabled). The PIT crystal and other wall-clock-paced
    // devices stay correct because chipset.tick() is told this rate --
    // slowing the CPU does not slow the 1.193182 MHz PIT. See PC486_REVIEW.
    static constexpr double kCpuHz = 66000000.0;         // Turbo on
    static constexpr double kCpuHzDeturbo = 33000000.0;  // Turbo off
    double cpu_hz() const { return cpu_hz_; }
    void set_cpu_hz(double hz);
    bool turbo() const { return cpu_hz_ >= (kCpuHz + kCpuHzDeturbo) * 0.5; }
    void set_turbo(bool on) { set_cpu_hz(on ? kCpuHz : kCpuHzDeturbo); }

    // Diagnostic hook, called immediately before each instruction executes,
    // with the CPU already standing at that instruction's CS:EIP. A plain
    // function pointer plus ctx rather than a std::function, for §8's
    // reason: this is the loop that runs 66 million times a second, and a
    // type-erased call in it would not be affordable. Left null the cost is
    // a predictable null test, measured at about 1% over a full
    // hdd_boot_check boot (15.07 s against 14.92 s, native -O2). Exists
    // because a guest that dies does it somewhere its own output cannot
    // show, and the only way to see that is an instruction ring buffer --
    // the technique §5.4/§6.5 needed and §9 was found with. See
    // disks/boom_run_check.cpp.
    void (*on_instruction)(void *ctx, Machine &m) = nullptr;
    void *on_instruction_ctx = nullptr;

    // Declared before `cpu` so it's constructed first -- `cpu`'s
    // constructor needs chipset.make_bus() already available.
    Chipset chipset;
    Cache486 cache;
    cpu80486::Cpu cpu;

private:
    uint64_t total_cycles_ = 0;
    // One 1.193182 MHz PIT count, the finest step at which a device here can
    // raise an IRQ, so a long REP yields no later than a 486 would take it.
    void set_rep_yield() { cpu.rep_yield_cycles = uint32_t(cpu_hz_ / 1193182.0); }
    double cpu_hz_ = kCpuHz;
    void service_kbc_reset() {
        if (!chipset.kbc.reset_requested()) return;
        chipset.kbc.clear_reset_request();
        reset_cpu();
    }
    // The CPU leaves RESET with its L1 off (CR0.CD and NW set). A period
    // AMI or Award BIOS turns it on during POST; the Bochs BIOS stand-in
    // never does, so the board does it here, on every reset.
    void reset_cpu() {
        cpu.reset();
        cpu.enable_cache();
    }
};

}  // namespace pc486

#endif  // PC486_MACHINE_H
