// Emscripten wrapper: binds the 80286 core + AT chipset into one Machine for the browser.
// JS surface via embind:
//   const m = new Module.Machine();
//   m.loadRom(0xF0000, biosBytes);       // BIOS-bochs-legacy at the reset vector
//   m.loadRom(0xC0000, vgaBiosBytes);    // VGABIOS-lgpl-latest.bin extension ROM
//   m.mountHdd(hddBytes);                // C: image, chosen before power-on
//   m.hddDirty() / m.clearHddDirty() / m.hddImage()  // persist C: across power cycles
//   m.mountFloppy(0, imgBytes);          // drive A:
//   m.runCycles(66667);                  // one frame at 8 MHz
//   const frame = m.renderFrame(blinkOn); // RGBA; then renderWidth()/renderHeight()
//   m.injectScancode(0x1E);               // Set 1 scan code (see i8042.h)
//   const edges = m.speakerEdges();       // {cycles: Float64Array, levels: Uint8Array}
//   m.textScreen();                       // test-only: text-mode screen as a string

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstdint>
#include <string>
#include <vector>

#include "../chipset.h"
#include "../ega_render.h"
#include "../machine.h"

using emscripten::val;
using ibmpcat::Machine;

class WasmMachine {
public:
    WasmMachine() { m_.reset(); }

    // reset keeps CMOS, like the real reset button
    void reset() { m_.reset(); }

    // BIOS-bochs-legacy at 0x100000-size (reset vector F000:FFF0 needs a 64KB image) or VGABIOS at 0xC0000
    void loadRom(double addr, val bytes) {
        std::vector<uint8_t> data = emscripten::convertJSArrayToNumberVector<uint8_t>(bytes);
        m_.chipset.load_rom(uint32_t(addr), data.data(), data.size());
    }

    // Runs in sub-chunks so activity LEDs latch; one frame can span a whole HDD BSY window.
    void runCycles(double cycles) {
        int64_t remaining = int64_t(cycles);
        constexpr int64_t kSubChunk = 2000;
        while (remaining > 0) {
            int64_t step = remaining < kSubChunk ? remaining : kSubChunk;
            m_.run_cycles(step);
            remaining -= step;
            if (m_.chipset.hdd.busy()) hdd_activity_latch_ = true;
            if (m_.chipset.fdc.drives[0].motor_on) floppy_activity_latch_[0] = true;
            if (m_.chipset.fdc.drives[1].motor_on) floppy_activity_latch_[1] = true;
        }
    }

    double totalCycles() const { return double(m_.total_cycles()); }
    bool halted() const { return m_.cpu.halted; }

    // ---- EGA screen ----
    // blinkOn is the text cursor phase. Call renderFrame() before renderWidth()/renderHeight().
    val renderFrame(bool blinkOn) {
        ibmpcat::RenderScreen(m_.chipset.ega, last_frame_, blinkOn);
        const auto &rgba = last_frame_.rgba;
        val out = val::global("Uint8ClampedArray").new_(rgba.size());
        if (!rgba.empty())
            out.call<void>("set", val(emscripten::typed_memory_view(rgba.size(), rgba.data())));
        return out;
    }
    int renderWidth() const { return last_frame_.width; }
    int renderHeight() const { return last_frame_.height; }

    // ---- keyboard ----
    // raw Set 1 byte; the front end owns the key table
    void injectScancode(int code) { m_.chipset.kbc.inject_scancode(uint8_t(code)); }

    // ---- floppy drives (fdc765) ----
    void mountFloppy(int drive, val bytes) {
        std::vector<uint8_t> data = emscripten::convertJSArrayToNumberVector<uint8_t>(bytes);
        m_.chipset.fdc.mount(drive, data.data(), data.size());
    }
    void unmountFloppy(int drive) { m_.chipset.fdc.unmount(drive); }
    bool floppyPresent(int drive) const { return m_.chipset.fdc.drives[drive & 1].present; }
    bool floppyDirty(int drive) const { return m_.chipset.fdc.drives[drive & 1].dirty; }
    void clearFloppyDirty(int drive) { m_.chipset.fdc.drives[drive & 1].dirty = false; }
    // motor was on at any point since the last call
    bool floppyMotorOn(int drive) {
        bool v = floppy_activity_latch_[drive & 1];
        floppy_activity_latch_[drive & 1] = false;
        return v;
    }
    // current image, possibly written to
    val floppyImage(int drive) {
        const std::vector<uint8_t> &img = m_.chipset.fdc.drives[drive & 1].image;
        val out = val::global("Uint8Array").new_(img.size());
        if (!img.empty())
            out.call<void>("set", val(emscripten::typed_memory_view(img.size(), img.data())));
        return out;
    }

