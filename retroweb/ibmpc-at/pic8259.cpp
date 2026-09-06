#include "pic8259.h"

namespace ibmpcat {

void Pic8259::reset() {
    irr_ = isr_ = 0;
    imr_ = 0xFF;
    vector_base_ = 0;
    icw3_ = icw4_ = 0;
    auto_eoi_ = false;
    single_mode_ = false;
    icw4_needed_ = false;
    icw_step_ = 0;
    read_isr_next_ = false;
}

uint8_t Pic8259::in(uint16_t port) const {
    if (port == uint16_t(base_ + 1)) return imr_;
    return read_isr_next_ ? isr_ : irr_;
}

void Pic8259::out(uint16_t port, uint8_t v) {
    if (port == base_) {
        if (v & 0x10) {  // ICW1
            irr_ = isr_ = 0;
            icw_step_ = 1;
            single_mode_ = (v & 0x02) != 0;
            icw4_needed_ = (v & 0x01) != 0;
            return;
        }
        if (v & 0x08) {  // OCW3
            if (v & 0x02) read_isr_next_ = (v & 0x01) != 0;
            // poll command (bit 2) not implemented -- see IBM_PCAT_REVIEW.md
            return;
        }
        // OCW2: EOI family. bits 7-5 select the command, bit 6 = specific,
        // bits 2-0 = the IR line for a specific command.
        bool specific = (v & 0x40) != 0;
        int level = v & 0x07;
        if (specific) {
            isr_ = uint8_t(isr_ & ~(1 << level));
        } else {
            for (int i = 0; i < 8; ++i) {
                if (isr_ & (1 << i)) { isr_ = uint8_t(isr_ & ~(1 << i)); break; }
            }
        }
        return;
    }
    // data port (base+1)
    if (icw_step_ == 1) {
        vector_base_ = uint8_t(v & 0xF8);
        icw_step_ = single_mode_ ? (icw4_needed_ ? 3 : 0) : 2;
        return;
    }
    if (icw_step_ == 2) {
        icw3_ = v;
        icw_step_ = icw4_needed_ ? 3 : 0;
        return;
    }
    if (icw_step_ == 3) {
        icw4_ = v;
        auto_eoi_ = (v & 0x02) != 0;
        icw_step_ = 0;
        return;
    }
    imr_ = v;  // OCW1
}

void Pic8259::raise(int irq) { irr_ = uint8_t(irr_ | (1 << irq)); }
void Pic8259::lower(int irq) { irr_ = uint8_t(irr_ & ~(1 << irq)); }

int Pic8259::highest_pending() const {
    uint8_t pending = uint8_t(irr_ & ~imr_);
    for (int i = 0; i < 8; ++i)
        if (pending & (1 << i)) return i;
    return -1;
}

uint8_t Pic8259::acknowledge() {
    int line = highest_pending();
    if (line < 0) return vector_base_;  // spurious -- caller should have checked has_interrupt()
    irr_ = uint8_t(irr_ & ~(1 << line));
    if (!auto_eoi_) isr_ = uint8_t(isr_ | (1 << line));
    return uint8_t(vector_base_ + line);
}

}  // namespace ibmpcat
