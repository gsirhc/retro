// Emscripten wrapper: binds the 80486 core and chipset into one `Machine` object for the browser.
//
// JS surface (all via embind):
//   const m = new Module.Machine();
//   m.loadRom(0xF0000, biosBytes);       // BIOS-bochs-legacy at the reset vector
//   m.loadRom(0xC0000, vgaBiosBytes);    // VGABIOS-lgpl-latest.bin extension ROM
//   m.mountHdd(hddBytes);                // C: as of this power-on (factory, blank or saved); no swap while running
//   m.hddDirty() / m.clearHddDirty() / m.hddImage()  // persist C:'s writes
//   m.hddDirtyPatches()                  // [{offset, bytes}, ...] only what changed
//   m.mountFloppy(imgBytes);             // this machine's one 3.5" bay (A:)
//   m.mountCdrom(isoBytes) / m.ejectCdrom()  // swappable, like the floppy
//   m.mountCdromCue(cueText, binBytes)   // mixed-mode disc: data + CD-DA audio tracks
//   const cd = m.cdromDrainSamples();    // the drive's own audio output, same shape as sbDrainSamples()
//   m.cdromSampleRateHz(); m.cdGainLeft(); m.cdGainRight();  // CD-DA's fixed rate and SB16 mixer gain
//   m.runCycles(66000000/60);            // advance one frame at real 66 MHz
//   const frame = m.renderFrame();        // Uint8ClampedArray RGBA; size from renderWidth()/renderHeight()
//   m.injectScancode(0x1E);               // real Set 1 scan code (see i8042.h)
//   m.injectMouseEvent(dx, dy, buttons);   // PS/2 AUX port, dy is +away-from-user
//   const edges = m.speakerEdges();       // {cycles: Float64Array, levels: Uint8Array}
//   const audio = m.sbDrainSamples();     // {cycles: Float64Array, left: Int16Array, right: Int16Array}
//   m.sbSampleRateHz();                   // the DSP's currently-programmed output rate
//   const fm = m.fmDrainSamples();        // the OPL3's stream, same shape as above
//   m.fmStartTrace(400000); m.fmDrainTrace();  // opt-in register-write trace
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

    // Front-panel Reset: pulses CPU+chipset reset, CMOS preserved. Power is a front-end concern.
    void reset() { m_.reset(); }

    // Loads the RTC calendar as BIOS Setup would. weekday 1-7, 1 = Sunday.
    void setRtc(int year, int month, int day, int hour, int minute, int second, int weekday) {
        m_.chipset.cmos.set_time(year, month, day, hour, minute, second, weekday);
    }

    // Front-panel Turbo. Off holds the CPU off the bus part of the time; the clock stays 66 MHz.
    void setTurbo(bool on) { m_.set_turbo(on); }
    bool turbo() const { return m_.turbo(); }
    double cpuHz() const { return m_.cpu_hz(); }

    // Drops a ROM image at a physical address: BIOS at 0x100000-size (reset vector F000:FFF0 expects 64KB) or VGABIOS at 0xC0000.
    void loadRom(double addr, val bytes) {
        std::vector<uint8_t> data = emscripten::convertJSArrayToNumberVector<uint8_t>(bytes);
        m_.chipset.load_rom(uint32_t(addr), data.data(), data.size());
    }

    // Runs until at least `cycles` more CPU cycles have elapsed. Sub-chunked so activity LEDs latch
    // mid-frame.
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

    // ---- video ----
    val renderFrame() {
        pc486::RenderScreen(m_.chipset.vga, last_frame_);
        const auto &rgba = last_frame_.rgba;
        val out = val::global("Uint8ClampedArray").new_(rgba.size());
        if (!rgba.empty())
            out.call<void>("set", val(emscripten::typed_memory_view(rgba.size(), rgba.data())));
        return out;
    }
    int renderWidth() const { return last_frame_.width; }
    int renderHeight() const { return last_frame_.height; }

    // ---- keyboard ----
    void injectScancode(int code) { m_.chipset.kbc.inject_scancode(uint8_t(code)); }
    bool keyboardRepeating() const { return m_.chipset.kbc.typematic_active(); }

    // ---- PS/2 mouse (8042 AUX port) ----
    // `dy` is +Y away from the user (chipset.h inject_mouse_event); negate a browser movementY.
    void injectMouseEvent(int dx, int dy, int buttons) {
        m_.chipset.inject_mouse_event(dx, dy, uint8_t(buttons));
    }

    // ---- floppy (fdc765) ----
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

    // ---- hard disk (wd1003) ----
    void mountHdd(val bytes) {
        m_.chipset.hdd.mount(0, emscripten::convertJSArrayToNumberVector<uint8_t>(bytes));
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
    // Only the byte ranges dirty_ranges() reports, each as its own Uint8Array (wd1003.h), so the
    // front end patches its copy of C: instead of pulling the whole image.
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

    // ---- CD-ROM (atapi_cdrom) ----
    void mountCdrom(val bytes) {
        std::vector<uint8_t> data = emscripten::convertJSArrayToNumberVector<uint8_t>(bytes);
        m_.chipset.cdrom.mount(data.data(), data.size());
    }
    // Mixed-mode disc: a CUE sheet naming the one BIN in `binBytes`. Returns false if the sheet
    // doesn't parse (atapi_cdrom.h mount_cue()).
    bool mountCdromCue(const std::string &cueText, val binBytes) {
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

    // ---- PC speaker ----
    bool speakerLevel() const { return m_.chipset.speaker.level(); }

    // ---- test-only: text-mode screen as a string ----
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

    // ---- Sound Blaster 16 ----
    // Drains samples the DSP produced since the last call, paced at the programmed sample rate.
    // The OPL3's stream. Separate from sbDrainSamples(): the card sums FM and digitized audio in the
    // analog domain behind separate CT1745 attenuators, which the front end applies.
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

    // The CD-ROM's analog audio leg, same shape as fmDrainSamples(). It never crosses the ATA bus;
    // the front end mixes it using cdGainLeft()/cdGainRight().
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

    // Test-only: the machine's real I/O decode, to drive a device through its ports.
    uint8_t portIn(uint16_t port) { return m_.chipset.io_in(port); }
    void portOut(uint16_t port, uint8_t v) { m_.chipset.io_out(port, v); }
    uint8_t fmReg(uint16_t index) const { return m_.chipset.sb.fm.reg(index); }
    bool fmOpl3Mode() const { return m_.chipset.sb.fm.opl3_mode(); }

    // ---- OPL3 register write trace (opt-in, app.js window.__fm) ----
    // Captures each FM register write with its CPU cycle stamp.
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

    // Cycles the 486 spent halted; differenced against totalCycles() for guest CPU usage (Cpu::halt_cycles).
    double haltCycles() const { return double(m_.cpu.halt_cycles); }

    // Cycles spent in a DOS wait loop rather than halted (Cpu::idle_poll_cycles). Added to halted
    // cycles, it makes a usage figure meaningful for an OS with no idle.
    double idleCycles() const { return double(m_.cpu.idle_poll_cycles); }

    // The wasm heap: 32MB guest RAM, video memory, disk images in flight and the rest.
    double heapBytes() const { return double(emscripten_get_heap_size()); }

    // Whether this build carries the emulator's own counters (Performance panel Tier 2). The shipped binary answers false.
    bool perfBuild() const {
#ifdef PC486_PERF
        return true;
#else
        return false;
#endif
    }
#ifdef PC486_PERF
    // Counters since the last call as "name=value" pairs. Reading resets them.
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

    // The busiest instruction forms since the last call, ranked. "0f" marks the two-byte map.
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
    // Raw CT1745 mixer register for the tone controls (44h-47h); see app.js refreshSbTone().
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
        .function("setRtc", &WasmMachine::setRtc)
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
        .function("keyboardRepeating", &WasmMachine::keyboardRepeating)
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
