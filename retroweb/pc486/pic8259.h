// Intel 8259A PIC. The AT has two, cascaded: master on 0x20/0x21 with IR2
// fed by the slave's INT, slave on 0xA0/0xA1 for IRQ8-15. chipset.h wires the
// cascade. Fixed priority (IR0 highest), normal EOI, OCW3 poll. No rotating
// or special-mask modes. ICW/OCW format: 8259A data sheet, "Programming".
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
    uint8_t in(uint16_t port);
    void out(uint16_t port, uint8_t v);

    // `irq` is 0-7 on this chip (the slave's IR0 is system IRQ8). Most AT devices
    // are edge-triggered, so a device calls raise() once per rising edge.
    void raise(int irq);
    void lower(int irq);

    bool has_interrupt() const { return pending_ != 0; }
    // Peek at the next line to service without mutating IRR/ISR; the cascade
    // needs it before committing to the master's acknowledge
    int peek_highest_pending() const { return highest_pending(); }
    // INTA: returns vector_base + line for the highest-priority unmasked line,
    // clears its IRR bit and (unless auto-EOI) sets ISR
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
    bool poll_next_ = false;      // OCW3 P bit: the next read is a poll

    // Fully nested mode (8259A "Fully Nested Mode"): a line is blocked unless
    // higher priority than the one in service, including its own bit. Stops a
    // handler that re-enabled interrupts being re-entered before EOI, which
    // scrambled mouse packets (PC486_REVIEW.md §13).
    uint8_t in_service_mask() const {
        if (isr_ == 0) return 0xFF;
        return uint8_t((1u << __builtin_ctz(isr_)) - 1u);
    }
    uint8_t unmasked_pending() const { return uint8_t(irr_ & ~imr_ & in_service_mask()); }
    // Memoized resolver result. Depends only on IRR, IMR and ISR, recomputed
    // on every change since the machine loop asks after every instruction
    // (PC486_REVIEW.md §8, §16).
    uint8_t pending_ = 0;
    void refresh_pending() { pending_ = unmasked_pending(); }
    // Fixed priority: the winner is the lowest set bit
    int highest_pending() const { return pending_ ? __builtin_ctz(pending_) : -1; }
    // out()'s ICW/OCW decode; all exits refresh the resolved answer
    void out_impl(uint16_t port, uint8_t v);
};

}  // namespace pc486

#endif  // PC486_PIC8259_H
