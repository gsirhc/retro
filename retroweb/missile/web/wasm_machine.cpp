// Emscripten wrapper for the Missile Command arcade board.

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "../machine.h"
#include "arcade_frame.h"
#include "hwtest_roms.h"
#include "../hwtest_set.h"

using emscripten::val;

namespace {


void copy_val(val src, uint8_t* dst, size_t n) {
    auto v = emscripten::convertJSArrayToNumberVector<uint8_t>(src);
    size_t m = v.size() < n ? v.size() : n;
    std::copy(v.begin(), v.begin() + std::ptrdiff_t(m), dst);
}

}  // namespace

class Machine {
public:
    Machine() {
        m_.load_roms(missile::hwtest_set());
    }

    void reset() { m_.reset(); }

    void loadHwtest() { m_.load_roms(missile::hwtest_set()); }

    void loadRomSet(val prog, val prom) {
        missile::RomSet s;
        copy_val(prog, s.program.data(), s.program.size());
        if (!prom.isNull() && !prom.isUndefined()) {
            copy_val(prom, s.prom.data(), s.prom.size());
            s.has_prom = true;
        }
        m_.load_roms(s);
    }

    int runCycles(int n) {
        int done = m_.run_cycles(n);
        total_ += done;
        return done;
    }

    val frameBuffer() {
        static std::array<uint32_t, missile::kFbW * missile::kFbH> rgb{};
        m_.render(rgb.data());
        return arcade::rgba_view(rgb.data(), rgb.size());
    }

    int screenWidth() const { return missile::kFbW; }
    int screenHeight() const { return missile::kFbH; }

    void setTrackX(uint8_t v) { m_.inputs.track_x = v; }
    void setTrackY(uint8_t v) { m_.inputs.track_y = v; }
    void setIn0(uint8_t v) { m_.inputs.in0 = v; }
    void setIn1(uint8_t v) { m_.inputs.in1 = v; }
    void setR8(uint8_t v) { m_.inputs.r8 = v; }
    void setR10(uint8_t v) { m_.inputs.r10 = v; }
    uint8_t in0() const { return m_.inputs.in0; }
    uint8_t in1() const { return m_.inputs.in1; }
    uint8_t trackX() const { return m_.inputs.track_x; }
    uint8_t trackY() const { return m_.inputs.track_y; }
    uint8_t r8() const { return m_.inputs.r8; }
    uint8_t r10() const { return m_.inputs.r10; }

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
        if (addr < 0 || addr > 0x3FFF) return;
        m_.mem_write(uint16_t(addr), v);
    }

private:
    missile::Machine m_;
    double total_ = 0;
};

EMSCRIPTEN_BINDINGS(missile_arcade) {
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
        .function("setIn0", &Machine::setIn0)
        .function("setIn1", &Machine::setIn1)
        .function("setR8", &Machine::setR8)
        .function("setR10", &Machine::setR10)
        .function("in0", &Machine::in0)
        .function("in1", &Machine::in1)
        .function("trackX", &Machine::trackX)
        .function("trackY", &Machine::trackY)
        .function("r8", &Machine::r8)
        .function("r10", &Machine::r10)
        .function("setAudioHz", &Machine::setAudioHz)
        .function("drainAudio", &Machine::drainAudio)
        .function("frames", &Machine::frames)
        .function("watchdogReset", &Machine::watchdogReset)
        .function("totalCycles", &Machine::totalCycles)
        .function("ramByte", &Machine::ramByte)
        .function("setRamByte", &Machine::setRamByte);
}
