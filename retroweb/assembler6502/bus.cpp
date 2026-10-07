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

    if (addr >= 0x8000) return rom.read(addr);          // CS_EEP = /A15

    if (addr < 0x4000) return ram[addr];

    // $4000-$7FFF: CS2 active
    bool via_sel = (addr & 0x2000) != 0;                  // A13
    bool acia_sel = (addr & 0x1000) != 0;                 // A12
    if (via_sel && acia_sel) {                            // $7000-$7FFF: both selects assert
        last_access_was_contended = true;
        // bus contention modeled as wired-AND; shipped firmware never hits this range
        return uint8_t(via.read(addr & 0x0F) & acia.read(addr & 0x03));
    }
    if (via_sel) return via.read(addr & 0x0F);
    if (acia_sel) return acia.read(addr & 0x03);
    return 0xFF;   // $4000-$4FFF: unmapped
}

void Bus::write(uint16_t addr, uint8_t v) {
    last_access_was_contended = false;

    if (addr >= 0x8000) return;                          // ROM ~WE tied to +5V

    if (addr < 0x4000) { ram[addr] = v; return; }

    bool via_sel = (addr & 0x2000) != 0;
    bool acia_sel = (addr & 0x1000) != 0;
    if (via_sel && acia_sel) {
        last_access_was_contended = true;
        via.write(addr & 0x0F, v);
        acia.write(addr & 0x03, v);
        return;
    }
    if (via_sel) { via.write(addr & 0x0F, v); return; }
    if (acia_sel) { acia.write(addr & 0x03, v); return; }
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

    // NMI is edge-triggered: latch on inactive->active
    bool via_irq = via.irq();
    bool acia_irq = acia.irq();
    if (jumpers.via_irq_route == JumperState::Route::Nmi && via_irq && !via_irq_last_) nmi_latched_ = true;
    if (jumpers.acia_irq_route == JumperState::Route::Nmi && acia_irq && !acia_irq_last_) nmi_latched_ = true;
    via_irq_last_ = via_irq;
    acia_irq_last_ = acia_irq;
}

} // namespace bus
