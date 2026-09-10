// Emscripten wrapper: binds the 80286 core + AT chipset into one `Machine`
// object the browser can drive. Build with `make` in this directory (needs
// the emsdk toolchain on PATH).
//
// JS surface (all via embind):
//   const m = new Module.Machine();
//   m.loadRom(0xF0000, biosBytes);       // BIOS-bochs-legacy at the reset vector
//   m.loadRom(0xC0000, vgaBiosBytes);    // VGABIOS-lgpl-latest.bin extension ROM
//   m.mountHdd(hddBytes);                // whatever the front end decides C: should start as this
//                                         // power-on -- factory FreeDOS, a blank drive, or a
//                                         // previously-saved image; no swap UI while running
//   m.hddDirty() / m.clearHddDirty() / m.hddImage()  // for persisting C:'s writes across power cycles
//   m.mountFloppy(0, imgBytes);          // drive A:
//   m.runCycles(66667);                  // advance one frame at real 8 MHz
//   const frame = m.renderFrame(blinkOn); // Uint8ClampedArray RGBA -- call
//                                          // renderWidth()/renderHeight() after (resolution varies by mode)
//   m.injectScancode(0x1E);               // real Set 1 scan code (see i8042.h)
//   const edges = m.speakerEdges();       // {cycles: Float64Array, levels: Uint8Array}
//   m.textScreen();                       // test-only: current text-mode screen as a string, "" in graphics modes

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
    // elapsed (real 8 MHz -- never sped up, per CLAUDE.md). Sub-chunked
    // (not one bulk m_.run_cycles() call) so activity-LED state can be
    // latched along the way -- a whole frame's worth of cycles easily
    // spans an HDD transfer's real BSY window start to finish, so
    // sampling busy()/motor_on only once at the very end, after the fact,
    // would just as easily miss it as catch it. See hddBusy()/
    // floppyMotorOn()'s own comments for how the latch is consumed.
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

    // ---- EGA screen -----------------------------------------------------
    // Renders whatever screen is currently active (text, or the CGA-
    // compatible 4-color graphics mode) to a packed RGBA8888 buffer -- see
    // ega_render.h for what it reads (real character-generator RAM, real
    // palette registers, real mode-detect registers) and its scope
    // (IBM_PCAT_REVIEW.md §14/§15). `blinkOn` selects the text cursor's
    // current blink phase (ignored in graphics modes); the caller paces
    // the real ~500ms rate. renderWidth()/renderHeight() reflect whatever
    // the most recent renderFrame() call actually rendered -- call it
    // first each frame, since resolution can change between calls.
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
    // True if the motor was on at any point since the last call (see
    // runCycles()'s comment) -- pulse-stretched to "since last read", the
    // same convention altair8800/web/wasm_machine.cpp's busActivityCounts()/
    // int_seen_ use for their own once-a-frame-polled indicators.
    bool floppyMotorOn(int drive) {
        bool v = floppy_activity_latch_[drive & 1];
        floppy_activity_latch_[drive & 1] = false;
        return v;
    }
    // The current (possibly written-to) image, for "save disk to file".
    val floppyImage(int drive) {
        const std::vector<uint8_t> &img = m_.chipset.fdc.drives[drive & 1].image;
        val out = val::global("Uint8Array").new_(img.size());
        if (!img.empty())
            out.call<void>("set", val(emscripten::typed_memory_view(img.size(), img.data())));
        return out;
    }

    // ---- hard disk (wd1003) -- fixed media, no swap-while-running UI -----
    // What bytes this loads is the front end's call (factory FreeDOS, a
    // blank drive, or a saved image from IndexedDB) -- a real fixed disk
    // isn't swappable at all, but which disk shipped in the box, or was
    // fitted since, is exactly this kind of pre-power-on decision.
    void mountHdd(val bytes) {
        std::vector<uint8_t> data = emscripten::convertJSArrayToNumberVector<uint8_t>(bytes);
        m_.chipset.hdd.mount(0, data.data(), data.size());
    }
    // True if the drive was busy at any point since the last call -- see
    // runCycles()'s comment.
    bool hddBusy() {
        bool v = hdd_activity_latch_;
        hdd_activity_latch_ = false;
        return v;
    }
    // True if C: has been written to since it was last mount()ed -- lets
    // the front end persist changes (e.g. to IndexedDB) only when there's
    // actually something new to save, same convention as floppyDirty().
    bool hddDirty() { return m_.chipset.hdd.dirty(0); }
    void clearHddDirty() { m_.chipset.hdd.clear_dirty(0); }
    // The current (possibly written-to) image, so the front end can carry
    // C:'s contents forward across power cycles.
    val hddImage() {
        const std::vector<uint8_t> &img = m_.chipset.hdd.image(0);
        val out = val::global("Uint8Array").new_(img.size());
        if (!img.empty())
            out.call<void>("set", val(emscripten::typed_memory_view(img.size(), img.data())));
        return out;
    }

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

    // ---- test-only convenience: text-mode screen as a string --------------
    // Real hardware has no such capability -- this stands in for a person's
    // own eyes on the CRT, so a test can assert on boot banners/prompts the
    // way altair8800/assembler6502's Playwright suites assert on their
    // serial-terminal buffer text. Mirrors ega_render.cpp's RenderTextScreen
    // character addressing exactly (see that function's own comments for why
    // the column count and start offset come from the live CRTC registers
    // rather than a hardcoded 80): the character byte at plane 0 of each
    // cell, one row per line, attribute byte ignored. Returns "" outside
    // text mode -- renderFrame()/renderWidth()/renderHeight() are the real,
    // mode-agnostic way to see the screen; this is a test convenience only.
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
