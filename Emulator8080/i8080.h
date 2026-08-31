// Intel 8080 CPU core.
//
// The core is host-agnostic: it talks to the outside world only through the
// Bus callbacks below, so the same core drives a raw RAM test harness, a
// Space Invaders machine, a CP/M box, etc.

#ifndef EMULATOR8080_I8080_H
#define EMULATOR8080_I8080_H

#include <cstdint>
#include <functional>

namespace i8080 {

// Flag bit positions inside the F register (the "PSW" low byte).
// Bits 5 and 3 always read 0, bit 1 always reads 1 on real hardware.
enum Flag : uint8_t {
    FLAG_C  = 1 << 0,  // carry / borrow
    FLAG_N1 = 1 << 1,  // unused, always 1
    FLAG_P  = 1 << 2,  // parity (1 = even number of set bits)
    FLAG_N3 = 1 << 3,  // unused, always 0
    FLAG_AC = 1 << 4,  // auxiliary (half) carry, used by DAA
    FLAG_N5 = 1 << 5,  // unused, always 0
    FLAG_Z  = 1 << 6,  // zero
    FLAG_S  = 1 << 7,  // sign (bit 7 of the result)
};

// Memory and I/O access callbacks supplied by the embedding machine.
struct Bus {
    std::function<uint8_t(uint16_t addr)>          read;    // memory read
    std::function<void(uint16_t addr, uint8_t v)>  write;   // memory write
    std::function<uint8_t(uint8_t port)>           in;      // IN  <port>
    std::function<void(uint8_t port, uint8_t v)>   out;     // OUT <port>
};

class Cpu {
public:
    // 8-bit registers. The 8080 pairs them as BC, DE, HL for 16-bit ops.
    uint8_t a = 0, b = 0, c = 0, d = 0, e = 0, h = 0, l = 0;
    uint8_t f = FLAG_N1;   // flags register

    uint16_t pc = 0;       // program counter
    uint16_t sp = 0;       // stack pointer

    bool     halted = false;
    bool     int_enabled = false;   // set by EI, cleared by DI / interrupt accept
    uint64_t cycles = 0;            // total T-states executed

    explicit Cpu(Bus bus) : bus_(std::move(bus)) {}

    // Restore power-on state (PC = 0, everything else cleared).
    void reset();

    // Decode and execute exactly one instruction at PC.
    // Returns the number of clock cycles (T-states) it consumed.
    int step();

    // Request a hardware interrupt. `opcode` is the byte the interrupting
    // device jams onto the bus, almost always one of the RST n opcodes
    // (0xC7 + n*8). Serviced only when interrupts are enabled; wakes HALT.
    // Returns cycles consumed (0 if ignored).
    int interrupt(uint8_t opcode);

    // --- 16-bit register-pair helpers -----------------------------------
    uint16_t bc() const { return (uint16_t(b) << 8) | c; }
    uint16_t de() const { return (uint16_t(d) << 8) | e; }
    uint16_t hl() const { return (uint16_t(h) << 8) | l; }
    void set_bc(uint16_t v) { b = v >> 8; c = v & 0xFF; }
    void set_de(uint16_t v) { d = v >> 8; e = v & 0xFF; }
    void set_hl(uint16_t v) { h = v >> 8; l = v & 0xFF; }

    bool flag(Flag fl) const { return (f & fl) != 0; }

private:
    Bus bus_;

    // memory / immediate fetch helpers
    uint8_t  rb(uint16_t addr)             { return bus_.read(addr); }
    void     wb(uint16_t addr, uint8_t v)  { bus_.write(addr, v); }
    uint16_t rw(uint16_t addr)             { return rb(addr) | (uint16_t(rb(addr + 1)) << 8); }
    void     ww(uint16_t addr, uint16_t v) { wb(addr, v & 0xFF); wb(addr + 1, v >> 8); }
    uint8_t  fetch8()                      { return rb(pc++); }
    uint16_t fetch16()                     { uint16_t v = rw(pc); pc += 2; return v; }

    // stack
    void     push(uint16_t v) { sp -= 2; ww(sp, v); }
    uint16_t pop()            { uint16_t v = rw(sp); sp += 2; return v; }

    // flag helpers
    void set_flag(Flag fl, bool on) { f = on ? (f | fl) : (f & ~fl); }
    void set_szp(uint8_t v);                 // sign, zero, parity from a result
    static bool parity_even(uint8_t v);

    // ALU primitives (all write A and flags unless noted)
    void add(uint8_t v, bool carry_in);
    void sub(uint8_t v, bool borrow_in);
    void ana(uint8_t v);
    void xra(uint8_t v);
    void ora(uint8_t v);
    void cmp(uint8_t v);                     // like sub but discards result
    uint8_t inr(uint8_t v);                  // ++, preserves carry
    uint8_t dcr(uint8_t v);                  // --, preserves carry
    void dad(uint16_t v);                    // HL += v, only touches carry
    void daa();
    void rlc(); void rrc(); void ral(); void rar();

    // control-flow helpers
    void jump_if(bool cond);
    void call_if(bool cond, int &extra_cycles);
    void ret_if(bool cond, int &extra_cycles);

    // 8-bit register file access by 3-bit opcode field (B,C,D,E,H,L,M,A).
    uint8_t  get_r(int idx);
    void     set_r(int idx, uint8_t v);
};

} // namespace i8080

#endif // EMULATOR8080_I8080_H
