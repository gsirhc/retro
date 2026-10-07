// Emscripten wrapper for the Asteroids arcade board.

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

asteroids::RomSet hwtest_set() {
    asteroids::RomSet s;
    std::copy(asteroids::hwtest::program.begin(), asteroids::hwtest::program.end(),
              s.program.begin());
    std::copy(asteroids::hwtest::vector.begin(), asteroids::hwtest::vector.end(),
              s.vector.begin());
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

    void loadRomSet(val prog, val vector) {
        asteroids::RomSet s;
        copy_val(prog, s.program.data(), s.program.size());
        copy_val(vector, s.vector.data(), s.vector.size());
        m_.load_roms(s);
        m_.reset();
    }

    int runCycles(int n) { return m_.run_cycles(n); }

    val frameBuffer() {
        // static: 4MB is far past the wasm stack.
        static std::array<uint32_t, asteroids::kFbW * asteroids::kFbH> rgb{};
        m_.render(rgb.data());
        const int n = asteroids::kFbW * asteroids::kFbH * 4;
        val out = val::global("Uint8ClampedArray").new_(n);
        auto* p = reinterpret_cast<uint8_t*>(rgb.data());
        // One bulk copy, a per-byte val::set() loop starves the cycle budget.
        out.call<void>("set", val(emscripten::typed_memory_view(size_t(n), p)));
        return out;
    }

    // Packed [x0,y0,x1,y1,intensity, ...] in DVG space (y up).
    val vectorSegments() {
        const auto& segs = m_.dvg.segments;
        std::vector<float> packed;
        packed.reserve(segs.size() * 5);
        for (const auto& s : segs) {
            if (s.intensity == 0) continue;
            packed.push_back(float(s.x0));
            packed.push_back(float(s.y0));
            packed.push_back(float(s.x1));
            packed.push_back(float(s.y1));
            packed.push_back(float(s.intensity));
        }
        val out = val::global("Float32Array").new_(int(packed.size()));
        if (!packed.empty()) {
            out.call<void>("set", val(emscripten::typed_memory_view(packed.size(), packed.data())));
        }
        return out;
    }

    int screenWidth() const { return asteroids::kFbW; }
    int screenHeight() const { return asteroids::kFbH; }

    void setIn0(uint8_t v) { m_.inputs.in0 = v; }
    void setIn1(uint8_t v) { m_.inputs.in1 = v; }
    void setDsw1(uint8_t v) { m_.inputs.dsw1 = v; }
    uint8_t in0() const { return m_.inputs.in0; }
    uint8_t in1() const { return m_.inputs.in1; }
    uint8_t dsw1() const { return m_.inputs.dsw1; }

    void setAudioHz(int hz) { m_.audio_hz = hz; }

    val drainAudio() {
        val out = val::global("Float32Array").new_(int(m_.audio.size()));
        for (size_t i = 0; i < m_.audio.size(); i++) out.set(i, m_.audio[i]);
        m_.audio.clear();
        return out;
    }

    int frames() const { return m_.frames; }
    bool watchdogReset() const { return m_.watchdog_reset; }
    double totalCycles() const { return double(m_.cpu.cycles); }

    uint8_t ramByte(int addr) const {
        if (addr < 0 || addr > 0xFFFF) return 0;
        return m_.mem_read(uint16_t(addr));
    }
    void setRamByte(int addr, uint8_t v) {
        if (addr < 0 || addr > 0xFFFF) return;
        m_.mem_write(uint16_t(addr), v);
    }

    // Test seam.
    void __testSetHaltBusy(bool busy) { m_.dvg.halt = !busy; }

private:
    asteroids::Machine m_;
};

EMSCRIPTEN_BINDINGS(asteroids_arcade) {
    emscripten::class_<Machine>("Machine")
        .constructor<>()
        .function("reset", &Machine::reset)
        .function("loadHwtest", &Machine::loadHwtest)
        .function("loadRomSet", &Machine::loadRomSet)
        .function("runCycles", &Machine::runCycles)
        .function("frameBuffer", &Machine::frameBuffer)
        .function("vectorSegments", &Machine::vectorSegments)
        .function("screenWidth", &Machine::screenWidth)
        .function("screenHeight", &Machine::screenHeight)
        .function("setIn0", &Machine::setIn0)
        .function("setIn1", &Machine::setIn1)
        .function("setDsw1", &Machine::setDsw1)
        .function("in0", &Machine::in0)
        .function("in1", &Machine::in1)
        .function("dsw1", &Machine::dsw1)
        .function("setAudioHz", &Machine::setAudioHz)
        .function("drainAudio", &Machine::drainAudio)
        .function("frames", &Machine::frames)
        .function("watchdogReset", &Machine::watchdogReset)
        .function("totalCycles", &Machine::totalCycles)
        .function("ramByte", &Machine::ramByte)
        .function("setRamByte", &Machine::setRamByte);
}
