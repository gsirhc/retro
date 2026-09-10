// Address decode implementation. See bus.h for the citation and the two
// confirmed corrections applied here.

#include "bus.h"

namespace bus {

void Bus::reset() {
    via.reset();
    acia.reset();
    nmi_latched_ = false;
    via_irq_last_ = acia_irq_last_ = false;
    for (auto &b : ram) b = 0;
}

uint8_t Bus::read(uint16_t addr) {
    last_access_was_contended = false;

    if (addr >= 0x8000) return rom.read(addr);          // CS_EEP = /A15 -- ROM.read() masks to 32K itself

    if (addr < 0x4000) return ram[addr];                 // corrected RAM range -- see file header

    // $4000-$7FFF: CS2 active (NAND(A14, CS_EEP)).
    bool via_sel = (addr & 0x2000) != 0;                  // A13
    bool acia_sel = (addr & 0x1000) != 0;                 // A12
    if (via_sel && acia_sel) {                            // $7000-$7FFF -- documented latent bug, see file header
        last_access_was_contended = true;
        // Both chips drive the bus at once; modeled as a wired-AND, a
        // common real-world approximation for two CMOS totem-pole outputs
        // fighting (whichever side pulls a bit low tends to win) -- not a
        // claim of exact real electrical behavior, just a deterministic
        // stand-in for firmware that (per the review doc) never actually
        // exercises this range.
        return uint8_t(via.read(addr & 0x0F) & acia.read(addr & 0x03));
    }
    if (via_sel) return via.read(addr & 0x0F);
    if (acia_sel) return acia.read(addr & 0x03);
    return 0xFF;   // $4000-$4FFF: CS2 asserted but neither chip further-qualified -- unmapped, floating bus
}

void Bus::write(uint16_t addr, uint8_t v) {
    last_access_was_contended = false;

    if (addr >= 0x8000) return;                          // ROM ~WE tied to +5V -- always write-disabled in-circuit

    if (addr < 0x4000) { ram[addr] = v; return; }

    bool via_sel = (addr & 0x2000) != 0;
    bool acia_sel = (addr & 0x1000) != 0;
    if (via_sel && acia_sel) {
        last_access_was_contended = true;
        via.write(addr & 0x0F, v);                        // both chips latch whatever's on the bus
        acia.write(addr & 0x03, v);
        return;
    }
    if (via_sel) { via.write(addr & 0x0F, v); return; }
    if (acia_sel) { acia.write(addr & 0x03, v); return; }
    // $4000-$4FFF: unmapped, write has no effect.
}

bool Bus::irq_line() const {
    bool via_irq = jumpers.via_irq_route == JumperState::Route::Irq && via.irq();
    bool acia_irq = jumpers.acia_irq_route == JumperState::Route::Irq && acia.irq();
    return via_irq || acia_irq;
}

bool Bus::nmi_pending() const { return nmi_latched_; }

void Bus::tick(int cycles) {
    via.tick(cycles);
    acia.tick(cycles);

    // NMI is edge-triggered on the CPU side; latch a fresh request whenever
    // either routed source transitions inactive->active, mirroring how
    // cpu65c02::Cpu::nmi() expects to be called (see machine.cpp).
    bool via_irq = via.irq();
    bool acia_irq = acia.irq();
    if (jumpers.via_irq_route == JumperState::Route::Nmi && via_irq && !via_irq_last_) nmi_latched_ = true;
    if (jumpers.acia_irq_route == JumperState::Route::Nmi && acia_irq && !acia_irq_last_) nmi_latched_ = true;
    via_irq_last_ = via_irq;
    acia_irq_last_ = acia_irq;
}

} // namespace bus
