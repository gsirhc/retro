// Emscripten wrapper for the Pac-Man arcade board.
//
//   const m = new Module.Machine();
//   m.runCycles(50688);
//   const rgba = m.frameBuffer();  // Uint8ClampedArray 224*288*4
//   m.setIn0(0xFE);
//   m.loadRomSet(prog, tiles, sprites, color, lookup, wave);

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstdint>
#include <vector>

#include "../machine.h"
#include "hwtest_roms.h"

using emscripten::val;

namespace {

pacman::RomSet hwtest_set() {
    pacman::RomSet s;
    std::copy(pacman::hwtest::program.begin(), pacman::hwtest::program.end(), s.program.begin());
    std::copy(pacman::hwtest::tiles.begin(), pacman::hwtest::tiles.end(), s.tiles.begin());
    std::copy(pacman::hwtest::sprites.begin(), pacman::hwtest::sprites.end(), s.sprites.begin());
    std::copy(pacman::hwtest::color_prom.begin(), pacman::hwtest::color_prom.end(), s.color_prom.begin());
    std::copy(pacman::hwtest::lookup_prom.begin(), pacman::hwtest::lookup_prom.end(), s.lookup_prom.begin());
    std::copy(pacman::hwtest::wave_prom.begin(), pacman::hwtest::wave_prom.end(), s.wave_prom.begin());
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

    void reset() {
        m_.reset();
    }

    void loadHwtest() {
        m_.load_roms(hwtest_set());
        m_.reset();
    }

    void loadRomSet(val prog, val tiles, val sprites, val color, val lookup, val wave) {
        pacman::RomSet s;
        copy_val(prog, s.program.data(), s.program.size());
        copy_val(tiles, s.tiles.data(), s.tiles.size());
        copy_val(sprites, s.sprites.data(), s.sprites.size());
        copy_val(color, s.color_prom.data(), s.color_prom.size());
        copy_val(lookup, s.lookup_prom.data(), s.lookup_prom.size());
        copy_val(wave, s.wave_prom.data(), s.wave_prom.size());
        m_.load_roms(s);
        m_.reset();
    }

    int runCycles(int n) { return m_.run_cycles(n); }

    val frameBuffer() {
        std::array<uint32_t, pacman::kUprightW * pacman::kUprightH> rgb{};
        m_.render(rgb.data());
        const int n = pacman::kUprightW * pacman::kUprightH * 4;
        val out = val::global("Uint8ClampedArray").new_(n);
        std::vector<uint8_t> rgba(static_cast<size_t>(n));
        for (int i = 0; i < pacman::kUprightW * pacman::kUprightH; i++) {
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
    // The host AudioContext's actual sample rate varies by device (commonly
    // but not always 48 kHz); Wsg::advance resamples its 96 kHz WSG clock to
    // whatever rate this is set to, so playback pitch/speed always matches
    // the real output device instead of assuming a fixed rate.
    void setAudioHz(int hz) { m_.audio_hz = hz; }
    int in0() const { return m_.inputs.in0; }
    int in1() const { return m_.inputs.in1; }

    val drainAudio() {
        val out = val::global("Float32Array").new_(m_.audio.size());
        if (!m_.audio.empty())
            out.call<void>("set", val(emscripten::typed_memory_view(m_.audio.size(), m_.audio.data())));
        m_.audio.clear();
        return out;
    }

    val state() const {
        val s = val::object();
        s.set("pc", cpu_pc());
        s.set("sp", int(m_.cpu.sp));
        s.set("a", int(m_.cpu.a));
        s.set("cycles", double(m_.cpu.cycles));
        s.set("frames", m_.frames);
        s.set("irqEnable", m_.irq_enable);
        s.set("halted", m_.cpu.halted);
        return s;
    }

    double totalCycles() const { return double(m_.cpu.cycles); }
    int screenWidth() const { return pacman::kUprightW; }
    int screenHeight() const { return pacman::kUprightH; }
    int ramByte(int addr) const {
        if (addr < 0x4800 || addr >= 0x5000) return -1;
        return m_.ram[unsigned(addr - 0x4800)];
    }

private:
    pacman::Machine m_;
    int cpu_pc() const { return m_.cpu.pc; }
};

EMSCRIPTEN_BINDINGS(pacman) {
    emscripten::class_<Machine>("Machine")
        .constructor<>()
        .function("reset", &Machine::reset)
        .function("loadHwtest", &Machine::loadHwtest)
        .function("loadRomSet", &Machine::loadRomSet)
        .function("runCycles", &Machine::runCycles)
        .function("frameBuffer", &Machine::frameBuffer)
        .function("setIn0", &Machine::setIn0)
        .function("setIn1", &Machine::setIn1)
        .function("setAudioHz", &Machine::setAudioHz)
        .function("in0", &Machine::in0)
        .function("in1", &Machine::in1)
        .function("drainAudio", &Machine::drainAudio)
        .function("state", &Machine::state)
        .function("totalCycles", &Machine::totalCycles)
        .function("screenWidth", &Machine::screenWidth)
        .function("screenHeight", &Machine::screenHeight)
        .function("ramByte", &Machine::ramByte);
}
