// Emscripten wrapper: binds the 80486 core + this machine's chipset into
// one `Machine` object the browser can drive. Build with `make` in this
// directory (needs the emsdk toolchain on PATH). Mirrors
// ibmpc-at/web/wasm_machine.cpp's structure and JS surface, adapted for a
// single floppy bay and the new secondary-channel ATAPI CD-ROM.
//
// JS surface (all via embind):
//   const m = new Module.Machine();
//   m.loadRom(0xF0000, biosBytes);       // BIOS-bochs-legacy at the reset vector
//   m.loadRom(0xC0000, vgaBiosBytes);    // VGABIOS-lgpl-latest.bin extension ROM
//   m.mountHdd(hddBytes);                // whatever the front end decides C: should start as this
//                                         // power-on -- factory FreeDOS, a blank drive, or a
//                                         // previously-saved image; no swap UI while running
//   m.hddDirty() / m.clearHddDirty() / m.hddImage()  // for persisting C:'s writes across power cycles
//   m.hddDirtyPatches()                  // [{offset, bytes}, ...] -- only what actually changed,
//                                         // for periodic persistence without re-copying all 504MB
//   m.mountFloppy(imgBytes);             // this machine's one 3.5" bay (A:)
//   m.mountCdrom(isoBytes) / m.ejectCdrom()  // swappable, like the floppy
//   m.runCycles(66000000/60);            // advance one frame at real 66 MHz
//   const frame = m.renderFrame(blinkOn); // Uint8ClampedArray RGBA -- call
//                                          // renderWidth()/renderHeight() after (resolution varies by mode)
//   m.injectScancode(0x1E);               // real Set 1 scan code (see i8042.h)
//   m.injectMouseEvent(dx, dy, buttons);   // PS/2 AUX port -- dy is +away-from-user, negate a browser movementY first
//   const edges = m.speakerEdges();       // {cycles: Float64Array, levels: Uint8Array}
//   const audio = m.sbDrainSamples();     // {cycles: Float64Array, left: Int16Array, right: Int16Array}
//   m.sbSampleRateHz();                   // the DSP's currently-programmed output rate
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
using pc486::Machine;

class WasmMachine {
public:
    WasmMachine() { m_.reset(); }

    // The front-panel Reset button: pulses CPU+chipset reset, preserving
    // CMOS -- exactly what a real reset button's RESET line does. Power
    // on/off (the front panel's separate power switch) is a front-end/JS
    // concern -- whether runCycles() gets called at all -- not modeled here,
    // same as ibmpc-at's own reset()/comment.
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
    // elapsed (real 66 MHz -- never sped up, per CLAUDE.md). Sub-chunked so
    // activity-LED state can be latched along the way -- see
    // ibmpc-at/web/wasm_machine.cpp's identical comment for why sampling
    // busy()/motor_on only once at the end of a whole frame would miss
    // activity that started and finished mid-frame.
    void runCycles(double cycles) {
        int64_t remaining = int64_t(cycles);
        constexpr int64_t kSubChunk = 2000;
        while (remaining > 0) {
            int64_t step = remaining < kSubChunk ? remaining : kSubChunk;
            m_.run_cycles(step);
            remaining -= step;
            if (m_.chipset.hdd.busy()) hdd_activity_latch_ = true;
            if (m_.chipset.cdrom.busy()) cdrom_activity_latch_ = true;
            if (m_.chipset.fdc.drives[0].motor_on) floppy_activity_latch_ = true;
        }
    }

    double totalCycles() const { return double(m_.total_cycles()); }
    bool halted() const { return m_.cpu.halted; }

    // ---- video (vga, running at its Milestone 1 real-EGA-ceiling modes) --
    val renderFrame(bool blinkOn) {
        pc486::RenderScreen(m_.chipset.vga, last_frame_, blinkOn);
        const auto &rgba = last_frame_.rgba;
        val out = val::global("Uint8ClampedArray").new_(rgba.size());
        if (!rgba.empty())
            out.call<void>("set", val(emscripten::typed_memory_view(rgba.size(), rgba.data())));
        return out;
    }
    int renderWidth() const { return last_frame_.width; }
    int renderHeight() const { return last_frame_.height; }

