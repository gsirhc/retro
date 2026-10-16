// Emscripten wrapper for the Pac-Man arcade board.

#include <emscripten/bind.h>
#include <emscripten/val.h>

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
        m_.load_roms(pacman::hwtest_set());
        m_.reset();
    }

    void reset() {
        m_.reset();
    }

    void loadHwtest() {
        m_.load_roms(pacman::hwtest_set());
        m_.reset();
    }

    void loadMsHwtest() {
        pacman::RomSet s = pacman::hwtest_set();
        std::copy(pacman::hwtest::mspacman_program.begin(),
                  pacman::hwtest::mspacman_program.end(), s.program.begin());
        m_.load_roms(s);
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

    void loadMsPacmanSet(val prog, val tiles, val sprites, val color, val lookup, val wave,
                         val u5, val u6, val u7) {
        pacman::RomSet s;
        copy_val(prog, s.program.data(), s.program.size());
        copy_val(tiles, s.tiles.data(), s.tiles.size());
        copy_val(sprites, s.sprites.data(), s.sprites.size());
        copy_val(color, s.color_prom.data(), s.color_prom.size());
        copy_val(lookup, s.lookup_prom.data(), s.lookup_prom.size());
        copy_val(wave, s.wave_prom.data(), s.wave_prom.size());
        copy_val(u5, s.aux_u5.data(), s.aux_u5.size());
        copy_val(u6, s.aux_u6.data(), s.aux_u6.size());
        copy_val(u7, s.aux_u7.data(), s.aux_u7.size());
        s.aux_board = true;
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
    void setDsw1(int v) { m_.inputs.dsw1 = uint8_t(v); }
    // Output device rate varies, Wsg::advance resamples its 96 kHz clock to it.
    void setAudioHz(int hz) { m_.audio_hz = hz; }
    int in0() const { return m_.inputs.in0; }
    int in1() const { return m_.inputs.in1; }
    int dsw1() const { return m_.inputs.dsw1; }

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
        if (addr < 0x4C00 || addr >= 0x5000) return -1;
        return m_.ram[unsigned(addr - 0x4C00)];
    }

    // Work RAM only, sprite RAM is $4FF0-$4FFF.
    void setRamByte(int addr, int v) {
        if (addr < 0x4C00 || addr >= 0x4FF0) return;
        m_.ram[unsigned(addr - 0x4C00)] = uint8_t(v);
    }
    void setMemByte(int addr, int v) {
        m_.mem_write(uint16_t(addr & 0xFFFF), uint8_t(v));
    }

    int memRead(int addr) { return m_.mem_read(uint16_t(addr & 0xFFFF)); }
    bool auxBoard() const { return m_.aux_board; }
    bool auxDecode() const { return m_.aux_decode; }

private:
    pacman::Machine m_;
    arcade::RgbaFrame<pacman::kUprightW, pacman::kUprightH> frame_;
    int cpu_pc() const { return m_.cpu.pc; }
};

EMSCRIPTEN_BINDINGS(pacman) {
    emscripten::class_<Machine>("Machine")
        .constructor<>()
        .function("reset", &Machine::reset)
        .function("loadHwtest", &Machine::loadHwtest)
        .function("loadMsHwtest", &Machine::loadMsHwtest)
        .function("loadRomSet", &Machine::loadRomSet)
        .function("loadMsPacmanSet", &Machine::loadMsPacmanSet)
        .function("runCycles", &Machine::runCycles)
        .function("frameBuffer", &Machine::frameBuffer)
        .function("setIn0", &Machine::setIn0)
        .function("setIn1", &Machine::setIn1)
        .function("setDsw1", &Machine::setDsw1)
        .function("setAudioHz", &Machine::setAudioHz)
        .function("in0", &Machine::in0)
        .function("in1", &Machine::in1)
        .function("dsw1", &Machine::dsw1)
        .function("drainAudio", &Machine::drainAudio)
        .function("state", &Machine::state)
        .function("totalCycles", &Machine::totalCycles)
        .function("screenWidth", &Machine::screenWidth)
        .function("screenHeight", &Machine::screenHeight)
        .function("ramByte", &Machine::ramByte)
        .function("setRamByte", &Machine::setRamByte)
        .function("setMemByte", &Machine::setMemByte)
        .function("memRead", &Machine::memRead)
        .function("auxBoard", &Machine::auxBoard)
        .function("auxDecode", &Machine::auxDecode);
}
