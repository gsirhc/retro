// Emscripten wrapper for the Battlezone arcade board.

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
    size_t m = std::min(v.size(), n);
    std::copy(v.begin(), v.begin() + std::ptrdiff_t(m), dst);
}

}  // namespace

class Machine {
public:
    Machine() { m_.load_roms(battlezone::hwtest_set()); }

    void reset() { m_.reset(); }
    void loadHwtest() { m_.load_roms(battlezone::hwtest_set()); }

    // ucode is the six 256-nibble PROMs back to back, M1 first.
    void loadRomSet(val program, val vector, val map, val ucode) {
        battlezone::RomSet s;
        copy_val(program, s.program.data(), s.program.size());
        copy_val(vector, s.vector.data(), s.vector.size());
        copy_val(map, s.mb_map.data(), s.mb_map.size());
        std::array<uint8_t, 6 * 256> u{};
        copy_val(ucode, u.data(), u.size());
        for (size_t p = 0; p < 6; p++)
            std::copy_n(u.begin() + std::ptrdiff_t(p * 256), 256, s.mb_ucode[p].begin());
        m_.load_roms(s);
    }

    int runCycles(int n) { return m_.run_cycles(n); }

    val frameBuffer() {
        static std::array<uint32_t, battlezone::kFbW * battlezone::kFbH> rgb{};
        m_.render(rgb.data());
        return arcade::rgba_view(rgb.data(), rgb.size());
    }

    // Packed [x0,y0,x1,y1,z, ...] in AVG picture units (y down).
    val vectorSegments() {
        const auto& segs = m_.avg.frame;
        out_.clear();
        out_.reserve(segs.size() * 5);
        for (const auto& s : segs) {
            out_.push_back(s.x0);
            out_.push_back(s.y0);
            out_.push_back(s.x1);
            out_.push_back(s.y1);
            out_.push_back(float(s.z));
        }
        val arr = val::global("Float32Array").new_(int(out_.size()));
        if (!out_.empty()) arr.call<void>("set", val(emscripten::typed_memory_view(out_.size(), out_.data())));
        return arr;
    }

    int screenWidth() const { return battlezone::kFbW; }
    int screenHeight() const { return battlezone::kFbH; }
    double overlaySplit() const { return battlezone::kOverlaySplit; }

    void setIn0(uint8_t v) { m_.inputs.in0 = v; }
    void setIn3(uint8_t v) { m_.inputs.in3 = v; }
    void setDsw0(uint8_t v) { m_.inputs.dsw0 = v; }
    void setDsw1(uint8_t v) { m_.inputs.dsw1 = v; }
    uint8_t in0() const { return m_.inputs.in0; }
    uint8_t in3() const { return m_.inputs.in3; }
    uint8_t dsw0() const { return m_.inputs.dsw0; }
    uint8_t dsw1() const { return m_.inputs.dsw1; }
    bool startLamp() const { return m_.start_lamp(); }
    uint8_t soundLatch() const { return m_.sound.latch; }

    void setAudioHz(int hz) { m_.audio_hz = hz; }
    val drainAudio() {
        val out = val::global("Float32Array").new_(int(m_.audio.size()));
        if (!m_.audio.empty()) out.call<void>("set", val(emscripten::typed_memory_view(m_.audio.size(), m_.audio.data())));
        m_.audio.clear();
        return out;
    }

    int frames() const { return m_.nmis; }
    int avgFrames() const { return m_.avg.frames; }
    double mathboxSteps() const { return double(m_.mathbox.steps); }
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

private:
    battlezone::Machine m_;
    std::vector<float> out_;
};

EMSCRIPTEN_BINDINGS(battlezone_arcade) {
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
        .function("overlaySplit", &Machine::overlaySplit)
        .function("setIn0", &Machine::setIn0)
        .function("setIn3", &Machine::setIn3)
        .function("setDsw0", &Machine::setDsw0)
        .function("setDsw1", &Machine::setDsw1)
        .function("in0", &Machine::in0)
        .function("in3", &Machine::in3)
        .function("dsw0", &Machine::dsw0)
        .function("dsw1", &Machine::dsw1)
        .function("startLamp", &Machine::startLamp)
        .function("soundLatch", &Machine::soundLatch)
        .function("setAudioHz", &Machine::setAudioHz)
        .function("drainAudio", &Machine::drainAudio)
        .function("frames", &Machine::frames)
        .function("avgFrames", &Machine::avgFrames)
        .function("mathboxSteps", &Machine::mathboxSteps)
        .function("watchdogReset", &Machine::watchdogReset)
        .function("totalCycles", &Machine::totalCycles)
        .function("ramByte", &Machine::ramByte)
        .function("setRamByte", &Machine::setRamByte);
}
