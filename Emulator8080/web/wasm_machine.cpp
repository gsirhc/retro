// Emscripten wrapper: binds the 8080 core + 88-2SIO board into one `Machine`
// object the browser can drive. Build with `make` in this directory (needs the
// emsdk toolchain on PATH).
//
// JS surface (all via embind):
//   const m = new Module.Machine();
//   m.reset();
//   m.loadBytes(uint8Array, 0x0000);   // drop a ROM / program into memory
//   m.sendByte(0x41);                   // terminal -> serial channel A
//   m.runCycles(33333);                 // advance ~1 frame at 2 MHz
//   const out = m.readOutput();         // Uint8Array the CPU transmitted
//   const s   = m.state();              // { a,b,c,...,pc,sp,halted,cycles }

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <array>
#include <cstdint>
#include <vector>

#include "../i8080.h"
#include "../serial2sio.h"

using emscripten::val;

namespace {

// A tiny built-in program so the page does something before a ROM is loaded:
// reset ACIA channel A, configure 8N1, then echo every received byte.
const uint8_t kEchoRom[] = {
    0x3E, 0x03, 0xD3, 0x10,                    // MVI A,03 / OUT 10   master reset
    0x3E, 0x11, 0xD3, 0x10,                    // MVI A,11 / OUT 10   8N1, /16, IRQ off
    0xDB, 0x10, 0xE6, 0x01, 0xCA, 0x08, 0x00,  // wait RDRF
    0xDB, 0x11, 0x47,                          // IN 11 / MOV B,A
    0xDB, 0x10, 0xE6, 0x02, 0xCA, 0x12, 0x00,  // wait TDRE
    0x78, 0xD3, 0x11,                          // MOV A,B / OUT 11
    0xC3, 0x08, 0x00,                          // JMP loop
};

} // namespace

class Machine {
public:
    Machine() : cpu_(make_bus()) {
        load_default_rom();
        cpu_.reset();
    }

    // Wipe RAM and re-seed the built-in echo program.
    void reset() {
        mem_.fill(0);
        load_default_rom();
        sio_.reset();
        cpu_.reset();
    }

    // Clear RAM without seeding anything (call before loadBytes for a real ROM).
    void clearMemory() { mem_.fill(0); }

    // Copy bytes from a JS Uint8Array (or array) into memory at `addr`.
    void loadBytes(val bytes, int addr) {
        const unsigned len = bytes["length"].as<unsigned>();
        for (unsigned i = 0; i < len; ++i) {
            const unsigned a = static_cast<unsigned>(addr) + i;
            if (a >= mem_.size()) break;
            mem_[a] = bytes[i].as<uint8_t>();
        }
    }

    void reboot() {
        sio_.reset();
        cpu_.reset();
    }

    // Point the program counter at a program's entry (call after loadBytes +
    // reboot). CP/M .COM images start at 0x0100; most ROM images at 0x0000.
    void setPC(int addr) { cpu_.pc = static_cast<uint16_t>(addr & 0xFFFF); }

    // Front-panel sense switches, read via IN 0FFh. Altair BASIC checks these
    // at cold start to pick the console device; 0 selects the 2SIO.
    void setSenseSwitches(int v) { sense_ = static_cast<uint8_t>(v & 0xFF); }

    // Terminal -> serial channel A receive FIFO.
    void sendByte(int byte) { sio_.host_send(static_cast<uint8_t>(byte & 0xFF)); }

    // Everything the CPU has transmitted on channel A since the last call,
    // as a freshly allocated Uint8Array.
    val readOutput() {
        std::vector<uint8_t> bytes = sio_.host_drain();
        val out = val::global("Uint8Array").new_(bytes.size());
        if (!bytes.empty()) {
            out.call<void>(
                "set",
                val(emscripten::typed_memory_view(bytes.size(), bytes.data())));
        }
        return out;
    }

    // Run instructions until at least `cycles` T-states have elapsed.
    void runCycles(int cycles) {
        int64_t remaining = cycles;
        while (remaining > 0) remaining -= cpu_.step();
    }

    // Execute exactly one instruction (front-panel SINGLE STEP).
    int stepOne() { return cpu_.step(); }

    // Front-panel EXAMINE / DEPOSIT peek and poke.
    int  readByte(int addr)          { return mem_[addr & 0xFFFF]; }
    void writeByte(int addr, int v)  { mem_[addr & 0xFFFF] = static_cast<uint8_t>(v & 0xFF); }

    bool   halted()      const { return cpu_.halted; }
    double cycleCount()   const { return static_cast<double>(cpu_.cycles); }
    unsigned rxPending()  const { return static_cast<unsigned>(sio_.rx_pending()); }
    unsigned txPending()  const { return static_cast<unsigned>(sio_.tx_pending()); }

    val state() const {
        val o = val::object();
        o.set("a", cpu_.a);   o.set("b", cpu_.b);   o.set("c", cpu_.c);
        o.set("d", cpu_.d);   o.set("e", cpu_.e);
        o.set("h", cpu_.h);   o.set("l", cpu_.l);
        o.set("pc", cpu_.pc); o.set("sp", cpu_.sp);
        o.set("flags", cpu_.f);
        o.set("halted", cpu_.halted);
        o.set("intEnabled", cpu_.int_enabled);
        o.set("cycles", static_cast<double>(cpu_.cycles));
        return o;
    }

private:
    i8080::Bus make_bus() {
        i8080::Bus bus;
        bus.read  = [this](uint16_t a)            { return mem_[a]; };
        bus.write = [this](uint16_t a, uint8_t v) { mem_[a] = v; };
        bus.in    = [this](uint8_t port) -> uint8_t {
            if (sio_.owns(port)) return sio_.in(port);
            if (port == 0xFF)    return sense_;   // Altair front-panel sense switches
            return 0xFF;                          // unmapped port: floating bus
        };
        bus.out   = [this](uint8_t port, uint8_t v) { sio_.out(port, v); };
        return bus;
    }

    void load_default_rom() {
        for (unsigned i = 0; i < sizeof(kEchoRom); ++i) mem_[i] = kEchoRom[i];
    }

    // Declaration order matters: mem_ and sio_ are built before cpu_, whose
    // constructor calls make_bus() and captures them.
    std::array<uint8_t, 0x10000> mem_{};
    altair::Serial2SIO            sio_{0x10};
    uint8_t                       sense_ = 0x00;  // 0 => console on the 2SIO
    i8080::Cpu                    cpu_;
};

EMSCRIPTEN_BINDINGS(retro8080) {
    emscripten::class_<Machine>("Machine")
        .constructor<>()
        .function("reset",       &Machine::reset)
        .function("reboot",      &Machine::reboot)
        .function("clearMemory", &Machine::clearMemory)
        .function("loadBytes",   &Machine::loadBytes)
        .function("setPC",       &Machine::setPC)
        .function("setSenseSwitches", &Machine::setSenseSwitches)
        .function("sendByte",    &Machine::sendByte)
        .function("readOutput",  &Machine::readOutput)
        .function("runCycles",   &Machine::runCycles)
        .function("stepOne",     &Machine::stepOne)
        .function("readByte",    &Machine::readByte)
        .function("writeByte",   &Machine::writeByte)
        .function("halted",      &Machine::halted)
        .function("cycleCount",  &Machine::cycleCount)
        .function("rxPending",   &Machine::rxPending)
        .function("txPending",   &Machine::txPending)
        .function("state",       &Machine::state);
}
