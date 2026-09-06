// Intel 8259A Programmable Interrupt Controller.
//
// The AT has two of these, cascaded: the master owns ports 0x20/0x21 and
// its IR2 input is wired to the slave's INT output; the slave owns
// 0xA0/0xA1 and handles IRQ8-15. Each chip is modeled independently here;
// `chipset.h` wires the cascade (master.raise(2)/lower(2) driven by
// slave.has_interrupt(), and forwards INTA to the slave when the master's
// highest-pending line is 2).
//
// Scope: fixed-priority mode only (IR0 highest), normal (non-rotating,
// non-speccial-mask) EOI handling -- real DOS-era BIOS/software never
// programs the rotating-priority or special-mask modes, so they're not
// implemented; see IBM_PCAT_REVIEW.md. ICW/OCW format: Intel 8259A data
// sheet, "Programming".
#ifndef IBMPCAT_PIC8259_H
#define IBMPCAT_PIC8259_H

#include <cstdint>

namespace ibmpcat {

class Pic8259 {
public:
    // `base` is 0x20 (master) or 0xA0 (slave) on a genuine AT.
    explicit Pic8259(uint16_t base) : base_(base) {}

    void reset();

    bool owns(uint16_t port) const { return port == base_ || port == uint16_t(base_ + 1); }
    uint8_t in(uint16_t port) const;
    void out(uint16_t port, uint8_t v);

    // Level/edge input from a peripheral: `irq` is 0-7 (this chip's own
    // IR0-IR7, not a global IRQ number -- e.g. the slave's IR0 is the
    // system's IRQ8). Matches real ISA convention: most AT devices are
    // edge-triggered, so a device calls raise() once per rising edge.
    void raise(int irq);
    void lower(int irq);

    bool has_interrupt() const { return highest_pending() >= 0; }
    // Read-only peek at which line would be serviced next, without
    // mutating IRR/ISR -- the chipset's master/slave cascade needs this to
    // decide whether to forward an INTA cycle to the slave (line 2)
    // *before* committing to the master's own acknowledge side effects.
    int peek_highest_pending() const { return highest_pending(); }
    // INTA cycle: returns the resolved interrupt vector (vector_base + line)
    // for the highest-priority pending, unmasked line; clears that line's
    // IRR bit and (unless auto-EOI is programmed) sets its ISR bit so a
    // lower-priority interrupt can't preempt it until EOI'd.
    uint8_t acknowledge();

private:
    uint16_t base_;

    uint8_t irr_ = 0, isr_ = 0, imr_ = 0xFF;  // real chips power on with everything masked
    uint8_t vector_base_ = 0;
    uint8_t icw3_ = 0, icw4_ = 0;
    bool auto_eoi_ = false;
    bool single_mode_ = false;
    bool icw4_needed_ = false;
    int  icw_step_ = 0;       // 0 = idle (OCW-ready), 1/2/3 = expecting that ICW next on the data port
    bool read_isr_next_ = false;  // OCW3 register-read select: false=IRR, true=ISR

    int highest_pending() const;
};

}  // namespace ibmpcat

#endif  // IBMPCAT_PIC8259_H