    // ---- hard disk (wd1003) ----
    void mountHdd(val bytes) {
        std::vector<uint8_t> data = emscripten::convertJSArrayToNumberVector<uint8_t>(bytes);
        m_.chipset.hdd.mount(0, data.data(), data.size());
    }
    // busy at any point since the last call
    bool hddBusy() {
        bool v = hdd_activity_latch_;
        hdd_activity_latch_ = false;
        return v;
    }
    // written since last mount, like floppyDirty()
    bool hddDirty() { return m_.chipset.hdd.dirty(0); }
    void clearHddDirty() { m_.chipset.hdd.clear_dirty(0); }
    // current image, possibly written to
    val hddImage() {
        const std::vector<uint8_t> &img = m_.chipset.hdd.image(0);
        val out = val::global("Uint8Array").new_(img.size());
        if (!img.empty())
            out.call<void>("set", val(emscripten::typed_memory_view(img.size(), img.data())));
        return out;
    }

    // ---- PC speaker ----
    // drains the (cpu_cycle, level) edges since the last call; cycles/8e6 is seconds
    // current level, for frames with no edges
    bool speakerLevel() const { return m_.chipset.speaker.level(); }

    // ---- test-only: text-mode screen as a string ----
    // Mirrors ega_render.cpp RenderTextScreen addressing: plane 0 characters, one row per line, "" outside text mode.
    std::string textScreen() const {
        const auto &ega = m_.chipset.ega;
        if (ibmpcat::DetectScreenMode(ega) != ibmpcat::ScreenMode::kText) return "";
        constexpr int kColsFallback = 80, kRows = 25;  // rows fixed, matching RenderTextScreen
        int cols_reg = int(ega.crtc_horizontal_display_end()) + 1;
        int cols = cols_reg <= 1 ? kColsFallback : cols_reg;
        std::string out;
        out.reserve(std::size_t(cols + 1) * kRows);
        for (int row = 0; row < kRows; ++row) {
            for (int col = 0; col < cols; ++col) {
                uint32_t plane_off = (uint32_t(ega.start_offset()) + uint32_t(row * cols + col)) & 0xFFFF;
                out += char(ega.vram[(plane_off << 2) + 0]);
            }
            out += '\n';
        }
        return out;
    }

    val speakerEdges() {
        std::vector<ibmpcat::PcSpeaker::Edge> edges = m_.chipset.speaker.drain_edges();
        std::vector<double> cycles(edges.size());
        std::vector<uint8_t> levels(edges.size());
        for (std::size_t i = 0; i < edges.size(); ++i) {
            cycles[i] = double(edges[i].cpu_cycle);
            levels[i] = edges[i].level ? 1 : 0;
        }
        val c = val::global("Float64Array").new_(cycles.size());
        if (!cycles.empty())
            c.call<void>("set", val(emscripten::typed_memory_view(cycles.size(), cycles.data())));
        val l = val::global("Uint8Array").new_(levels.size());
        if (!levels.empty())
            l.call<void>("set", val(emscripten::typed_memory_view(levels.size(), levels.data())));
        val out = val::object();
        out.set("cycles", c);
        out.set("levels", l);
        return out;
    }

private:
    Machine m_;
    ibmpcat::RenderedFrame last_frame_;
    bool hdd_activity_latch_ = false;
    bool floppy_activity_latch_[2] = {false, false};
};

EMSCRIPTEN_BINDINGS(ibmpcat_machine) {
    emscripten::class_<WasmMachine>("Machine")
        .constructor<>()
        .function("reset", &WasmMachine::reset)
        .function("loadRom", &WasmMachine::loadRom)
        .function("runCycles", &WasmMachine::runCycles)
        .function("totalCycles", &WasmMachine::totalCycles)
        .function("halted", &WasmMachine::halted)
        .function("renderFrame", &WasmMachine::renderFrame)
        .function("renderWidth", &WasmMachine::renderWidth)
        .function("renderHeight", &WasmMachine::renderHeight)
        .function("injectScancode", &WasmMachine::injectScancode)
        .function("mountFloppy", &WasmMachine::mountFloppy)
        .function("unmountFloppy", &WasmMachine::unmountFloppy)
        .function("floppyPresent", &WasmMachine::floppyPresent)
        .function("floppyDirty", &WasmMachine::floppyDirty)
        .function("clearFloppyDirty", &WasmMachine::clearFloppyDirty)
        .function("floppyMotorOn", &WasmMachine::floppyMotorOn)
        .function("floppyImage", &WasmMachine::floppyImage)
        .function("mountHdd", &WasmMachine::mountHdd)
        .function("hddBusy", &WasmMachine::hddBusy)
        .function("hddDirty", &WasmMachine::hddDirty)
        .function("clearHddDirty", &WasmMachine::clearHddDirty)
        .function("hddImage", &WasmMachine::hddImage)
        .function("speakerLevel", &WasmMachine::speakerLevel)
        .function("speakerEdges", &WasmMachine::speakerEdges)
        .function("textScreen", &WasmMachine::textScreen);
}
