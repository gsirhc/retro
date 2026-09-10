// WDC W65C22 VIA implementation. See via65c22.h for the interface and
// citation.
//
// Simplifications, labelled rather than silently approximated (none are
// exercised by the board's shipped firmware, which only uses DDRA/DDRB,
// free-run Timer 1, IER, and PORTA/PORTB -- see rom/via.s):
//   - Input latching (ACR bits 0/1) is not modeled -- port reads always
//     return the live external line state, never a CB1/CA1-edge-latched
//     snapshot. WDC datasheet §Input Latching.
//   - CA2/CB2 "pulse output" mode drops the line low for the duration of
//     one tick() call and raises it again before the next register access
//     can observe it, rather than a true single-Phi2-cycle pulse -- tick()
//     is called with however many cycles the last CPU instruction took,
//     not one cycle at a time, so sub-instruction pulse width isn't
//     representable without a per-cycle bus model.
//   - The shift register (SR) models the register and IFR bit but not the
//     8 external/T2/Phi2 clock-source nuances of ACR bits 2-4 in detail.

#include "via65c22.h"

namespace via65c22 {

void Via::reset() {
    orb_ = ora_ = ddrb_ = ddra_ = 0;
    t1c_ = t1l_ = 0xFFFF;
    t2c_ = 0xFFFF; t2l_lo_ = 0xFF;
    sr_ = 0;
    acr_ = pcr_ = 0;
    ifr_ = ier_ = 0;
    t1_running_ = t2_running_ = false;
    t1_pb7_ = true;
    ca1_ = cb1_ = ca2_ = cb2_ = false;
    ca1_last_ = cb1_last_ = ca2_in_last_ = cb2_in_last_ = false;
}

uint8_t Via::out_pb() const {
    uint8_t v = uint8_t((orb_ & ddrb_) | (read_pb ? (read_pb() & ~ddrb_) : uint8_t(~ddrb_)));
    if ((acr_ & 0x80) && (ddrb_ & 0x80)) {           // ACR bit7: PB7 driven by Timer1 (free-run/one-shot pulse)
        v = uint8_t((v & 0x7F) | (t1_pb7_ ? 0x80 : 0));
    }
    return v;
}

CxMode Via::ca2_mode() const {
    int m = (pcr_ >> 1) & 7;
    switch (m) {
        case 0: return CxMode::InputNegEdge;
        case 1: return CxMode::InputNegEdge;   // independent -- flag-clear distinction handled by caller
        case 2: return CxMode::InputPosEdge;
        case 3: return CxMode::InputPosEdge;
        case 4: return CxMode::HandshakeOut;
        case 5: return CxMode::PulseOut;
        default: return CxMode::ManualOut;      // 6 = fixed low, 7 = fixed high
    }
}
CxMode Via::cb2_mode() const {
    int m = (pcr_ >> 5) & 7;
    switch (m) {
        case 0: return CxMode::InputNegEdge;
        case 1: return CxMode::InputNegEdge;
        case 2: return CxMode::InputPosEdge;
        case 3: return CxMode::InputPosEdge;
        case 4: return CxMode::HandshakeOut;
        case 5: return CxMode::PulseOut;
        default: return CxMode::ManualOut;
    }
}

static bool is_independent(uint8_t pcr_field3) { return (pcr_field3 & 1) != 0; }

void Via::handle_ca1_edge() {
    set_if(IRQ_CA1);
    if (((pcr_ & 0x0E) >> 1) == 4) {                // CA2 handshake output: released high by CA1's active edge
        ca2_ = true;
        if (on_ca2_change) on_ca2_change(true);
    }
}
void Via::handle_cb1_edge() {
    set_if(IRQ_CB1);
    if (((pcr_ >> 5) & 7) == 4) {
        cb2_ = true;
        if (on_cb2_change) on_cb2_change(true);
    }
}

void Via::set_ca1(bool level) {
    bool edge_positive = (pcr_ & 0x01) != 0;
    bool active = edge_positive ? (level && !ca1_last_) : (!level && ca1_last_);
    ca1_last_ = level;
    if (active) handle_ca1_edge();
}
void Via::set_cb1(bool level) {
    bool edge_positive = (pcr_ & 0x10) != 0;
    bool active = edge_positive ? (level && !cb1_last_) : (!level && cb1_last_);
    cb1_last_ = level;
    if (active) handle_cb1_edge();
}
void Via::set_ca2_in(bool level) {
    CxMode m = ca2_mode();
    if (m != CxMode::InputNegEdge && m != CxMode::InputPosEdge) { ca2_in_last_ = level; return; }
    bool positive = m == CxMode::InputPosEdge;
    bool active = positive ? (level && !ca2_in_last_) : (!level && ca2_in_last_);
    ca2_in_last_ = level;
    if (active) set_if(IRQ_CA2);
}
void Via::set_cb2_in(bool level) {
    CxMode m = cb2_mode();
    if (m != CxMode::InputNegEdge && m != CxMode::InputPosEdge) { cb2_in_last_ = level; return; }
    bool positive = m == CxMode::InputPosEdge;
    bool active = positive ? (level && !cb2_in_last_) : (!level && cb2_in_last_);
    cb2_in_last_ = level;
    if (active) set_if(IRQ_CB2);
}

uint8_t Via::read(uint8_t reg) {
    switch (reg & 0x0F) {
        case 0x0: {                                      // ORB/IRB
            uint8_t v = out_pb();
            int field3 = (pcr_ >> 5) & 7;
            clear_if(IRQ_CB1);
            if (!is_independent(uint8_t(field3))) clear_if(IRQ_CB2);
            return v;
        }
        case 0x1: {                                      // ORA/IRA (with handshake)
            uint8_t v = out_pa();
            int field3 = (pcr_ >> 1) & 7;
            clear_if(IRQ_CA1);
            if (!is_independent(uint8_t(field3))) clear_if(IRQ_CA2);
            return v;
        }
        case 0x2: return ddrb_;
        case 0x3: return ddra_;
        case 0x4: clear_if(IRQ_T1); return uint8_t(t1c_ & 0xFF);
        case 0x5: return uint8_t((t1c_ >> 8) & 0xFF);
        case 0x6: return uint8_t(t1l_ & 0xFF);
        case 0x7: return uint8_t((t1l_ >> 8) & 0xFF);
        case 0x8: clear_if(IRQ_T2); return uint8_t(t2c_ & 0xFF);
        case 0x9: return uint8_t((t2c_ >> 8) & 0xFF);
        case 0xA: clear_if(IRQ_SR); return sr_;
        case 0xB: return acr_;
        case 0xC: return pcr_;
        case 0xD: return uint8_t(ifr_ | ((ifr_ & ier_ & 0x7F) ? 0x80 : 0));
        case 0xE: return uint8_t(ier_ | 0x80);
        default:  return out_pa();                        // $F: ORA, no handshake side effects
    }
}

void Via::write(uint8_t reg, uint8_t v) {
    switch (reg & 0x0F) {
        case 0x0: {
            orb_ = v;
            int field3 = (pcr_ >> 5) & 7;
            clear_if(IRQ_CB1);
            if (!is_independent(uint8_t(field3))) clear_if(IRQ_CB2);
            if (on_pb_change) on_pb_change(out_pb());
            break;
        }
        case 0x1: {
            ora_ = v;
            int field3 = (pcr_ >> 1) & 7;
            clear_if(IRQ_CA1);
            if (!is_independent(uint8_t(field3))) clear_if(IRQ_CA2);
            if (ca2_mode() == CxMode::PulseOut) { ca2_ = false; if (on_ca2_change) on_ca2_change(false); }
            if (on_pa_change) on_pa_change(out_pa());
            break;
        }
        case 0x2: ddrb_ = v; if (on_pb_change) on_pb_change(out_pb()); break;
        case 0x3: ddra_ = v; if (on_pa_change) on_pa_change(out_pa()); break;
        case 0x4: t1l_ = uint16_t((t1l_ & 0xFF00) | v); break;                     // T1L-L
        case 0x5:                                                                   // T1C-H: latch->counter, start, clear IRQ
            t1l_ = uint16_t((t1l_ & 0x00FF) | (uint16_t(v) << 8));
            t1c_ = t1l_; t1_running_ = true; t1_pb7_ = false;
            clear_if(IRQ_T1);
            break;
        case 0x6: t1l_ = uint16_t((t1l_ & 0xFF00) | v); break;                     // T1L-L (no side effect)
        case 0x7: t1l_ = uint16_t((t1l_ & 0x00FF) | (uint16_t(v) << 8)); clear_if(IRQ_T1); break; // T1L-H
        case 0x8: t2l_lo_ = v; break;                                               // T2L-L
        case 0x9:                                                                   // T2C-H: latch->counter, start, clear IRQ
            t2c_ = uint16_t(t2l_lo_ | (uint16_t(v) << 8));
            t2_running_ = true;
            clear_if(IRQ_T2);
            break;
        case 0xA: sr_ = v; clear_if(IRQ_SR); break;
        case 0xB: acr_ = v; break;
        case 0xC: {
            pcr_ = v;
            if (ca2_mode() == CxMode::ManualOut) { ca2_ = (pcr_ & 0x08) != 0; if (on_ca2_change) on_ca2_change(ca2_); }
            if (cb2_mode() == CxMode::ManualOut) { cb2_ = (pcr_ & 0x80) != 0; if (on_cb2_change) on_cb2_change(cb2_); }
            break;
        }
        case 0xD: ifr_ = uint8_t(ifr_ & ~(v & 0x7F)); break;   // write 1 to clear
        case 0xE:
            if (v & 0x80) ier_ = uint8_t(ier_ | (v & 0x7F));
            else ier_ = uint8_t(ier_ & ~(v & 0x7F));
            break;
        default: ora_ = v; if (on_pa_change) on_pa_change(out_pa()); break; // $F: ORA, no handshake
    }
}

void Via::tick(int n) {
    for (int i = 0; i < n; i++) {
        if (t1_running_) {
            if (t1c_ == 0) {
                t1c_ = t1l_;
                set_if(IRQ_T1);
                t1_pb7_ = !t1_pb7_;
                if (!(acr_ & 0x40)) t1_running_ = false;   // ACR bit6 clear = one-shot, stop after underflow
            } else {
                t1c_--;
            }
        }
        if (t2_running_ && !(acr_ & 0x20)) {   // ACR bit5 set = PB6 pulse-counting mode, not modeled (see header)
            if (t2c_ == 0) {
                t2c_ = 0xFFFF;
                set_if(IRQ_T2);
                t2_running_ = false;            // T2 is always one-shot
            } else {
                t2c_--;
            }
        }
    }
}

} // namespace via65c22
