// WDC W65C02S CPU core.
//
// The board (see FullBoard/pcb6502full.net, U1) populates a real WDC
// W65C02S, not an NMOS 6502 — this core implements the CMOS instruction
// set and its documented behavioral fixes over NMOS (see cpu65c02.cpp for
// the specific list), never the NMOS bugs some other emulators port by
// habit. Cite: WDC W65C02S datasheet
// (https://www.westerndesigncenter.com/wdc/documentation/w65c02s.pdf).
//
// Host-agnostic like i8080::Cpu: talks to the outside world only through
// the Bus callbacks, so the same core drives the GoogleTest harness (incl.
// Klaus Dormann's 6502/65C02 functional test) and the real machine.

#ifndef CG_OAC_6502_CPU65C02_H
#define CG_OAC_6502_CPU65C02_H

#include <cstdint>
#include <functional>

namespace cpu65c02 {

// Processor status register bits.
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

    // WAI (Wait for Interrupt) and STP (Stop) — both real W65C02S
    // instructions with no NMOS equivalent. WAI suspends fetch/execute
    // until an IRQ or NMI is pending (the interrupt still only *services*
    // if I=0, but WAI itself wakes on either); STP suspends until reset.
    bool waiting = false;
    bool stopped = false;

    uint64_t cycles = 0;   // total clock cycles executed, for wall-clock pacing

    // Level-sensitive IRQ line (wire-ORed by the bus from VIA/ACIA, per the
    // board's J7 jumper routing) and edge-triggered NMI request — the
    // embedding machine sets/clears these; step() samples them once per
    // instruction boundary, matching real 65C02 interrupt polling.
    bool irq_line = false;
    void nmi();            // request an NMI (edge — latches until serviced)

    explicit Cpu(Bus bus) : bus_(std::move(bus)) {}

    // Load PC from the reset vector ($FFFC/$FFFD). SP settles at 0xFD: a
    // real 65C02 decrements SP three times during reset with R/W forced
    // high (no actual bus writes), so from a power-on SP of 0 it lands on
    // 0xFD — modeled directly rather than simulating the phantom pushes.
    // The 65C02 (unlike NMOS 6502) also clears D on reset — WDC datasheet.
    void reset();

    // Decode and execute one instruction (or one idle tick if WAI/STP-
    // suspended). Returns clock cycles consumed.
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

    // Addressing modes: each returns the effective address, advancing PC
    // past its operand bytes. `crossed` (when supplied) reports whether an
    // indexed mode crossed a page boundary, for the +1-cycle penalty on
    // read-only instructions (never charged on stores or read-modify-write).
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
    // 65C02 addition: (zp) with no index — fills the gap NMOS left at the
    // $x2 column for eight of the ALU ops (ORA/AND/EOR/ADC/STA/LDA/CMP/SBC).
    uint16_t am_ind() {
        uint8_t zp = fetch8();
        return uint16_t(rb(zp)) | (uint16_t(rb(uint8_t(zp + 1))) << 8);
    }
    int8_t rel() { return int8_t(fetch8()); }

    // ALU / RMW primitives.
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
