// Intel 8259A Programmable Interrupt Controller (Intel 8259A data sheet, 231468).
// The AT has two cascaded: master at 0x20/0x21 with the slave's INT on IR2,
// slave at 0xA0/0xA1 for IRQ8-15. chipset.cpp wires the cascade.
#ifndef IBMPCAT_PIC8259_H
#define IBMPCAT_PIC8259_H

#include <cstdint>

namespace ibmpcat {

class Pic8259 {
public:
    // `sp` is the level on the SP/EN pin: the AT ties the master's high, the slave's low.
    Pic8259(uint16_t base, bool sp) : base_(base), sp_(sp) {}

    void reset();

    bool owns(uint16_t port) const { return port == base_ || port == uint16_t(base_ + 1); }
    uint8_t in(uint16_t port);
    void out(uint16_t port, uint8_t v);

    // Drives IR0-IR7 to a level. The request latch is transparent, so a line that
    // drops before INTA withdraws its request (data sheet Figure 9).
    void set_line(int irq, bool level);
    bool line(int irq) const { return (lines_ >> irq) & 1; }

    // The INT pin.
    bool has_interrupt() const { return resolve() >= 0; }

    // First INTA: returns the level being acknowledged and sets its ISR bit. With
    // no request left it returns 7 and sets nothing ("default IR7").
    int inta1();
    // Second INTA: the 8086-mode vector byte for `level`, then AEOI if programmed.
    uint8_t inta2(int level);
    // True when this chip is a master and `level` is a slave input (ICW3).
    bool cascades(int level) const { return is_master() && !single_ && ((icw3_ >> level) & 1); }
    // The slave's ID from ICW3, compared against the CAS lines.
    int slave_id() const { return icw3_ & 7; }

    uint8_t irr() const { return irr_value(); }
    uint8_t isr() const { return isr_; }
    uint8_t imr() const { return imr_; }

private:
    uint16_t base_;
    bool sp_;

    uint8_t lines_ = 0;
    uint8_t armed_ = 0xFF;  // edge sense latches; set while a line is low
    uint8_t isr_ = 0, imr_ = 0xFF;
    uint8_t icw1_ = 0, icw2_ = 0, icw3_ = 0, icw4_ = 0;
    bool single_ = false;
    int icw_step_ = 0;            // 0 = idle, 2/3/4 = expecting that ICW on the data port
    int lowest_ = 7;              // bottom-priority level
    bool special_mask_ = false;
    bool rotate_aeoi_ = false;
    bool read_isr_ = false;
    bool poll_ = false;

    bool level_mode() const { return (icw1_ & 0x08) != 0; }
    bool aeoi() const { return (icw4_ & 0x02) != 0; }
    bool sfnm() const { return (icw4_ & 0x10) != 0; }
    bool is_master() const { return (icw4_ & 0x08) ? (icw4_ & 0x04) != 0 : sp_; }

    uint8_t irr_value() const { return uint8_t(lines_ & (level_mode() ? 0xFF : armed_)); }
    // Levels in priority order, highest first.
    int by_priority(int n) const { return (lowest_ + 1 + n) & 7; }
    int resolve() const;
    void set_isr(int level);
    int non_specific_eoi();
};

}  // namespace ibmpcat

#endif  // IBMPCAT_PIC8259_H