    // ---- keyboard -------------------------------------------------------
    void injectScancode(int code) { m_.chipset.kbc.inject_scancode(uint8_t(code)); }

    // ---- PS/2 mouse (8042 AUX port) --------------------------------------
    // `dy` follows the mouse's own axis convention (+Y away from the user)
    // -- see chipset.h's inject_mouse_event comment. A caller passing a
    // browser `movementY` must negate it first.
    void injectMouseEvent(int dx, int dy, int buttons) {
        m_.chipset.inject_mouse_event(dx, dy, uint8_t(buttons));
    }

    // ---- floppy drive (fdc765) -- this machine's single 3.5" bay --------
    void mountFloppy(val bytes) {
        std::vector<uint8_t> data = emscripten::convertJSArrayToNumberVector<uint8_t>(bytes);
        m_.chipset.fdc.mount(0, data.data(), data.size());
    }
    void unmountFloppy() { m_.chipset.fdc.unmount(0); }
    bool floppyPresent() const { return m_.chipset.fdc.drives[0].present; }
    bool floppyDirty() const { return m_.chipset.fdc.drives[0].dirty; }
    void clearFloppyDirty() { m_.chipset.fdc.drives[0].dirty = false; }
    bool floppyMotorOn() {
        bool v = floppy_activity_latch_;
        floppy_activity_latch_ = false;
        return v;
    }
    val floppyImage() {
        const std::vector<uint8_t> &img = m_.chipset.fdc.drives[0].image;
        val out = val::global("Uint8Array").new_(img.size());
        if (!img.empty())
            out.call<void>("set", val(emscripten::typed_memory_view(img.size(), img.data())));
        return out;
    }

    // ---- hard disk (wd1003) -- fixed media, no swap-while-running UI -----
    void mountHdd(val bytes) {
        std::vector<uint8_t> data = emscripten::convertJSArrayToNumberVector<uint8_t>(bytes);
        m_.chipset.hdd.mount(0, data.data(), data.size());
    }
    bool hddBusy() {
        bool v = hdd_activity_latch_;
        hdd_activity_latch_ = false;
        return v;
    }
    bool hddDirty() { return m_.chipset.hdd.dirty(0); }
    void clearHddDirty() { m_.chipset.hdd.clear_dirty(0); }
    val hddImage() {
        const std::vector<uint8_t> &img = m_.chipset.hdd.image(0);
        val out = val::global("Uint8Array").new_(img.size());
        if (!img.empty())
            out.call<void>("set", val(emscripten::typed_memory_view(img.size(), img.data())));
        return out;
    }
    // Only the byte ranges dirty_ranges() says actually changed, each as its
    // own small Uint8Array -- see wd1003.h's dirty_ranges() comment. The
    // front end patches these into its own kept copy of C: instead of
    // pulling the whole 504MB image on every periodic save.
    val hddDirtyPatches() {
        auto ranges = m_.chipset.hdd.dirty_ranges(0);
        const std::vector<uint8_t> &img = m_.chipset.hdd.image(0);
        val out = val::array();
        for (const auto &r : ranges) {
            val entry = val::object();
            entry.set("offset", r.offset);
            val bytes = val::global("Uint8Array").new_(r.length);
            bytes.call<void>("set", val(emscripten::typed_memory_view(r.length, img.data() + r.offset)));
            entry.set("bytes", bytes);
            out.call<val>("push", entry);
        }
        return out;
    }

    // ---- CD-ROM (atapi_cdrom) -- removable media, like the floppy -------
    void mountCdrom(val bytes) {
        std::vector<uint8_t> data = emscripten::convertJSArrayToNumberVector<uint8_t>(bytes);
        m_.chipset.cdrom.mount(data.data(), data.size());
    }
    void ejectCdrom() { m_.chipset.cdrom.eject(); }
    bool cdromPresent() const { return m_.chipset.cdrom.media_present(); }
    bool cdromBusy() {
        bool v = cdrom_activity_latch_;
        cdrom_activity_latch_ = false;
        return v;
    }

