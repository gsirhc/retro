// WDC W65C02S core (U1): CMOS instruction set and fixes over NMOS, no NMOS bugs.
// Datasheet: https://www.westerndesigncenter.com/wdc/documentation/w65c02s.pdf

#ifndef CG_OAC_6502_CPU65C02_H
#define CG_OAC_6502_CPU65C02_H

#include <cstdint>
#include <functional>

namespace cpu65c02 {

enum Flag : uint8_t {
    FLAG_C = 1 << 0,  // carry
    FLAG_Z = 1 << 1,  // zero
    FLAG_I = 1 << 2,  // IRQ disable
    FLAG_D = 1 << 3,  // decimal mode
    FLAG_B = 1 << 4,  // break (only meaningful in the byte pushed by BRK/PHP)
    FLAG_U = 1 << 5,  // unused, always reads 1
    FLAG_V = 1 << 6,  // overflow
    FLAG_N = 1 << 7,  // negative (sign)
};

struct Bus {
    std::function<uint8_t(uint16_t addr)>         read;
    std::function<void(uint16_t addr, uint8_t v)> write;
};

class Cpu {
public:
    uint8_t  a = 0, x = 0, y = 0;
    uint8_t  sp = 0xFD;
    uint16_t pc = 0;
    uint8_t  p = FLAG_U | FLAG_I;

    // WAI wakes on any pending IRQ/NMI (serviced only if I=0); STP halts until reset.
    bool waiting = false;
    bool stopped = false;

    uint64_t cycles = 0;

    // Level-sensitive IRQ and edge-triggered NMI; step() samples them once per instruction.
    bool irq_line = false;
    void nmi();

    explicit Cpu(Bus bus) : bus_(std::move(bus)) {}

    // Rebinds the bus callbacks without touching registers or the cycle count.
    void rebind_bus(Bus bus) { bus_ = std::move(bus); }

    // Loads PC from $FFFC. SP lands on 0xFD (three phantom pushes on real
    // hardware). Clears D, unlike NMOS (WDC datasheet).
    void reset();

    // Executes one instruction (or an idle tick if WAI/STP). Returns cycles.
    int step();

    bool flag(Flag f) const { return (p & f) != 0; }

private:
    Bus bus_;
    bool nmi_pending_ = false;

    uint8_t  rb(uint16_t addr)            { return bus_.read(addr); }
    void     wb(uint16_t addr, uint8_t v) { bus_.write(addr, v); }
    uint8_t  fetch8()  { return rb(pc++); }
    uint16_t fetch16() { uint16_t v = uint16_t(rb(pc)) | (uint16_t(rb(pc + 1)) << 8); pc += 2; return v; }

    void    push8(uint8_t v)  { wb(0x0100 + sp--, v); }
    uint8_t pop8()            { return rb(0x0100 + uint8_t(++sp)); }
    void    push16(uint16_t v){ push8(v >> 8); push8(v & 0xFF); }
    uint16_t pop16()          { uint8_t lo = pop8(); uint8_t hi = pop8(); return uint16_t(lo) | (uint16_t(hi) << 8); }

    void set_flag(Flag f, bool on) { p = on ? uint8_t(p | f) : uint8_t(p & ~f); }
    void set_nz(uint8_t v)         { set_flag(FLAG_Z, v == 0); set_flag(FLAG_N, (v & 0x80) != 0); }

    // Addressing modes return the effective address. `crossed` reports a page
    // cross, for the +1 cycle on reads (not stores or RMW).
    uint16_t am_zp()    { return fetch8(); }
    uint16_t am_zpx()   { return uint8_t(fetch8() + x); }
    uint16_t am_zpy()   { return uint8_t(fetch8() + y); }
    uint16_t am_abs()   { return fetch16(); }
    uint16_t am_absx(bool &crossed) {
        uint16_t base = fetch16(); uint16_t addr = uint16_t(base + x);
        crossed = (base & 0xFF00) != (addr & 0xFF00); return addr;
    }
    uint16_t am_absy(bool &crossed) {
        uint16_t base = fetch16(); uint16_t addr = uint16_t(base + y);
        crossed = (base & 0xFF00) != (addr & 0xFF00); return addr;
    }
    uint16_t am_indx() {
        uint8_t zp = uint8_t(fetch8() + x);
        return uint16_t(rb(zp)) | (uint16_t(rb(uint8_t(zp + 1))) << 8);
    }
    uint16_t am_indy(bool &crossed) {
        uint8_t zp = fetch8();
        uint16_t base = uint16_t(rb(zp)) | (uint16_t(rb(uint8_t(zp + 1))) << 8);
        uint16_t addr = uint16_t(base + y);
        crossed = (base & 0xFF00) != (addr & 0xFF00); return addr;
    }
    // 65C02 (zp) mode, no index
    uint16_t am_ind() {
        uint8_t zp = fetch8();
        return uint16_t(rb(zp)) | (uint16_t(rb(uint8_t(zp + 1))) << 8);
    }
    int8_t rel() { return int8_t(fetch8()); }

    void adc(uint8_t v);
    void sbc(uint8_t v);
    void cmp_(uint8_t reg, uint8_t v);
    void bit(uint8_t v, bool immediate);
    uint8_t asl_(uint8_t v);
    uint8_t lsr_(uint8_t v);
    uint8_t rol_(uint8_t v);
    uint8_t ror_(uint8_t v);

    void branch(bool cond, int &extra);
    void service_irq(bool is_nmi, bool is_brk);
};

} // namespace cpu65c02

#endif // CG_OAC_6502_CPU65C02_H
