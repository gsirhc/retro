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
//   m.mountCdromCue(cueText, binBytes)   // mixed-mode disc: data + CD-DA audio tracks
//   const cd = m.cdromDrainSamples();    // the drive's own audio output, same shape as sbDrainSamples()
//   m.cdromSampleRateHz(); m.cdGainLeft(); m.cdGainRight();  // CD-DA's fixed rate and SB16 mixer gain
//   m.runCycles(66000000/60);            // advance one frame at real 66 MHz
//   const frame = m.renderFrame(blinkOn); // Uint8ClampedArray RGBA -- call
//                                          // renderWidth()/renderHeight() after (resolution varies by mode)
//   m.injectScancode(0x1E);               // real Set 1 scan code (see i8042.h)
//   m.injectMouseEvent(dx, dy, buttons);   // PS/2 AUX port -- dy is +away-from-user, negate a browser movementY first
//   const edges = m.speakerEdges();       // {cycles: Float64Array, levels: Uint8Array}
//   const audio = m.sbDrainSamples();     // {cycles: Float64Array, left: Int16Array, right: Int16Array}
//   m.sbSampleRateHz();                   // the DSP's currently-programmed output rate
//   const fm = m.fmDrainSamples();        // the OPL3's stream, same shape as above
//   m.fmStartTrace(400000); m.fmDrainTrace();  // opt-in register-write trace, see app.js's window.__fm
//   m.fmGainLeft(); m.sbGainLeft();       // the CT1745 attenuators the front end mixes with
//   m.sbMixerRegister(0x44);              // raw CT1745 mixer register (treble/bass: 44h-47h)
//   m.textScreen();                       // test-only: current text-mode screen as a string, "" in graphics modes

