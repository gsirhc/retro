// Emscripten wrapper: binds machine::Machine into one `Machine` object the
// browser can drive. Build with `make` in this directory (needs the emsdk
// toolchain on PATH). Modeled on retroweb/altair8800/web/wasm_machine.cpp.
//
// JS surface (all via embind):
//   const m = new Module.Machine();
//   m.burnRom(uint8Array);          // seat a chip in the ZIF socket (instant, boot-time load)
//   m.pressReset();                  // SW1 / J4 -- re-arms the DS1813 hold
//   m.typeChar(0x41);                // terminal -> ACIA receive
//   m.runCycles(16667);              // advance ~1 frame at 1 MHz
//   const out = m.readOutput();      // Uint8Array the ACIA transmitted
//   const lcd = m.lcdText();         // 32-char string, row-major 16x2
//   const s   = m.state();           // { pc, a, x, y, sp, p, cycles, ... }

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstdint>
#include <vector>

#include "../machine.h"

using emscripten::val;

class Machine {
public:
    Machine() {
        m_.on_serial_out = [this](uint8_t b) { out_q_.push_back(b); };
        m_.on_rx_led = [this](bool) { rx_pulse_ = true; };
        m_.on_tx_led = [this](bool) { tx_pulse_ = true; };
    }

    // --- ROM programmer ("the ZIF socket") ------------------------------
    // Instant seat -- used for the default boot ROM and the "chip library"
    // swap, both of which are "pull the chip, put a different one in"
    // moments, not a programming operation. See programRom() for the
    // actual page-write-timed burn.
    void burnRom(val bytes) {
        std::vector<uint8_t> data = emscripten::convertJSArrayToNumberVector<uint8_t>(bytes);
        m_.bus.rom.load_image(data.data(), int(data.size()));
    }

    // Begin a realistic, page-write-timed program of `bytes` starting at
    // `addr` (see eeprom28c256.h). advanceProgram() drives it forward;
    // programBusy()/programProgress() report status for the UI.
    void programRom(int addr, val bytes) {
        program_buf_ = emscripten::convertJSArrayToNumberVector<uint8_t>(bytes);
        m_.bus.rom.begin_program(uint16_t(addr), program_buf_.data(), int(program_buf_.size()));
    }
    void advanceProgram(int us) { m_.bus.rom.advance(us); }
    bool programBusy() const { return m_.bus.rom.busy(); }
    double programProgress() const { return m_.bus.rom.progress(); }

    // Read back the chip's current contents (for the programmer's
    // "verify" step, or to save the running image as a named chip).
    val readRom() {
        const uint8_t *raw = m_.bus.rom.raw();
        val out = val::global("Uint8Array").new_(eeprom28c256::kSize);
        out.call<void>("set", val(emscripten::typed_memory_view(size_t(eeprom28c256::kSize), raw)));
        return out;
    }

    // --- reset / jumpers -------------------------------------------------
    void pressReset() { m_.press_reset(); }

    void setLcdAttached(bool a) { m_.set_lcd_attached(a); }
    bool lcdAttached() const { return m_.lcd_attached(); }

    // J7: 0 none, 1 IRQ, 2 NMI.
    void setViaIrqRoute(int r) { m_.bus.jumpers.via_irq_route = bus::JumperState::Route(r); }
    void setAciaIrqRoute(int r) { m_.bus.jumpers.acia_irq_route = bus::JumperState::Route(r); }
    void setRtsToCts(bool on) { m_.bus.jumpers.rts_to_rs232_cts = on; }
    void setBootSelect(int v) { m_.bus.jumpers.boot_select = uint8_t(v & 0x0F); }

    // --- terminal <-> ACIA -------------------------------------------------
    void typeChar(int byte) { m_.type_char(uint8_t(byte & 0xFF)); }

    val readOutput() {
        val out = val::global("Uint8Array").new_(out_q_.size());
        if (!out_q_.empty())
            out.call<void>("set", val(emscripten::typed_memory_view(out_q_.size(), out_q_.data())));
        out_q_.clear();
        return out;
    }

    int aciaBaud() const { return m_.bus.acia.baud(); }

    // --- run -------------------------------------------------------------
    void runCycles(int cycles) { m_.run_cycles(cycles); }
    double cycleCount() const { return double(m_.cycles()); }

    // --- LCD (J3 accessory) -----------------------------------------------
    // 32-char string, row-major (chars 0-15 = row 0, 16-31 = row 1).
    val lcdText() const {
        std::string s;
        s.reserve(32);
        for (auto &row : m_.lcd.text) s.append(row, 16);
        return val(s);
    }

    // --- LEDs: "since last poll" pulses, like the Altair bridge's
    // rxPending()/txPending() pattern ---------------------------------
    bool rxLedPulse() { bool p = rx_pulse_; rx_pulse_ = false; return p; }
    bool txLedPulse() { bool p = tx_pulse_; tx_pulse_ = false; return p; }
    bool contended() const { return m_.bus.last_access_was_contended; }

    val state() const {
        val o = val::object();
        o.set("pc", m_.cpu.pc);
        o.set("a", m_.cpu.a); o.set("x", m_.cpu.x); o.set("y", m_.cpu.y);
        o.set("sp", m_.cpu.sp); o.set("p", m_.cpu.p);
        o.set("waiting", m_.cpu.waiting);
        o.set("stopped", m_.cpu.stopped);
        return o;
    }

private:
    machine::Machine m_;
    std::vector<uint8_t> out_q_;
    std::vector<uint8_t> program_buf_;
    bool rx_pulse_ = false, tx_pulse_ = false;
};

EMSCRIPTEN_BINDINGS(cgoac6502) {
    emscripten::class_<Machine>("Machine")
        .constructor<>()
        .function("burnRom", &Machine::burnRom)
        .function("programRom", &Machine::programRom)
        .function("advanceProgram", &Machine::advanceProgram)
        .function("programBusy", &Machine::programBusy)
        .function("programProgress", &Machine::programProgress)
        .function("readRom", &Machine::readRom)
        .function("pressReset", &Machine::pressReset)
        .function("setLcdAttached", &Machine::setLcdAttached)
        .function("lcdAttached", &Machine::lcdAttached)
        .function("setViaIrqRoute", &Machine::setViaIrqRoute)
        .function("setAciaIrqRoute", &Machine::setAciaIrqRoute)
        .function("setRtsToCts", &Machine::setRtsToCts)
        .function("setBootSelect", &Machine::setBootSelect)
        .function("typeChar", &Machine::typeChar)
        .function("readOutput", &Machine::readOutput)
        .function("aciaBaud", &Machine::aciaBaud)
        .function("runCycles", &Machine::runCycles)
        .function("cycleCount", &Machine::cycleCount)
        .function("lcdText", &Machine::lcdText)
        .function("rxLedPulse", &Machine::rxLedPulse)
        .function("txLedPulse", &Machine::txLedPulse)
        .function("contended", &Machine::contended)
        .function("state", &Machine::state);
}
