// Emscripten wrapper for the Midway Space Invaders (1978) arcade board.

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "../machine.h"
#include "arcade_frame.h"
#include "hwtest_roms.h"
#include "../hwtest_set.h"

using emscripten::val;

class Machine {
public:
    Machine() {
        m_.load_roms(invaders::hwtest_set());
        m_.reset();
    }

    void reset() { m_.reset(); }

    void loadHwtest() {
        m_.load_roms(invaders::hwtest_set());
        m_.reset();
    }

    void loadRomSet(val prog) {
        invaders::RomSet s;
        auto v = emscripten::convertJSArrayToNumberVector<uint8_t>(prog);
        std::copy_n(v.begin(), std::min(v.size(), s.program.size()), s.program.begin());
        m_.load_roms(s);
        m_.reset();
    }

    int runCycles(int n) { return m_.run_cycles(n); }

    val frameBuffer() {
        m_.render(frame_.clear());
        return frame_.rgba();
    }

    void setIn0(int v) { m_.inputs.in0 = uint8_t(v); }
    void setIn1(int v) { m_.inputs.in1 = uint8_t(v); }
    void setIn2(int v) { m_.inputs.in2 = uint8_t(v); }
    void setAudioHz(int hz) { m_.audio_hz = hz; }
    int in0() const { return m_.inputs.in0; }
    int in1() const { return m_.inputs.in1; }
    int in2() const { return m_.inputs.in2; }

    val drainAudio() {
        val out = val::global("Float32Array").new_(m_.audio.size());
        if (!m_.audio.empty())
            out.call<void>("set",
                           val(emscripten::typed_memory_view(m_.audio.size(), m_.audio.data())));
        m_.audio.clear();
        return out;
    }

    val state() const {
        val s = val::object();
        s.set("pc", int(m_.cpu.pc));
        s.set("sp", int(m_.cpu.sp));
        s.set("frames", m_.frames);
        s.set("line", m_.line());
        s.set("amp", m_.sound.amp_enabled());
        s.set("watchdogReset", m_.watchdog_reset);
        return s;
    }

    double totalCycles() const { return double(m_.cpu.cycles); }
    int screenWidth() const { return invaders::kUprightW; }
    int screenHeight() const { return invaders::kUprightH; }
    int ramByte(int addr) const {
        if (addr < 0x2000 || addr >= 0x4000) return -1;
        return m_.ram[unsigned(addr - 0x2000)];
    }
    void setRamByte(int addr, int v) {
        if (addr < 0x2000 || addr >= 0x4000) return;
        m_.ram[unsigned(addr - 0x2000)] = uint8_t(v);
    }
    void portOut(int port, int v) { m_.port_out(uint8_t(port), uint8_t(v)); }

private:
    invaders::Machine m_;
    arcade::RgbaFrame<invaders::kUprightW, invaders::kUprightH> frame_;
};

EMSCRIPTEN_BINDINGS(invaders) {
    emscripten::class_<Machine>("Machine")
        .constructor<>()
        .function("reset", &Machine::reset)
        .function("loadHwtest", &Machine::loadHwtest)
        .function("loadRomSet", &Machine::loadRomSet)
        .function("runCycles", &Machine::runCycles)
        .function("frameBuffer", &Machine::frameBuffer)
        .function("setIn0", &Machine::setIn0)
        .function("setIn1", &Machine::setIn1)
        .function("setIn2", &Machine::setIn2)
        .function("setAudioHz", &Machine::setAudioHz)
        .function("in0", &Machine::in0)
        .function("in1", &Machine::in1)
        .function("in2", &Machine::in2)
        .function("drainAudio", &Machine::drainAudio)
        .function("state", &Machine::state)
        .function("totalCycles", &Machine::totalCycles)
        .function("screenWidth", &Machine::screenWidth)
        .function("screenHeight", &Machine::screenHeight)
        .function("ramByte", &Machine::ramByte)
        .function("setRamByte", &Machine::setRamByte)
        .function("portOut", &Machine::portOut);
}