#include <emscripten/bind.h>
#include <emscripten/heap.h>
#include <emscripten/val.h>

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "../chipset.h"
#include "../opl3.h"
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

    // Front-panel Turbo: on = 66 MHz (DX2 clock-doubled), off = 33 MHz
    // (bus rate). The PIT crystal and device wall-clock pacing stay correct
    // -- only the CPU's internal clock drops. See Machine::set_turbo.
    void setTurbo(bool on) { m_.set_turbo(on); }
    bool turbo() const { return m_.turbo(); }
    double cpuHz() const { return m_.cpu_hz(); }

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
    // Mixed-mode disc (data + CD-DA audio tracks): a CUE sheet naming the
    // one BIN file `binBytes` supplies. Returns false (mounting nothing) if
    // the sheet doesn't parse -- see atapi_cdrom.h's mount_cue().
    bool mountCdromCue(std::string cueText, val binBytes) {
        std::vector<uint8_t> data = emscripten::convertJSArrayToNumberVector<uint8_t>(binBytes);
        return m_.chipset.cdrom.mount_cue(cueText.c_str(), data.data(), data.size());
    }
    void ejectCdrom() { m_.chipset.cdrom.eject(); }
    bool cdromPresent() const { return m_.chipset.cdrom.media_present(); }
    bool cdromBusy() {
        bool v = cdrom_activity_latch_;
        cdrom_activity_latch_ = false;
        return v;
    }
    bool cdromPlayingAudio() const { return m_.chipset.cdrom.playing_audio(); }

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
    // The OPL3's own stream. Separate from sbDrainSamples() because the card
    // sums FM and digitized audio in the analog domain, each behind its own
    // CT1745 attenuator -- the front end applies those and mixes.
    val fmDrainSamples() {
        std::vector<pc486::Opl3::Sample> samples = m_.chipset.sb.fm.drain_samples();
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

    // The CD-ROM's own analog audio leg, same {cycles,left,right} shape as
    // fmDrainSamples() above -- on real hardware this never crosses the ATA
    // bus either, it reaches the sound card over a physical cable. The
    // front end mixes it in alongside FM/digitized audio, gated by
    // cdGainLeft()/cdGainRight() below.
    val cdromDrainSamples() {
        std::vector<pc486::AtapiCdrom::Sample> samples = m_.chipset.cdrom.drain_samples();
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
    uint32_t cdromSampleRateHz() const { return pc486::AtapiCdrom::kAudioSampleRateHz; }

    // Test-only: the machine's real I/O decode, so a test can drive a device
    // through its actual ports rather than reaching past the port block.
    uint8_t portIn(uint16_t port) { return m_.chipset.io_in(port); }
    void portOut(uint16_t port, uint8_t v) { m_.chipset.io_out(port, v); }
    uint8_t fmReg(uint16_t index) const { return m_.chipset.sb.fm.reg(index); }
    bool fmOpl3Mode() const { return m_.chipset.sb.fm.opl3_mode(); }

    // ---- OPL3 register write trace (opt-in diagnostic, see app.js's
    // window.__fm) -- captures every FM register write with its CPU cycle
    // stamp so a real DOS game's music can be analysed offline.
    void fmStartTrace(int maxEvents) { m_.chipset.sb.fm.start_trace(std::size_t(maxEvents)); }
    val fmDrainTrace() {
        std::vector<pc486::Opl3::TraceEvent> events = m_.chipset.sb.fm.drain_trace();
        val out = val::array();
        for (const auto &e : events) {
            val entry = val::object();
            entry.set("cycle", double(e.cycle));
            entry.set("reg", e.reg);
            entry.set("value", e.value);
            out.call<val>("push", entry);
        }
        return out;
    }

    // Cycles the 486 has spent halted. The front end differences this
    // against totalCycles() to show the guest's own CPU usage -- the share
    // of its time the machine is doing work rather than waiting on an
    // interrupt. Always available; see Cpu::halt_cycles.
    double haltCycles() const { return double(m_.cpu.halt_cycles); }

    // Cycles the guest spent in a DOS wait loop rather than halted -- see
    // Cpu::idle_poll_cycles. Added to halted cycles, this is what makes a
    // usage figure mean anything on a machine whose OS has no idea what
    // idle is.
    double idleCycles() const { return double(m_.cpu.idle_poll_cycles); }

    // The emulator's real memory footprint: the wasm heap holds the 32MB of
    // guest RAM, video memory, the disk images in flight and everything else
    // the machine owns. Always available -- it costs one call and is the
    // figure a visitor is most likely to find interesting.
    double heapBytes() const { return double(emscripten_get_heap_size()); }

    // Always present, so the front end can ask whether this build carries
    // the emulator's own counters -- the Performance panel's Tier 2 -- and
    // show them only then. The shipped binary answers false.
    bool perfBuild() const {
#ifdef PC486_PERF
        return true;
#else
        return false;
#endif
    }
#ifdef PC486_PERF
    // Counters since the last call, as "name=value" pairs. Reading resets
    // them, so the front end gets a per-interval rate rather than a total.
    std::string perfStats() {
        auto &p = m_.cpu.perf;
        char buf[256];
        std::snprintf(buf, sizeof buf,
                      "instrs=%llu tlb_miss=%llu fetch_slow=%llu mmio=%llu services=%llu",
                      (unsigned long long)p.instrs, (unsigned long long)p.tlb_miss,
                      (unsigned long long)p.fetch_slow, (unsigned long long)p.mmio,
                      (unsigned long long)m_.chipset.perf_services);
        p.instrs = p.tlb_miss = p.fetch_slow = p.mmio = 0;
        m_.chipset.perf_services = 0;
        return buf;
    }

    // The busiest instruction forms since the last call, ranked. For an
    // interpreter, what it runs most is what is worth optimizing -- and a
    // count costs one increment, where timing each instruction would cost
    // more than the instruction. "0f" marks the two-byte map.
    std::string perfHotOpcodes(int top) {
        auto &p = m_.cpu.perf;
        struct Entry { uint64_t n; int op; bool two; };
        std::vector<Entry> all;
        all.reserve(512);
        for (int i = 0; i < 256; ++i) {
            if (p.opcode[i]) all.push_back({p.opcode[i], i, false});
            if (p.opcode0f[i]) all.push_back({p.opcode0f[i], i, true});
        }
        std::sort(all.begin(), all.end(),
                  [](const Entry &a, const Entry &b) { return a.n > b.n; });
        uint64_t total = 0;
        for (const Entry &e : all) total += e.n;
        std::string out;
        char line[64];
        int shown = 0;
        for (const Entry &e : all) {
            if (shown++ >= top) break;
            std::snprintf(line, sizeof line, "%s%02X=%llu ", e.two ? "0f" : "",
                          e.op, (unsigned long long)e.n);
            out += line;
        }
        std::snprintf(line, sizeof line, "total=%llu", (unsigned long long)total);
        out += line;
        for (int i = 0; i < 256; ++i) { p.opcode[i] = 0; p.opcode0f[i] = 0; }
        return out;
    }
#endif

    float fmGainLeft() const { return m_.chipset.sb.fm_gain_left(); }
    float fmGainRight() const { return m_.chipset.sb.fm_gain_right(); }
    float sbGainLeft() const { return m_.chipset.sb.output_gain_left(); }
    float sbGainRight() const { return m_.chipset.sb.output_gain_right(); }
    float cdGainLeft() const { return m_.chipset.sb.cd_gain_left(); }
    float cdGainRight() const { return m_.chipset.sb.cd_gain_right(); }
    // Raw CT1745 mixer register, for the tone controls (44h-47h) the front
    // end turns into shelving-filter gains -- see app.js's refreshSbTone().
    uint8_t sbMixerRegister(uint8_t index) const { return m_.chipset.sb.mixer_register(index); }

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
        .function("setTurbo", &WasmMachine::setTurbo)
        .function("turbo", &WasmMachine::turbo)
        .function("cpuHz", &WasmMachine::cpuHz)
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
        .function("mountCdromCue", &WasmMachine::mountCdromCue)
        .function("ejectCdrom", &WasmMachine::ejectCdrom)
        .function("cdromPresent", &WasmMachine::cdromPresent)
        .function("cdromBusy", &WasmMachine::cdromBusy)
        .function("cdromPlayingAudio", &WasmMachine::cdromPlayingAudio)
        .function("cdromDrainSamples", &WasmMachine::cdromDrainSamples)
        .function("cdromSampleRateHz", &WasmMachine::cdromSampleRateHz)
        .function("speakerLevel", &WasmMachine::speakerLevel)
        .function("speakerEdges", &WasmMachine::speakerEdges)
        .function("sbDrainSamples", &WasmMachine::sbDrainSamples)
        .function("sbSampleRateHz", &WasmMachine::sbSampleRateHz)
        .function("portIn", &WasmMachine::portIn)
        .function("portOut", &WasmMachine::portOut)
        .function("fmReg", &WasmMachine::fmReg)
        .function("fmOpl3Mode", &WasmMachine::fmOpl3Mode)
        .function("fmDrainSamples", &WasmMachine::fmDrainSamples)
        .function("fmStartTrace", &WasmMachine::fmStartTrace)
        .function("fmDrainTrace", &WasmMachine::fmDrainTrace)
        .function("heapBytes", &WasmMachine::heapBytes)
        .function("haltCycles", &WasmMachine::haltCycles)
        .function("idleCycles", &WasmMachine::idleCycles)
        .function("perfBuild", &WasmMachine::perfBuild)
#ifdef PC486_PERF
        .function("perfStats", &WasmMachine::perfStats)
        .function("perfHotOpcodes", &WasmMachine::perfHotOpcodes)
#endif
        .function("fmGainLeft", &WasmMachine::fmGainLeft)
        .function("fmGainRight", &WasmMachine::fmGainRight)
        .function("sbGainLeft", &WasmMachine::sbGainLeft)
        .function("sbGainRight", &WasmMachine::sbGainRight)
        .function("cdGainLeft", &WasmMachine::cdGainLeft)
        .function("cdGainRight", &WasmMachine::cdGainRight)
        .function("sbMixerRegister", &WasmMachine::sbMixerRegister)
        .function("textScreen", &WasmMachine::textScreen);
}