    // ---- PC speaker -------------------------------------------------------
    bool speakerLevel() const { return m_.chipset.speaker.level(); }

    // ---- test-only convenience: text-mode screen as a string --------------
    // See ibmpc-at/web/wasm_machine.cpp's identical function for the full
    // rationale -- unchanged here beyond the namespace.
    std::string textScreen() const {
        const auto &vga = m_.chipset.vga;
        if (pc486::DetectScreenMode(vga) != pc486::ScreenMode::kText) return "";
        constexpr int kColsFallback = 80, kRows = 25;
        int cols_reg = int(vga.crtc_horizontal_display_end()) + 1;
        int cols = cols_reg <= 1 ? kColsFallback : cols_reg;
        std::string out;
        out.reserve(std::size_t(cols + 1) * kRows);
        for (int row = 0; row < kRows; ++row) {
            for (int col = 0; col < cols; ++col) {
                uint32_t plane_off = (uint32_t(vga.start_offset()) + uint32_t(row * cols + col)) & 0xFFFF;
                out += char(vga.vram[(plane_off << 2) + 0]);
            }
            out += '\n';
        }
        return out;
    }

    val speakerEdges() {
        std::vector<pc486::PcSpeaker::Edge> edges = m_.chipset.speaker.drain_edges();
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

    // ---- Sound Blaster 16 -------------------------------------------------
    // Drains the samples the DSP produced since the last call, paced at the
    // real programmed sample rate (soundblaster.h's Sample log) -- same
    // drain-and-clear shape as speakerEdges() above.
    val sbDrainSamples() {
        std::vector<pc486::SoundBlaster::Sample> samples = m_.chipset.sb.drain_samples();
        std::vector<double> cycles(samples.size());
        std::vector<int16_t> left(samples.size());
        std::vector<int16_t> right(samples.size());
        for (std::size_t i = 0; i < samples.size(); ++i) {
            cycles[i] = double(samples[i].cpu_cycle);
            left[i] = samples[i].left;
            right[i] = samples[i].right;
        }
        val c = val::global("Float64Array").new_(cycles.size());
        if (!cycles.empty())
            c.call<void>("set", val(emscripten::typed_memory_view(cycles.size(), cycles.data())));
        val l = val::global("Int16Array").new_(left.size());
        if (!left.empty())
            l.call<void>("set", val(emscripten::typed_memory_view(left.size(), left.data())));
        val r = val::global("Int16Array").new_(right.size());
        if (!right.empty())
            r.call<void>("set", val(emscripten::typed_memory_view(right.size(), right.data())));
        val out = val::object();
        out.set("cycles", c);
        out.set("left", l);
        out.set("right", r);
        return out;
    }
    uint32_t sbSampleRateHz() const { return m_.chipset.sb.sample_rate_hz(); }

private:
    Machine m_;
    pc486::RenderedFrame last_frame_;
    bool hdd_activity_latch_ = false;
    bool cdrom_activity_latch_ = false;
    bool floppy_activity_latch_ = false;
};

EMSCRIPTEN_BINDINGS(pc486_machine) {
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
        .function("injectMouseEvent", &WasmMachine::injectMouseEvent)
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
        .function("hddDirtyPatches", &WasmMachine::hddDirtyPatches)
        .function("mountCdrom", &WasmMachine::mountCdrom)
        .function("ejectCdrom", &WasmMachine::ejectCdrom)
        .function("cdromPresent", &WasmMachine::cdromPresent)
        .function("cdromBusy", &WasmMachine::cdromBusy)
        .function("speakerLevel", &WasmMachine::speakerLevel)
        .function("speakerEdges", &WasmMachine::speakerEdges)
        .function("sbDrainSamples", &WasmMachine::sbDrainSamples)
        .function("sbSampleRateHz", &WasmMachine::sbSampleRateHz)
        .function("textScreen", &WasmMachine::textScreen);
}
