// WDC W65C22 VIA, U4. 16 registers selected by A0-A3: PORTB, PORTA, DDRB,
// DDRA, T1C-L, T1C-H, T1L-L, T1L-H, T2C-L, T2C-H, SR, ACR, PCR, IFR, IER,
// PORTA-no-handshake. Datasheet:
// https://www.westerndesigncenter.com/wdc/documentation/w65c22.pdf
// CMOS part, so no NMOS 6522 errata (extra T2 one-shot IRQ, PB7 glitches).

#ifndef CG_OAC_6502_VIA65C22_H
#define CG_OAC_6502_VIA65C22_H

#include <cstdint>
#include <functional>

namespace via65c22 {

enum class CxMode { InputNegEdge, InputPosEdge, HandshakeOut, PulseOut, ManualOut };

class Via {
public:
    // read_pa/read_pb sample external lines for input bits (DDR bit = 0);
    // on_pa_change/on_pb_change fire on every output latch change.
    std::function<uint8_t()> read_pa;
    std::function<uint8_t()> read_pb;
    std::function<void(uint8_t)> on_pa_change;
    std::function<void(uint8_t)> on_pb_change;
    std::function<void(bool)> on_ca2_change;
    std::function<void(bool)> on_cb2_change;

    // Level-sensitive IRQ output
    bool irq() const { return (ifr_ & ier_ & 0x7F) != 0; }

    uint8_t read(uint8_t reg);
    void write(uint8_t reg, uint8_t v);

    // Port B as pins see it (latch plus live inputs), without read side effects
    uint8_t peek_pb() const { return out_pb(); }

    // Advance `n` Phi2 cycles: T1/T2, free-run reload, PB7 toggle, IFR on underflow
    void tick(int n);

    // External edge inputs
    void set_ca1(bool level);
    void set_cb1(bool level);
    void set_ca2_in(bool level);
    void set_cb2_in(bool level);

    void reset();

private:
    uint8_t orb_ = 0, ora_ = 0, ddrb_ = 0, ddra_ = 0;
    uint16_t t1c_ = 0xFFFF, t1l_ = 0xFFFF;
    uint16_t t2c_ = 0xFFFF; uint8_t t2l_lo_ = 0xFF;
    uint8_t sr_ = 0;
    uint8_t acr_ = 0, pcr_ = 0;
    uint8_t ifr_ = 0, ier_ = 0;
    bool t1_running_ = false, t1_pb7_ = true;   // PB7 output level in pulse mode
    bool t2_running_ = false;
    bool ca1_ = false, cb1_ = false, ca2_ = false, cb2_ = false;
    bool ca1_last_ = false, cb1_last_ = false, ca2_in_last_ = false, cb2_in_last_ = false;

    void set_if(uint8_t bit) { ifr_ = uint8_t(ifr_ | bit); }
    void clear_if(uint8_t bit) { ifr_ = uint8_t(ifr_ & ~bit); }

    uint8_t out_pa() const { return uint8_t((ora_ & ddra_) | (read_pa ? (read_pa() & ~ddra_) : uint8_t(~ddra_))); }
    uint8_t out_pb() const;

    CxMode ca2_mode() const;
    CxMode cb2_mode() const;
    void handle_ca1_edge();
    void handle_cb1_edge();
};

// IFR/IER bit positions (WDC datasheet Table 4).
enum IrqBit : uint8_t {
    IRQ_CA2 = 1 << 0, IRQ_CA1 = 1 << 1, IRQ_SR = 1 << 2, IRQ_CB2 = 1 << 3,
    IRQ_CB1 = 1 << 4, IRQ_T2 = 1 << 5, IRQ_T1 = 1 << 6, IRQ_ANY = 1 << 7,
};

} // namespace via65c22

#endif // CG_OAC_6502_VIA65C22_H
