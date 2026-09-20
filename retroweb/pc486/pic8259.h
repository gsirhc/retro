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
// implemented; see PC486_REVIEW.md. ICW/OCW format: Intel 8259A data
// sheet, "Programming".
#ifndef PC486_PIC8259_H
#define PC486_PIC8259_H

#include <cstdint>

namespace pc486 {

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

    bool has_interrupt() const { return pending_ != 0; }
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

    // Fully nested mode's priority resolver (Intel 8259A data sheet, "Fully
    // Nested Mode": "interrupts are ... allowed only if they are of higher
    // priority than the one currently being serviced"). Fixed priority, so
    // "higher" means a lower bit index than the highest-priority in-service
    // line; equal priority -- the line's own bit -- is blocked too, which is
    // what stops a handler that has re-enabled interrupts from being
    // re-entered by its own device before it writes EOI. Without this, a
    // device that re-asserts its line inside its own ISR (the AUX port does
    // exactly that for bytes 2 and 3 of every mouse packet) nests the
    // handler and scrambles the data it is assembling -- PC486_REVIEW.md §13.
    uint8_t in_service_mask() const {
        if (isr_ == 0) return 0xFF;
        return uint8_t((1u << __builtin_ctz(isr_)) - 1u);
    }
    uint8_t unmasked_pending() const { return uint8_t(irr_ & ~imr_ & in_service_mask()); }
    // The resolved answer, memoized. It is a pure function of IRR, IMR and
    // ISR, and only reset()/out()/raise()/lower()/acknowledge() can move any
    // of those -- all of which recompute it here -- while the machine loop
    // *asks* for it after every single instruction (see PC486_REVIEW.md §8;
    // §16 measured the recomputation at 5.2% of a BOOM run). Same resolver,
    // same result; only the frequency of running it changes.
    uint8_t pending_ = 0;
    void refresh_pending() { pending_ = unmasked_pending(); }
    // Fixed-priority resolution (IR0 highest, see the header comment), so
    // the winning line is just the lowest set bit.
    int highest_pending() const { return pending_ ? __builtin_ctz(pending_) : -1; }
    // out()'s ICW/OCW decode, with every exit path funnelled through out() so
    // none of them can forget to refresh the resolved answer above.
    void out_impl(uint16_t port, uint8_t v);
};

}  // namespace pc486

#endif  // PC486_PIC8259_H
