#include "pic8259.h"

namespace ibmpcat {

void Pic8259::reset() {
    armed_ = uint8_t(~lines_);
    isr_ = 0;
    imr_ = 0xFF;
    icw1_ = icw2_ = icw3_ = icw4_ = 0;
    single_ = false;
    icw_step_ = 0;
    lowest_ = 7;
    special_mask_ = false;
    rotate_aeoi_ = false;
    read_isr_ = false;
    poll_ = false;
}

void Pic8259::set_line(int irq, bool level) {
    uint8_t bit = uint8_t(1 << irq);
    if (level) {
        lines_ = uint8_t(lines_ | bit);
    } else {
        lines_ = uint8_t(lines_ & ~bit);
        armed_ = uint8_t(armed_ | bit);
    }
}

int Pic8259::resolve() const {
    uint8_t req = uint8_t(irr_value() & ~imr_);
    // Special mask mode hides masked IS bits from the resolver, as it does from a non-specific EOI.
    uint8_t in_service = special_mask_ ? uint8_t(isr_ & ~imr_) : isr_;
    for (int n = 0; n < 8; ++n) {
        int level = by_priority(n);
        uint8_t bit = uint8_t(1 << level);
        if (in_service & bit) {
            // Special fully nested: a slave in service doesn't lock out its own input.
            if (sfnm() && cascades(level) && (req & bit)) return level;
            return -1;
        }
        if (req & bit) return level;
    }
    return -1;
}

void Pic8259::set_isr(int level) {
    uint8_t bit = uint8_t(1 << level);
    isr_ = uint8_t(isr_ | bit);
    // SET ISR clears the edge sense latch (Figure 9); a low line re-sets it at once.
    armed_ = uint8_t(armed_ & ~(bit & lines_));
}

int Pic8259::non_specific_eoi() {
    uint8_t visible = special_mask_ ? uint8_t(isr_ & ~imr_) : isr_;
    for (int n = 0; n < 8; ++n) {
        int level = by_priority(n);
        if (visible & (1 << level)) {
            isr_ = uint8_t(isr_ & ~(1 << level));
            return level;
        }
    }
    return -1;
}

int Pic8259::inta1() {
    int level = resolve();
    if (level < 0) return 7;
    set_isr(level);
    return level;
}

uint8_t Pic8259::inta2(int level) {
    uint8_t vec;
    if (icw4_ & 0x01) {
        vec = uint8_t((icw2_ & 0xF8) | level);
    } else if (icw1_ & 0x04) {
        // MCS-80/85 mode: the 286 reads the CALL's second byte, interval 4
        vec = uint8_t((icw1_ & 0xE0) | (level << 2));
    } else {
        vec = uint8_t((icw1_ & 0xC0) | (level << 3));
    }
    // MCS-80/85 AEOI waits for a third INTA the 286 never sends.
    if (aeoi() && (icw4_ & 0x01)) {
        int done = non_specific_eoi();
        if (rotate_aeoi_ && done >= 0) lowest_ = done;
    }
    return vec;
}

uint8_t Pic8259::in(uint16_t port) {
    if (poll_) {
        // "The Poll Command": this RD is an interrupt acknowledge; bits 6-3 are undefined
        poll_ = false;
        int level = resolve();
        if (level < 0) return 0x00;
        set_isr(level);
        return uint8_t(0x80 | level);
    }
    if (port == uint16_t(base_ + 1)) return imr_;
    return read_isr_ ? isr_ : irr_value();
}

void Pic8259::out(uint16_t port, uint8_t v) {
    if (port == base_) {
        if (v & 0x10) {
            // ICW1 resets edge sense, IMR, priority, slave address, SMM and the status
            // read, and ICW4 if IC4=0 ("Initialization Command Words"). ISR clears too:
            // a Ctrl+Alt+Del reboot reaches POST with IRQ1 still in service.
            icw1_ = v;
            armed_ = uint8_t(~lines_);
            imr_ = 0;
            isr_ = 0;
            lowest_ = 7;
            icw3_ = 7;
            special_mask_ = false;
            read_isr_ = false;
            single_ = (v & 0x02) != 0;
            if (!(v & 0x01)) icw4_ = 0;
            icw_step_ = 2;
            return;
        }
        if (v & 0x08) {  // OCW3
            if (v & 0x40) special_mask_ = (v & 0x20) != 0;
            if (v & 0x04) poll_ = true;
            if (v & 0x02) read_isr_ = (v & 0x01) != 0;
            return;
        }
        // OCW2: R, SL, EOI
        int level = v & 0x07;
        switch (v >> 5) {
            case 1: non_specific_eoi(); break;
            case 3: isr_ = uint8_t(isr_ & ~(1 << level)); break;
            case 5: { int done = non_specific_eoi(); if (done >= 0) lowest_ = done; break; }
            case 4: rotate_aeoi_ = true; break;
            case 0: rotate_aeoi_ = false; break;
            case 7: isr_ = uint8_t(isr_ & ~(1 << level)); lowest_ = level; break;
            case 6: lowest_ = level; break;
            default: break;  // 2: no operation
        }
        return;
    }
    switch (icw_step_) {
        case 2:
            icw2_ = v;
            icw_step_ = single_ ? ((icw1_ & 0x01) ? 4 : 0) : 3;
            return;
        case 3:
            icw3_ = v;
            icw_step_ = (icw1_ & 0x01) ? 4 : 0;
            return;
        case 4:
            icw4_ = v;
            icw_step_ = 0;
            return;
        default:
            imr_ = v;  // OCW1
            return;
    }
}

}  // namespace ibmpcat
