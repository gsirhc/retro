// Emscripten wrapper for the Namco Galaxian (1979) arcade board.

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

galaxian::RomSet hwtest_set() {
    galaxian::RomSet s;
    std::copy(galaxian::hwtest::program.begin(), galaxian::hwtest::program.end(), s.program.begin());
    std::copy(galaxian::hwtest::gfx.begin(), galaxian::hwtest::gfx.end(), s.gfx.begin());
    std::copy(galaxian::hwtest::color_prom.begin(), galaxian::hwtest::color_prom.end(),
              s.color_prom.begin());
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

    void loadRomSet(val prog, val gfx, val color) {
        galaxian::RomSet s;
        copy_val(prog, s.program.data(), s.program.size());
        copy_val(gfx, s.gfx.data(), s.gfx.size());
        copy_val(color, s.color_prom.data(), s.color_prom.size());
        m_.load_roms(s);
        m_.reset();
    }

    int runCycles(int n) { return m_.run_cycles(n); }

    val frameBuffer() {
        std::array<uint32_t, galaxian::kUprightW * galaxian::kUprightH> rgb{};
        m_.render(rgb.data());
        const int n = galaxian::kUprightW * galaxian::kUprightH * 4;
        val out = val::global("Uint8ClampedArray").new_(n);
        std::vector<uint8_t> rgba(static_cast<size_t>(n));
        for (int i = 0; i < galaxian::kUprightW * galaxian::kUprightH; i++) {
            uint32_t p = rgb[unsigned(i)];
            rgba[unsigned(i * 4 + 0)] = uint8_t(p >> 16);
            rgba[unsigned(i * 4 + 1)] = uint8_t(p >> 8);
            rgba[unsigned(i * 4 + 2)] = uint8_t(p);
            rgba[unsigned(i * 4 + 3)] = 255;
        }
        out.call<void>("set", val(emscripten::typed_memory_view(rgba.size(), rgba.data())));
        return out;
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
        s.set("nmiEnable", m_.nmi_enable);
        return s;
    }

    double totalCycles() const { return double(m_.cpu.cycles); }
    int screenWidth() const { return galaxian::kUprightW; }
    int screenHeight() const { return galaxian::kUprightH; }
    int ramByte(int addr) const {
        if (addr < 0x4000 || addr >= 0x4400) return -1;
        return m_.ram[unsigned(addr - 0x4000)];
    }
    int memRead(int addr) { return m_.mem_read(uint16_t(addr & 0xFFFF)); }

private:
    galaxian::Machine m_;
};

EMSCRIPTEN_BINDINGS(galaxian) {
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
        .function("memRead", &Machine::memRead);
}
