// Emscripten wrapper for the Centipede arcade board.

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "../machine.h"
#include "hwtest_roms.h"

using emscripten::val;

namespace {

centipede::RomSet hwtest_set() {
    centipede::RomSet s;
    std::copy(centipede::hwtest::program.begin(), centipede::hwtest::program.end(),
              s.program.begin());
    std::copy(centipede::hwtest::gfx.begin(), centipede::hwtest::gfx.end(),
              s.gfx.begin());
    return s;
}

void copy_val(val src, uint8_t* dst, size_t n) {
    auto v = emscripten::convertJSArrayToNumberVector<uint8_t>(src);
    size_t m = v.size() < n ? v.size() : n;
    std::copy(v.begin(), v.begin() + std::ptrdiff_t(m), dst);
}

}  // namespace

class Machine {
public:
    Machine() {
        m_.load_roms(hwtest_set());
        m_.reset();
    }

    void reset() { m_.reset(); }

    void loadHwtest() {
        m_.load_roms(hwtest_set());
        m_.reset();
    }

    void loadRomSet(val prog, val gfx) {
        centipede::RomSet s;
        copy_val(prog, s.program.data(), s.program.size());
        copy_val(gfx, s.gfx.data(), s.gfx.size());
        m_.load_roms(s);
        m_.reset();
    }

    int runCycles(int n) {
        int done = m_.run_cycles(n);
        total_ += done;
        return done;
    }

    val frameBuffer() {
        static std::array<uint32_t, centipede::kUprightW * centipede::kUprightH> rgb{};
        m_.render(rgb.data());
        const int n = centipede::kUprightW * centipede::kUprightH * 4;
        val out = val::global("Uint8ClampedArray").new_(n);
        auto* p = reinterpret_cast<uint8_t*>(rgb.data());
        out.call<void>("set", val(emscripten::typed_memory_view(size_t(n), p)));
        return out;
    }

    int screenWidth() const { return centipede::kUprightW; }
    int screenHeight() const { return centipede::kUprightH; }

    void setTrackX(uint8_t v) { m_.inputs.track_x = v; }
    void setTrackY(uint8_t v) { m_.inputs.track_y = v; }
    void setIn0Switches(uint8_t v) { m_.inputs.in0_switches = v; }
    void setIn1(uint8_t v) { m_.inputs.in1 = v; }
    void setIn3(uint8_t v) { m_.inputs.in3 = v; }
    void setDsw1(uint8_t v) { m_.inputs.dsw1 = v; }
    void setDsw2(uint8_t v) { m_.inputs.dsw2 = v; }
    uint8_t in0Switches() const { return m_.inputs.in0_switches; }
    uint8_t trackX() const { return m_.inputs.track_x; }
    uint8_t trackY() const { return m_.inputs.track_y; }
    uint8_t in1() const { return m_.inputs.in1; }
    uint8_t dsw1() const { return m_.inputs.dsw1; }
    uint8_t dsw2() const { return m_.inputs.dsw2; }

    void setAudioHz(int hz) { m_.audio_hz = hz; }

    val drainAudio() {
        val out = val::global("Float32Array").new_(int(m_.audio.size()));
        if (!m_.audio.empty()) {
            out.call<void>("set",
                           val(emscripten::typed_memory_view(m_.audio.size(), m_.audio.data())));
        }
        m_.audio.clear();
        return out;
    }

    int frames() const { return m_.frames; }
    bool watchdogReset() const { return m_.watchdog_reset; }
    double totalCycles() const { return total_; }

    uint8_t ramByte(int addr) const {
        if (addr < 0 || addr > 0xFFFF) return 0;
        return m_.mem_read(uint16_t(addr));
    }
    void setRamByte(int addr, uint8_t v) {
        if (addr < 0 || addr > 0xFFFF) return;
        m_.mem_write(uint16_t(addr), v);
    }

    val earomBytes() const {
        val out = val::global("Uint8Array").new_(int(m_.earom.size()));
        out.call<void>("set",
                       val(emscripten::typed_memory_view(m_.earom.size(), m_.earom.data())));
        return out;
    }
    void setEaromBytes(val data) {
        auto v = emscripten::convertJSArrayToNumberVector<uint8_t>(data);
        m_.set_earom_bytes(v.data(), v.size());
    }
    void setEaromByte(int i, uint8_t v) {
        if (i < 0 || i >= int(m_.earom.size())) return;
        m_.earom[unsigned(i)] = v;
    }

private:
    centipede::Machine m_;
    double total_ = 0;
};

EMSCRIPTEN_BINDINGS(centipede_arcade) {
    emscripten::class_<Machine>("Machine")
        .constructor<>()
        .function("reset", &Machine::reset)
        .function("loadHwtest", &Machine::loadHwtest)
        .function("loadRomSet", &Machine::loadRomSet)
        .function("runCycles", &Machine::runCycles)
        .function("frameBuffer", &Machine::frameBuffer)
        .function("screenWidth", &Machine::screenWidth)
        .function("screenHeight", &Machine::screenHeight)
        .function("setTrackX", &Machine::setTrackX)
        .function("setTrackY", &Machine::setTrackY)
        .function("setIn0Switches", &Machine::setIn0Switches)
        .function("setIn1", &Machine::setIn1)
        .function("setIn3", &Machine::setIn3)
        .function("setDsw1", &Machine::setDsw1)
        .function("setDsw2", &Machine::setDsw2)
        .function("in0Switches", &Machine::in0Switches)
        .function("trackX", &Machine::trackX)
        .function("trackY", &Machine::trackY)
        .function("in1", &Machine::in1)
        .function("dsw1", &Machine::dsw1)
        .function("dsw2", &Machine::dsw2)
        .function("setAudioHz", &Machine::setAudioHz)
        .function("drainAudio", &Machine::drainAudio)
        .function("frames", &Machine::frames)
        .function("watchdogReset", &Machine::watchdogReset)
        .function("totalCycles", &Machine::totalCycles)
        .function("ramByte", &Machine::ramByte)
        .function("setRamByte", &Machine::setRamByte)
        .function("earomBytes", &Machine::earomBytes)
        .function("setEaromBytes", &Machine::setEaromBytes)
        .function("setEaromByte", &Machine::setEaromByte);
}
