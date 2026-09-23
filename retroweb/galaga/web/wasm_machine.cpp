// Emscripten wrapper for the Midway Galaga (1981) arcade board.

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

galaga::RomSet hwtest_set() {
    galaga::RomSet s;
    auto copy = [](const auto& src, auto& dst) {
        std::copy(src.begin(), src.end(), dst.begin());
    };
    copy(galaga::hwtest::main, s.main);
    copy(galaga::hwtest::sub, s.sub);
    copy(galaga::hwtest::sound, s.sound);
    copy(galaga::hwtest::tiles, s.tiles);
    copy(galaga::hwtest::sprites, s.sprites);
    copy(galaga::hwtest::palette, s.palette);
    copy(galaga::hwtest::char_lut, s.char_lut);
    copy(galaga::hwtest::sprite_lut, s.sprite_lut);
    copy(galaga::hwtest::wave, s.wave);
    return s;
}

void copy_val(val src, uint8_t* dst, size_t n) {
    if (src.isNull() || src.isUndefined()) return;
    auto v = emscripten::convertJSArrayToNumberVector<uint8_t>(src);
    size_t m = v.size() < n ? v.size() : n;
    std::copy(v.begin(), v.begin() + std::ptrdiff_t(m), dst);
}

size_t val_len(val src) {
    if (src.isNull() || src.isUndefined()) return 0;
    return src["length"].as<size_t>();
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

    void loadRomSet(val main, val sub, val sound, val tiles, val sprites,
                    val palette, val char_lut, val sprite_lut, val wave,
                    val mcu51, val mcu54) {
        galaga::RomSet s;
        copy_val(main, s.main.data(), s.main.size());
        copy_val(sub, s.sub.data(), s.sub.size());
        copy_val(sound, s.sound.data(), s.sound.size());
        copy_val(tiles, s.tiles.data(), s.tiles.size());
        copy_val(sprites, s.sprites.data(), s.sprites.size());
        copy_val(palette, s.palette.data(), s.palette.size());
        copy_val(char_lut, s.char_lut.data(), s.char_lut.size());
        copy_val(sprite_lut, s.sprite_lut.data(), s.sprite_lut.size());
        copy_val(wave, s.wave.data(), s.wave.size());
        if (val_len(mcu51) == galaga::kMcuRomBytes) {
            copy_val(mcu51, s.mcu51.data(), s.mcu51.size());
            s.has51 = true;
        }
        if (val_len(mcu54) == galaga::kMcuRomBytes) {
            copy_val(mcu54, s.mcu54.data(), s.mcu54.size());
            s.has54 = true;
        }
        m_.load_roms(s);
        m_.reset();
    }

    int runCycles(int n) { return m_.run_cycles(n); }

    val frameBuffer() {
        std::array<uint32_t, galaga::kUprightW * galaga::kUprightH> rgb{};
        m_.render(rgb.data());
        const int n = galaga::kUprightW * galaga::kUprightH * 4;
        val out = val::global("Uint8ClampedArray").new_(n);
        std::vector<uint8_t> rgba(static_cast<size_t>(n));
        for (int i = 0; i < galaga::kUprightW * galaga::kUprightH; i++) {
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
    void setDswA(int v) { m_.inputs.dsw_a = uint8_t(v); }
    void setDswB(int v) { m_.inputs.dsw_b = uint8_t(v); }
    void setAudioHz(int hz) { m_.audio_hz = hz; }
    int in0() const { return m_.inputs.in0; }
    int in1() const { return m_.inputs.in1; }
    int dswA() const { return m_.inputs.dsw_a; }
    int dswB() const { return m_.inputs.dsw_b; }
    bool mcu51Hle() const { return m_.mcu51_hle; }
    bool mcu54Hle() const { return m_.mcu54_hle; }

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
        s.set("pc", int(m_.main.pc));
        s.set("sp", int(m_.main.sp));
        s.set("frames", m_.frames);
        s.set("irq1Enable", m_.irq1_enable);
        s.set("mcu51Hle", m_.mcu51_hle);
        s.set("mcu54Hle", m_.mcu54_hle);
        return s;
    }

    double totalCycles() const { return double(m_.main.cycles); }
    int screenWidth() const { return galaga::kUprightW; }
    int screenHeight() const { return galaga::kUprightH; }

    int ramByte(int addr) const {
        if (addr >= 0x8000 && addr < 0x8800) return m_.video.videoram[addr - 0x8000];
        if (addr >= 0x8800 && addr < 0x9000) return m_.ram1[(addr - 0x8800) & 0x3FF];
        if (addr >= 0x9000 && addr < 0x9800) return m_.ram2[(addr - 0x9000) & 0x3FF];
        if (addr >= 0x9800 && addr < 0xA000) return m_.ram3[(addr - 0x9800) & 0x3FF];
        return -1;
    }
    void setRamByte(int addr, int v) {
        if (addr >= 0x8000 && addr < 0x8800) m_.video.videoram[addr - 0x8000] = uint8_t(v);
        else if (addr >= 0x8800 && addr < 0x9000) m_.ram1[(addr - 0x8800) & 0x3FF] = uint8_t(v);
        else if (addr >= 0x9000 && addr < 0x9800) m_.ram2[(addr - 0x9000) & 0x3FF] = uint8_t(v);
        else if (addr >= 0x9800 && addr < 0xA000) m_.ram3[(addr - 0x9800) & 0x3FF] = uint8_t(v);
    }
    int memRead(int addr) { return m_.mem_read(uint16_t(addr & 0xFFFF)); }

private:
    galaga::Machine m_;
};

EMSCRIPTEN_BINDINGS(galaga) {
    emscripten::class_<Machine>("Machine")
        .constructor<>()
        .function("reset", &Machine::reset)
        .function("loadHwtest", &Machine::loadHwtest)
        .function("loadRomSet", &Machine::loadRomSet)
        .function("runCycles", &Machine::runCycles)
        .function("frameBuffer", &Machine::frameBuffer)
        .function("setIn0", &Machine::setIn0)
        .function("setIn1", &Machine::setIn1)
        .function("setDswA", &Machine::setDswA)
        .function("setDswB", &Machine::setDswB)
        .function("setAudioHz", &Machine::setAudioHz)
        .function("in0", &Machine::in0)
        .function("in1", &Machine::in1)
        .function("dswA", &Machine::dswA)
        .function("dswB", &Machine::dswB)
        .function("mcu51Hle", &Machine::mcu51Hle)
        .function("mcu54Hle", &Machine::mcu54Hle)
        .function("drainAudio", &Machine::drainAudio)
        .function("state", &Machine::state)
        .function("totalCycles", &Machine::totalCycles)
        .function("screenWidth", &Machine::screenWidth)
        .function("screenHeight", &Machine::screenHeight)
        .function("ramByte", &Machine::ramByte)
        .function("setRamByte", &Machine::setRamByte)
        .function("memRead", &Machine::memRead);
}
