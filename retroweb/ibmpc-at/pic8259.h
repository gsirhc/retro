// Intel 8259A Programmable Interrupt Controller. The AT has two cascaded: the
// master at 0x20/0x21 takes the slave's INT on IR2, the slave at 0xA0/0xA1
// handles IRQ8-15. chipset.h wires the cascade.
//
// Fixed priority only (IR0 highest) with normal EOI. Rotating and special-mask
// modes are unimplemented. ICW/OCW format: Intel 8259A data sheet, "Programming".
#ifndef IBMPCAT_PIC8259_H
#define IBMPCAT_PIC8259_H

#include <cstdint>

namespace ibmpcat {

class Pic8259 {
public:
    // `base` is 0x20 (master) or 0xA0 (slave).
    explicit Pic8259(uint16_t base) : base_(base) {}

    void reset();

    bool owns(uint16_t port) const { return port == base_ || port == uint16_t(base_ + 1); }
    uint8_t in(uint16_t port) const;
    void out(uint16_t port, uint8_t v);

    // `irq` is this chip's own IR0-IR7 (the slave's IR0 is system IRQ8). Most AT
    // devices are edge-triggered, so call raise() once per rising edge.
    void raise(int irq);
    void lower(int irq);

    bool has_interrupt() const { return highest_pending() >= 0; }
    // Peek at the next line to be serviced without touching IRR/ISR, so the
    // cascade can decide to forward INTA to the slave first.
    int peek_highest_pending() const { return highest_pending(); }
    // INTA: returns vector_base + the highest unmasked pending line, clears its
    // IRR bit and (unless auto-EOI) sets its ISR bit.
    uint8_t acknowledge();

private:
    uint16_t base_;

    uint8_t irr_ = 0, isr_ = 0, imr_ = 0xFF;  // powers on fully masked
    uint8_t vector_base_ = 0;
    uint8_t icw3_ = 0, icw4_ = 0;
    bool auto_eoi_ = false;
    bool single_mode_ = false;
    bool icw4_needed_ = false;
    int  icw_step_ = 0;       // 0 = idle, 1/2/3 = expecting that ICW next on the data port
    bool read_isr_next_ = false;  // OCW3 read select: false=IRR, true=ISR

    int highest_pending() const;
};

}  // namespace ibmpcat

#endif  // IBMPCAT_PIC8259_H
