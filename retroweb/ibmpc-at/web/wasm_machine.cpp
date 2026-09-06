// Emscripten wrapper: binds the 80286 core + AT chipset into one `Machine`
// object the browser can drive. Build with `make` in this directory (needs
// the emsdk toolchain on PATH).
//
// JS surface (all via embind):
//   const m = new Module.Machine();
//   m.loadRom(0xF0000, biosBytes);       // BIOS-bochs-legacy at the reset vector
//   m.loadRom(0xC0000, vgaBiosBytes);    // VGABIOS-lgpl-latest.bin extension ROM
//   m.mountHdd(hddBytes);                // ships pre-loaded -- no swap UI
//   m.mountFloppy(0, imgBytes);          // drive A:
//   m.runCycles(66667);                  // advance one frame at real 8 MHz
//   const frame = m.renderFrame(blinkOn); // Uint8ClampedArray, 640x350 RGBA
//   m.injectScancode(0x1E);               // real Set 1 scan code (see i8042.h)
//   const edges = m.speakerEdges();       // {cycles: Float64Array, levels: Uint8Array}

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstdint>
#include <vector>

#include "../chipset.h"
#include "../ega_render.h"
#include "../machine.h"

using emscripten::val;
using ibmpcat::Machine;

class WasmMachine {
public:
    WasmMachine() { m_.reset(); }

    // The front-panel RESET-equivalent (no separate power switch on a real
    // AT beyond the mains switch itself -- reloading the page models that).
    // Preserves CMOS, exactly like the real reset button.
    void reset() { m_.reset(); }

    // Drop a ROM image at a physical address -- BIOS-bochs-legacy at
    // 0x100000-bios.size() (the real reset vector, F000:FFF0, expects a
    // 64KB image there) or VGABIOS-lgpl-latest.bin at 0xC0000 (the
    // standard video-BIOS extension ROM window).
    void loadRom(double addr, val bytes) {
        std::vector<uint8_t> data = emscripten::convertJSArrayToNumberVector<uint8_t>(bytes);
        m_.chipset.load_rom(uint32_t(addr), data.data(), data.size());
    }

    // Run instructions until at least `cycles` more CPU cycles have
    // elapsed (real 8 MHz -- never sped up, per CLAUDE.md).
    void runCycles(double cycles) { m_.run_cycles(int64_t(cycles)); }

    double totalCycles() const { return double(m_.total_cycles()); }
    bool halted() const { return m_.cpu.halted; }

    // ---- EGA text-mode screen -----------------------------------------
    // Renders the current screen to a packed RGBA8888 buffer -- see
    // ega_render.h for what it reads (real character-generator RAM, real
    // palette registers) and its scope (text mode only; see
    // IBM_PCAT_REVIEW.md §14). `blinkOn` selects the cursor's current
    // blink phase; the caller paces the real ~500ms rate.
    val renderFrame(bool blinkOn) {
        std::vector<uint8_t> rgba;
        ibmpcat::RenderTextScreen(m_.chipset.ega, rgba, blinkOn);
        val out = val::global("Uint8ClampedArray").new_(rgba.size());
        if (!rgba.empty())
            out.call<void>("set", val(emscripten::typed_memory_view(rgba.size(), rgba.data())));
        return out;
    }
    int renderWidth() const { return ibmpcat::kTextRenderWidth; }
    int renderHeight() const { return ibmpcat::kTextRenderHeight; }

    // ---- keyboard -------------------------------------------------------
    // Deliver one real Set 1 scan code (make or break) -- see i8042.h.
    // The front end owns the physical-key -> Set-1 table; this device
    // does no translation of its own.
    void injectScancode(int code) { m_.chipset.kbc.inject_scancode(uint8_t(code)); }

    // ---- floppy drives (fdc765) -----------------------------------------
    void mountFloppy(int drive, val bytes) {
        std::vector<uint8_t> data = emscripten::convertJSArrayToNumberVector<uint8_t>(bytes);
        m_.chipset.fdc.mount(drive, data.data(), data.size());
    }
    void unmountFloppy(int drive) { m_.chipset.fdc.unmount(drive); }
    bool floppyPresent(int drive) const { return m_.chipset.fdc.drives[drive & 1].present; }
    bool floppyDirty(int drive) const { return m_.chipset.fdc.drives[drive & 1].dirty; }
    void clearFloppyDirty(int drive) { m_.chipset.fdc.drives[drive & 1].dirty = false; }
    bool floppyMotorOn(int drive) const { return m_.chipset.fdc.drives[drive & 1].motor_on; }
    // The current (possibly written-to) image, for "save disk to file".
    val floppyImage(int drive) {
        const std::vector<uint8_t> &img = m_.chipset.fdc.drives[drive & 1].image;
        val out = val::global("Uint8Array").new_(img.size());
        if (!img.empty())
            out.call<void>("set", val(emscripten::typed_memory_view(img.size(), img.data())));
        return out;
    }

    // ---- hard disk (wd1003) -- ships pre-loaded, no swap UI --------------
    void mountHdd(val bytes) {
        std::vector<uint8_t> data = emscripten::convertJSArrayToNumberVector<uint8_t>(bytes);
        m_.chipset.hdd.mount(0, data.data(), data.size());
    }
    bool hddBusy() const { return m_.chipset.hdd.busy(); }

    // ---- PC speaker -------------------------------------------------------
    // Drains the real-time (cpu_cycle, level) edge trace since the last
    // call -- see pcspeaker.h. Two parallel typed arrays rather than an
    // array of objects: cheap to marshal, and cycles/8e6 (this machine's
    // fixed real clock) converts directly to seconds for a Web Audio
    // renderer to resample against.
    // The speaker's current level, independent of the edge trace -- a
    // frame with zero edges still needs to know what level was already
    // holding, so its audio buffer isn't wrongly filled with silence
    // instead of a steady tone/held sample level.
    bool speakerLevel() const { return m_.chipset.speaker.level(); }

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
        .function("speakerLevel", &WasmMachine::speakerLevel)
        .function("speakerEdges", &WasmMachine::speakerEdges);
}
