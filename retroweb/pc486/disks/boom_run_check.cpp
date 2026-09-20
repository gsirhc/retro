// Boots the shipped FreeDOS HDD image and runs BOOM (a DOS Doom source port
// built with DJGPP, loaded by the GO32-V2 stub through the CWSDPMI DPMI host)
// off C:\GAMES\BOOM, then holds the machine to the whole Milestone 4 bar --
// in the same shape as hdd_boot_check's "reaches a live C:\>" and
// vbe_mode13_check's per-pixel proof: real third-party protected-mode
// software exercising this hardware, not a test written to its expectations.
// Three phases, all of which must pass:
//
//   1. The renderer runs: a run of genuinely *different* 320x200x256 frames.
//   2. The PS/2 mouse: synthetic AUX-port packets reach BOOM. A left click
//      during the attract demo brings up its menu, and in a live game
//      injected horizontal motion turns the player's view and turning the
//      same distance back restores the frame it started from. The whole
//      chain is real: i8042 AUX -> IRQ12 -> the firmware's INT 74h ->
//      CTMOUSE's INT 33h driver -> Allegro -> BOOM (PC486_REVIEW.md §13).
//   3. Digitized sound: the Sound Blaster's DAC has to have clocked out real
//      audio -- a waveform, at the rate the driver programmed, with IRQ5
//      firing per block.
//
// Usage:
//   boom_run_check <bios> <vgabios> <hdd.img> [options]
//     --bmp PREFIX      write PREFIX-NNN.bmp screenshots as frames arrive,
//                       plus one per step of the mouse phase
//     --budget CYCLES   cycle budget for the BOOM run itself
//     --sb-trace        log every Sound Blaster DSP state change (rate,
//                       width, channels, speaker gate) as it happens
//
//   Diagnostics (all imply --trace, which keeps an instruction ring buffer
//   and costs real time; §9 of PC486_REVIEW.md is the worked example of
//   using them together):
//     --trace           ring-buffer every instruction and dump it at the
//                       first #PF/#GP/#DF
//     --trace-cr2 HEX   narrow that dump to a #PF at this CR2
//     --catch-runaway   dump the ring the moment the guest starts executing
//                       zeroed memory -- i.e. the moment control flow left
//                       real code, while its history is still in the buffer
//     --watch LINEAR    dump the ring when this linear dword changes; the
//                       instruction that changed it is the newest entry
//     --dump-at CYCLES  dump the ring at a chosen point in the run, for a
//                       guest that is stuck rather than crashed
//     --ring N          how many ring entries a dump prints (default 600)
//
// Phase 1's bar is a *changing* picture, not a picture: BOOM's title screen
// is one static frame a working bitmap loader alone would produce. Any
// CWSDPMI crash-handler screen, DPMI error or DOS-level abort fails the run
// and prints the captured evidence. Exit codes: 1 no renderer, 3 runaway,
// 4 watchpoint, 5 sound, 6 mouse.

#include "../machine.h"
#include "../ega_render.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace pc486;

namespace {

std::vector<uint8_t> ReadFile(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Same planar-VRAM text reconstruction hdd_boot_check.cpp uses; plane 0 is
// the character plane and the CRTC's start address is honored so a scrolled
// screen still reads correctly.
std::string ScreenText(Machine &m) {
    const auto &vga = m.chipset.vga;
    std::string out;
    for (int row = 0; row < 25; ++row) {
        std::string line;
        for (int col = 0; col < 80; ++col) {
            uint32_t plane_off = (uint32_t(vga.start_offset()) + uint32_t(row * 80 + col)) & 0xFFFF;
            uint8_t ch = vga.vram[(plane_off << 2) + 0];
            line.push_back((ch >= 32 && ch < 127) ? char(ch) : ' ');
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line;
        out += '\n';
    }
    return out;
}

// --- keyboard -------------------------------------------------------------
// Set 1 (XT) make codes, break code = make | 0x80, exactly as a real
// keyboard reports them to the 8042 (same table as build_freedos_hdd.cpp,
// extended with the punctuation a path needs).
uint8_t Set1MakeCode(char c) {
    static const std::map<char, uint8_t> table = {
        {'1', 0x02}, {'2', 0x03}, {'3', 0x04}, {'4', 0x05}, {'5', 0x06},
        {'6', 0x07}, {'7', 0x08}, {'8', 0x09}, {'9', 0x0A}, {'0', 0x0B},
        {'-', 0x0C}, {'=', 0x0D},
        {'q', 0x10}, {'w', 0x11}, {'e', 0x12}, {'r', 0x13}, {'t', 0x14},
        {'y', 0x15}, {'u', 0x16}, {'i', 0x17}, {'o', 0x18}, {'p', 0x19},
        {'[', 0x1A}, {']', 0x1B},
        {'a', 0x1E}, {'s', 0x1F}, {'d', 0x20}, {'f', 0x21}, {'g', 0x22},
        {'h', 0x23}, {'j', 0x24}, {'k', 0x25}, {'l', 0x26},
        {';', 0x27}, {'\'', 0x28}, {'`', 0x29}, {'\\', 0x2B},
        {'z', 0x2C}, {'x', 0x2D}, {'c', 0x2E}, {'v', 0x2F}, {'b', 0x30},
        {'n', 0x31}, {'m', 0x32}, {',', 0x33}, {'.', 0x34}, {'/', 0x35},
        {' ', 0x39}, {'\n', 0x1C}, {'\r', 0x1C}, {'\x1b', 0x01},
    };
    auto it = table.find(c);
    return it != table.end() ? it->second : 0;
}

void SendKey(Machine &m, uint8_t make) {
    m.chipset.kbc.inject_scancode(make);
    m.run_cycles(150000);
    m.chipset.kbc.inject_scancode(uint8_t(make | 0x80));
    m.run_cycles(150000);
}

void SendString(Machine &m, const std::string &s) {
    for (char c : s) {
        uint8_t mk = Set1MakeCode(c);
        if (mk) SendKey(m, mk);
    }
}

constexpr uint8_t kScanEnter = 0x1C;

// --- guest memory, read the way the CPU would ----------------------------
// Walks the live page tables so a harness can read guest *linear* memory
// (for instruction bytes at a faulting EIP). Returns false if the walk
// cannot resolve the address -- which is itself information.
bool ReadLinearByte(Machine &m, uint32_t linear, uint8_t &out) {
    if (!m.cpu.paging_enabled()) { out = m.chipset.mem_read(linear); return true; }
    uint32_t pde_addr = (m.cpu.cr(3) & 0xFFFFF000u) + ((linear >> 22) << 2);
    uint32_t pde = 0;
    for (int i = 0; i < 4; ++i) pde |= uint32_t(m.chipset.mem_read(pde_addr + uint32_t(i))) << (8 * i);
    if (!(pde & 1)) return false;
    uint32_t pte_addr = (pde & 0xFFFFF000u) + (((linear >> 12) & 0x3FFu) << 2);
    uint32_t pte = 0;
    for (int i = 0; i < 4; ++i) pte |= uint32_t(m.chipset.mem_read(pte_addr + uint32_t(i))) << (8 * i);
    if (!(pte & 1)) return false;
    out = m.chipset.mem_read((pte & 0xFFFFF000u) | (linear & 0xFFFu));
    return true;
}

std::string LinearBytes(Machine &m, uint32_t linear, int n) {
    std::string s;
    char buf[8];
    for (int i = 0; i < n; ++i) {
        uint8_t b = 0;
        if (!ReadLinearByte(m, linear + uint32_t(i), b)) { s += "?? "; continue; }
        std::snprintf(buf, sizeof buf, "%02X ", b);
        s += buf;
    }
    return s;
}

// --- instruction ring buffer ---------------------------------------------
struct TraceEntry {
    uint64_t cycle;
    uint16_t cs;
    uint32_t cs_base, eip;
    uint32_t eax, ebx, ecx, edx, esi, edi, esp, ebp;
    uint32_t eflags, cr0, cr2, cr3;
    uint16_t ds, es, ss;
    uint32_t ds_base, es_base, ss_base, es_limit;
    uint8_t  cpl;
    bool     paging;
};

struct FaultRecord {
    uint64_t seq;        // trace index when it fired
    uint64_t cycle;
    int      vector;
    uint32_t error;
    uint16_t cs;
    uint32_t eip;
    uint32_t cr2;
    uint32_t cr3;
    int      cpl;
};

struct Diag {
    static constexpr std::size_t kRing = 24576;
    std::vector<TraceEntry> ring = std::vector<TraceEntry>(kRing);
    uint64_t seq = 0;
    bool trace_on = false;
    std::vector<FaultRecord> faults;
    std::map<int, uint64_t> fault_counts;
    // Faults grouped by (vector, cr2) so a recurring, never-repaired fault
    // is visible as a count rather than a wall of identical lines.
    std::map<std::pair<int, uint32_t>, uint64_t> pf_by_cr2;
    // Sampled CS:EIP histogram -- the §5.9 technique for "where is it
    // spinning?", at one sample per kSampleEvery instructions so it costs
    // nothing measurable.
    static constexpr uint64_t kSampleEvery = 4096;
    uint64_t instr = 0;
    std::map<uint64_t, uint64_t> hot;   // (cs<<32)|eip -> samples
    // Unimplemented opcodes, by (cs:eip, opcode word): a guest dying on an
    // opcode this core does not have looks identical to a logic bug from the
    // outside, so it is worth separating by evidence.
    std::map<std::pair<uint64_t, uint16_t>, uint64_t> unimpl;
    bool catch_runaway = false;
    bool runaway_dumped = false;
    int  zero_run = 0;
    std::size_t runaway_dump_count = 600;
    // The IVT as it stood when BOOM was launched, so a corrupted vector can
    // be told apart from a wiped handler.
    std::vector<uint8_t> ivt_at_start = std::vector<uint8_t>(1024, 0);
    // Memory watchpoint: the linear dword to watch, and its last observed
    // value. Checked at each instruction boundary, so a change is attributed
    // to the instruction that just retired -- the newest ring entry.
    bool     watch_on = false;
    uint32_t watch_lin = 0;
    uint32_t watch_val = 0;
    bool     watch_primed = false;
    bool     watch_fired = false;
    // IRQ5 (Sound Blaster) rising edges, counted exactly rather than sampled:
    // this is the same 0->1 transition chipset.cpp raises the line on.
    bool     sb_irq_prev = false;
    uint64_t sb_irq_edges = 0;
    // Entries to the real-mode INT 74h handler -- i.e. mouse packets the
    // firmware actually serviced, which separates "the AUX port queued a
    // packet" from "IRQ12 reached a handler" (§13).
    bool     irq12_watch = false;
    uint32_t irq12_handler_lin = 0;
    uint64_t irq12_entries = 0;
};

Diag g_diag;

// --- digitized audio the Sound Blaster's DAC actually latched -------------
// Accumulated from SoundBlaster::drain_samples() exactly as the browser
// front end drains it, so this harness sees precisely the sample stream a
// real listener would hear.
struct SoundLog {
    uint64_t samples = 0;
    uint64_t nonsilent = 0;            // away from digital silence (0 after normalization)
    int16_t  min_v = 0, max_v = 0;
    uint64_t first_sample_cycle = 0;
    uint64_t first_nonsilent_cycle = 0;
    uint64_t last_sample_cycle = 0;
    // Distinct sample values, capped: enough to tell a waveform from a
    // constant without holding the whole stream.
    std::set<int16_t> distinct;
    // DSP state, logged on change: rate, width, channels, speaker gate.
    bool     playing = false;
    uint32_t rate = 0;
    bool     bits16 = false, stereo = false, speaker = false;
    bool     trace = false;
};

SoundLog g_sound;

void PollSound(Machine &m) {
    SoundBlaster &sb = m.chipset.sb;
    bool playing = sb.playing();
    uint32_t rate = sb.sample_rate_hz();
    bool bits16 = sb.sixteen_bit(), stereo = sb.stereo(), speaker = sb.speaker_on();
    if (g_sound.trace &&
        (playing != g_sound.playing || rate != g_sound.rate || bits16 != g_sound.bits16 ||
         stereo != g_sound.stereo || speaker != g_sound.speaker)) {
        std::fprintf(stderr,
                     "[sb %6llu Mcyc] playing=%d %u Hz %d-bit %s speaker=%d irq5-edges=%llu\n",
                     (unsigned long long)(m.total_cycles() / 1'000'000), playing ? 1 : 0, rate,
                     bits16 ? 16 : 8, stereo ? "stereo" : "mono", speaker ? 1 : 0,
                     (unsigned long long)g_diag.sb_irq_edges);
    }
    g_sound.playing = playing; g_sound.rate = rate;
    g_sound.bits16 = bits16; g_sound.stereo = stereo; g_sound.speaker = speaker;

    for (const SoundBlaster::Sample &s : sb.drain_samples()) {
        if (g_sound.samples == 0) g_sound.first_sample_cycle = s.cpu_cycle;
        ++g_sound.samples;
        g_sound.last_sample_cycle = s.cpu_cycle;
        for (int16_t v : {s.left, s.right}) {
            if (v < g_sound.min_v) g_sound.min_v = v;
            if (v > g_sound.max_v) g_sound.max_v = v;
            if (g_sound.distinct.size() < 4096) g_sound.distinct.insert(v);
        }
        if (s.left != 0 || s.right != 0) {
            if (g_sound.nonsilent == 0) g_sound.first_nonsilent_cycle = s.cpu_cycle;
            ++g_sound.nonsilent;
        }
    }
}

void DumpRing(Machine &m, std::size_t count);

void RecordInstruction(void *, Machine &m) {
    if (++g_diag.instr % Diag::kSampleEvery == 0)
        ++g_diag.hot[(uint64_t(m.cpu.cs) << 32) | m.cpu.eip];
    bool sb_irq = m.chipset.sb.irq_pending();
    if (sb_irq && !g_diag.sb_irq_prev) ++g_diag.sb_irq_edges;
    g_diag.sb_irq_prev = sb_irq;
    if (g_diag.irq12_watch && !m.cpu.paging_enabled() &&
        m.cpu.desc(cpu80486::Cpu::SEG_CS).base + m.cpu.eip == g_diag.irq12_handler_lin)
        ++g_diag.irq12_entries;
    if (!g_diag.trace_on) return;
    // Runaway detector. A guest whose control flow has left real code grinds
    // through zeroed memory executing 00 00 (ADD [BX+SI],AL) two bytes at a
    // time -- the §5.9 signature. Catching the *first* few of those, rather
    // than noticing the spin billions of cycles later, is what keeps the
    // preceding real instructions in the ring buffer where they can be read.
    if (g_diag.watch_on && !g_diag.watch_fired) {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= uint32_t(m.chipset.mem_read(g_diag.watch_lin + uint32_t(i))) << (8 * i);
        if (!g_diag.watch_primed) { g_diag.watch_val = v; g_diag.watch_primed = true; }
        else if (v != g_diag.watch_val) {
            g_diag.watch_fired = true;
            std::fprintf(stderr,
                         "\n=== WATCHPOINT lin=%08X changed %08X -> %08X at cycle %llu; the "
                         "instruction that did it is the LAST line below ===\n",
                         g_diag.watch_lin, g_diag.watch_val, v, (unsigned long long)m.total_cycles());
            DumpRing(m, g_diag.runaway_dump_count);
            std::fflush(stderr);
        }
    }
    if (g_diag.catch_runaway && !g_diag.runaway_dumped) {
        uint32_t lin = m.cpu.desc(cpu80486::Cpu::SEG_CS).base + m.cpu.eip;
        if (m.chipset.mem_read(lin) == 0 && m.chipset.mem_read(lin + 1) == 0) {
            if (++g_diag.zero_run == 8) {
                g_diag.runaway_dumped = true;
                std::fprintf(stderr,
                             "\n=== RUNAWAY: executing zeroed memory at %04X:%08X (lin=%08X), "
                             "cycle %llu ===\n",
                             m.cpu.cs, m.cpu.eip, lin, (unsigned long long)m.total_cycles());
                // Which is it: a corrupted interrupt vector, or a vector
                // that still points where it always did at memory that has
                // since been wiped? Comparing the live IVT against the
                // snapshot taken when BOOM started answers that outright.
                std::fprintf(stderr, "=== IVT entries changed since BOOM started ===\n");
                int changes = 0;
                for (int v = 0; v < 256; ++v) {
                    uint32_t now = 0, then = 0;
                    for (int i = 0; i < 4; ++i) {
                        now |= uint32_t(m.chipset.mem_read(uint32_t(v * 4 + i))) << (8 * i);
                        then |= uint32_t(g_diag.ivt_at_start[std::size_t(v * 4 + i)]) << (8 * i);
                    }
                    if (now != then) {
                        std::fprintf(stderr, "  INT %02X: %04X:%04X -> %04X:%04X\n", v,
                                     unsigned(then >> 16), unsigned(then & 0xFFFF),
                                     unsigned(now >> 16), unsigned(now & 0xFFFF));
                        ++changes;
                    }
                }
                if (!changes) std::fprintf(stderr, "  (none -- every vector is unchanged)\n");
                std::fprintf(stderr, "=== 48 bytes at the runaway target lin=%08X ===\n  %s\n",
                             lin & ~0xFu, LinearBytes(m, lin & ~0xFu, 48).c_str());
                std::fflush(stderr);
            }
        } else {
            g_diag.zero_run = 0;
        }
    }
    TraceEntry &e = g_diag.ring[g_diag.seq % Diag::kRing];
    const auto &c = m.cpu;
    e.cycle = m.total_cycles();
    e.cs = c.cs;
    e.cs_base = c.desc(cpu80486::Cpu::SEG_CS).base;
    e.eip = c.eip;
    e.eax = c.eax; e.ebx = c.ebx; e.ecx = c.ecx; e.edx = c.edx;
    e.esi = c.esi; e.edi = c.edi; e.esp = c.esp; e.ebp = c.ebp;
    e.eflags = c.eflags;
    e.cr0 = c.cr(0); e.cr2 = c.cr(2); e.cr3 = c.cr(3);
    e.ds = c.ds; e.es = c.es; e.ss = c.ss;
    e.ds_base = c.desc(cpu80486::Cpu::SEG_DS).base;
    e.es_base = c.desc(cpu80486::Cpu::SEG_ES).base;
    e.ss_base = c.desc(cpu80486::Cpu::SEG_SS).base;
    e.es_limit = c.desc(cpu80486::Cpu::SEG_ES).limit;
    e.cpl = uint8_t(c.cpl());
    e.paging = c.paging_enabled();
    ++g_diag.seq;
}

void DumpRing(Machine &m, std::size_t count) {
    std::size_t have = g_diag.seq < Diag::kRing ? std::size_t(g_diag.seq) : Diag::kRing;
    if (count > have) count = have;
    std::fprintf(stderr, "=== last %zu instructions (oldest first) ===\n", count);
    for (std::size_t i = count; i-- > 0;) {
        const TraceEntry &e = g_diag.ring[(g_diag.seq - 1 - i) % Diag::kRing];
        uint32_t lin = e.cs_base + e.eip;
        std::fprintf(stderr,
                     "%10llu cyc=%llu %04X:%08X lin=%08X cpl=%u pg=%d cr0=%08X "
                     "ds=%04X/%08X es=%04X/%08X(lim %08X) ss=%04X/%08X  "
                     "eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X esp=%08X ebp=%08X fl=%08X | %s\n",
                     (unsigned long long)(g_diag.seq - 1 - i), (unsigned long long)e.cycle,
                     e.cs, e.eip, lin, e.cpl, e.paging ? 1 : 0, e.cr0,
                     e.ds, e.ds_base, e.es, e.es_base, e.es_limit, e.ss, e.ss_base,
                     e.eax, e.ebx, e.ecx, e.edx, e.esi, e.edi, e.esp, e.ebp, e.eflags,
                     LinearBytes(m, lin, 10).c_str());
    }
}

// --- BMP output (same 24-bit bottom-up layout render_screen.cpp writes) ---
void WriteBmp(const std::string &path, const RenderedFrame &f) {
    if (f.width <= 0 || f.height <= 0) return;
    const int row_bytes = ((f.width * 3) + 3) & ~3;
    const uint32_t pix_size = uint32_t(row_bytes * f.height);
    const uint32_t off = 14 + 40;
    std::vector<uint8_t> out(off + pix_size, 0);
    auto put32 = [&](std::size_t at, uint32_t v) {
        out[at] = uint8_t(v); out[at + 1] = uint8_t(v >> 8);
        out[at + 2] = uint8_t(v >> 16); out[at + 3] = uint8_t(v >> 24);
    };
    out[0] = 'B'; out[1] = 'M';
    put32(2, off + pix_size);
    put32(10, off);
    put32(14, 40);
    put32(18, uint32_t(f.width));
    put32(22, uint32_t(f.height));
    out[26] = 1; out[28] = 24;
    for (int y = 0; y < f.height; ++y) {
        const uint8_t *src = f.rgba.data() + std::size_t(y) * std::size_t(f.width) * 4;
        uint8_t *dst = out.data() + off + std::size_t(f.height - 1 - y) * std::size_t(row_bytes);
        for (int x = 0; x < f.width; ++x) {
            dst[x * 3 + 0] = src[x * 4 + 2];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 0];
        }
    }
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(out.data()), std::streamsize(out.size()));
}

uint64_t HashFrame(const RenderedFrame &f) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : f.rgba) { h ^= b; h *= 1099511628211ull; }
    return h ^ (uint64_t(f.width) << 32) ^ uint64_t(f.height);
}

// How many pixels of the frame are not the background index -- a static
// black screen and a live 3D view are trivially distinguishable, and this
// keeps "the renderer ran" from being satisfied by a cleared screen.
std::size_t DistinctColors(const RenderedFrame &f) {
    std::set<uint32_t> seen;
    for (std::size_t i = 0; i + 3 < f.rgba.size(); i += 4) {
        seen.insert(uint32_t(f.rgba[i]) | (uint32_t(f.rgba[i + 1]) << 8) | (uint32_t(f.rgba[i + 2]) << 16));
        if (seen.size() > 64) break;
    }
    return seen.size();
}

bool Contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

// Text the DPMI host, the go32 stub or DOS itself prints when the run has
// already failed -- checked so the harness reports the real message instead
// of timing out with a screen nobody looks at.
const char *kFailureMarkers[] = {
    "Page fault", "Page Fault", "PAGE FAULT",
    "General Protection Fault", "Exiting due to signal",
    "no DPMI", "No DPMI", "DPMI memory", "Not enough DPMI",
    "Load error", "Cannot allocate", "cannot allocate",
};

// --- running the guest past the render bar --------------------------------
struct FrameProbe {
    RenderedFrame last;
    uint64_t last_hash = 0;
    int distinct = 0;
};

// Runs the guest while keeping the audio log drained (the front end's own
// job) and counting distinct mode-13h frames.
void RunAndPoll(Machine &m, uint64_t cycles, FrameProbe *probe = nullptr) {
    const uint64_t kStep = 2'000'000;
    for (uint64_t used = 0; used < cycles; used += kStep) {
        m.run_cycles(int64_t(kStep));
        PollSound(m);
        if (probe && DetectScreenMode(m.chipset.vga) == ScreenMode::kVga256) {
            RenderScreen(m.chipset.vga, probe->last, true);
            uint64_t h = HashFrame(probe->last);
            if (h != probe->last_hash) { probe->last_hash = h; ++probe->distinct; }
        }
    }
}

// --- phase 3: digitized sound --------------------------------------------
// Allegro's sb.c programs the DSP straight through port I/O -- no DOS driver
// or TSR in the path -- so whether BOOM's sound effects reach the DAC is a
// question about this machine's own hardware wiring and nothing else. Run
// last, because it judges the whole run: the sample log has been draining
// since BOOM started, phase 2's menu included.
bool CheckSound(Machine &m) {
    std::fprintf(stderr, "\n=== phase 3: digitized sound through the Sound Blaster ===\n");
    std::fprintf(stderr,
                 "  samples=%llu nonsilent=%llu amplitude=[%d..%d] rate=%u Hz %d-bit %s\n"
                 "  first sample at cycle %llu, first nonsilent at %llu, last at %llu\n"
                 "  IRQ5 rising edges=%llu speaker=%d mixer 80h=%02X 81h=%02X 82h=%02X\n",
                 (unsigned long long)g_sound.samples, (unsigned long long)g_sound.nonsilent,
                 g_sound.min_v, g_sound.max_v, m.chipset.sb.sample_rate_hz(),
                 m.chipset.sb.sixteen_bit() ? 16 : 8, m.chipset.sb.stereo() ? "stereo" : "mono",
                 (unsigned long long)g_sound.first_sample_cycle,
                 (unsigned long long)g_sound.first_nonsilent_cycle,
                 (unsigned long long)g_sound.last_sample_cycle,
                 (unsigned long long)g_diag.sb_irq_edges, m.chipset.sb.speaker_on() ? 1 : 0,
                 m.chipset.sb.mixer_register(0x80), m.chipset.sb.mixer_register(0x81),
                 m.chipset.sb.mixer_register(0x82));
    std::fprintf(stderr, "  waveform: %zu distinct sample values, peak |amplitude| %d\n",
                 g_sound.distinct.size(),
                 std::max(int(g_sound.max_v), -int(g_sound.min_v)));
    bool ok = true;
    if (g_sound.samples == 0) {
        std::fprintf(stderr, "FAILED: the DSP never clocked a single sample out\n");
        ok = false;
    }
    if (g_sound.nonsilent < 1000) {
        std::fprintf(stderr, "FAILED: the DMA path moved no actual audio -- the DAC saw only "
                             "digital silence\n");
        ok = false;
    }
    // A constant is not audio. This is the bar that a DC level would fail:
    // §13's inverted-DMA bug played the card's own zeroed buffer, which is a
    // full-scale negative constant in the unsigned format Allegro programs --
    // "not silent" by any amplitude test, and inaudible as anything but a
    // click. Real mixed game audio moves through thousands of values.
    if (g_sound.distinct.size() < 256) {
        std::fprintf(stderr, "FAILED: the sample stream is a constant, not a waveform\n");
        ok = false;
    }
    if (std::max(int(g_sound.max_v), -int(g_sound.min_v)) < 4096) {
        std::fprintf(stderr, "FAILED: the sample stream never rises above the noise floor\n");
        ok = false;
    }
    if (g_diag.sb_irq_edges == 0) {
        std::fprintf(stderr, "FAILED: IRQ5 never fired, so the driver was never told a block "
                             "finished\n");
        ok = false;
    }
    // Pacing: the sample timestamps are the DAC's own clock, so the count
    // across the run has to match the programmed rate. This is the same
    // "never faster than real hardware" bar PlaybackIsPacedAtTheRealSampleRate
    // holds the device to, measured here against real third-party software's
    // own rate rather than a test's.
    if (g_sound.samples > 1 && g_sound.rate != 0) {
        double span = double(g_sound.last_sample_cycle - g_sound.first_sample_cycle);
        double expect = span / (Machine::kCpuHz / double(g_sound.rate));
        double ratio = expect > 0 ? double(g_sound.samples) / expect : 0.0;
        std::fprintf(stderr, "  pacing: %llu samples over %.2f s of emulated time = %.0f Hz "
                             "(programmed %u Hz, ratio %.3f)\n",
                     (unsigned long long)g_sound.samples, span / Machine::kCpuHz,
                     span > 0 ? double(g_sound.samples) * Machine::kCpuHz / span : 0.0,
                     g_sound.rate, ratio);
        if (ratio > 1.05) {
            std::fprintf(stderr, "FAILED: playback ran faster than the rate the driver "
                                 "programmed\n");
            ok = false;
        }
    }
    if (ok) std::fprintf(stderr, "OK: BOOM's sound effects reached the DAC.\n");
    return ok;
}

// --- phase 2: the PS/2 mouse ---------------------------------------------
void Snap(Machine &m, RenderedFrame &f) { RenderScreen(m.chipset.vga, f, true); }

void DumpFrame(const std::string &prefix, const char *tag, const RenderedFrame &f) {
    if (prefix.empty()) return;
    WriteBmp(prefix + "-mouse-" + tag + ".bmp", f);
}

// How many pixels of the 3D view hold the same color across every frame in a
// short burst. The attract demo repaints that whole view every frame, so a
// static overlay -- a menu -- shows up as a large jump in this number,
// without the harness knowing where BOOM happens to draw its menu. The
// status bar is left out for the reason ViewDiff gives below.
long StablePixels(Machine &m, FrameProbe *probe, int frames, uint64_t gap_cycles) {
    std::vector<RenderedFrame> shots;
    shots.resize(std::size_t(frames));
    for (int i = 0; i < frames; ++i) {
        Snap(m, shots[std::size_t(i)]);
        if (i + 1 < frames) RunAndPoll(m, gap_cycles, probe);
    }
    const RenderedFrame &f0 = shots[0];
    const std::size_t limit =
        std::min(f0.rgba.size(), std::size_t(f0.width) * std::size_t(std::min(f0.height, 168)) * 4);
    long same = 0;
    for (std::size_t i = 0; i + 3 < limit; i += 4) {
        bool all = true;
        for (int k = 1; k < frames && all; ++k) {
            const RenderedFrame &fk = shots[std::size_t(k)];
            if (fk.rgba.size() != f0.rgba.size()) { all = false; break; }
            all = fk.rgba[i] == f0.rgba[i] && fk.rgba[i + 1] == f0.rgba[i + 1] &&
                  fk.rgba[i + 2] == f0.rgba[i + 2];
        }
        if (all) ++same;
    }
    return same;
}

// Differing pixels in the 3D view only (rows 0..167 of mode 13h): the status
// bar's face sprite animates on its own timer, so counting it would put a
// floor under every "nothing moved" measurement.
long ViewDiff(const RenderedFrame &a, const RenderedFrame &b) {
    if (a.width != b.width || a.height != b.height) return -1;
    long n = 0;
    const int rows = std::min(a.height, 168);
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < a.width; ++x) {
            std::size_t i = (std::size_t(y) * std::size_t(a.width) + std::size_t(x)) * 4;
            if (a.rgba[i] != b.rgba[i] || a.rgba[i + 1] != b.rgba[i + 1] ||
                a.rgba[i + 2] != b.rgba[i + 2]) ++n;
        }
    }
    return n;
}

bool CheckMouse(Machine &m, const std::string &bmp_prefix) {
    std::fprintf(stderr, "\n=== phase 2: the PS/2 mouse ===\n");
    // Vector 0x74 is IRQ12's, and its handler is the firmware's; counting
    // entries to it separates "the AUX port queued a packet" from "the
    // interrupt reached a handler", which is the distinction §13's PIC bug
    // turned on.
    uint32_t v74 = 0;
    for (int i = 0; i < 4; ++i) v74 |= uint32_t(m.chipset.mem_read(0x74 * 4 + uint32_t(i))) << (8 * i);
    g_diag.irq12_handler_lin = ((v74 >> 16) << 4) + (v74 & 0xFFFF);
    g_diag.irq12_watch = true;
    bool reporting = m.chipset.kbc.mouse_reporting_enabled();
    std::fprintf(stderr, "  INT 74h vector = %04X:%04X (lin %05X); the AUX port is in stream mode "
                         "with reporting enabled: %d\n",
                 unsigned(v74 >> 16), unsigned(v74 & 0xFFFF), g_diag.irq12_handler_lin,
                 reporting ? 1 : 0);
    if (!reporting) {
        std::fprintf(stderr, "FAILED: no driver ever enabled the mouse -- CTMOUSE did not install "
                             "(FDAUTO.BAT only runs it on the 386+ path, and that needs the CPU to "
                             "identify itself; see PC486_REVIEW.md §13)\n");
        return false;
    }

    FrameProbe probe;
    RenderedFrame rest, moved_frame, back_frame, shot;

    // A click first, with no keyboard anywhere in the path: DOOM's own
    // G_Responder treats any mouse button during demo playback exactly like a
    // keypress and calls M_StartControlPanel, so this is what brings the menu
    // up. It is captured as evidence; the pass/fail measurements are the ones
    // below, taken in a live game where a still screen makes them decisive.
    long stable_demo = StablePixels(m, &probe, 4, 20'000'000);
    uint64_t irq12_before = g_diag.irq12_entries;
    m.chipset.inject_mouse_event(0, 0, I8042::kMouseLeft);
    RunAndPoll(m, 20'000'000, &probe);
    m.chipset.inject_mouse_event(0, 0, 0);
    RunAndPoll(m, 200'000'000, &probe);
    long stable_menu = StablePixels(m, &probe, 4, 20'000'000);
    Snap(m, shot);
    DumpFrame(bmp_prefix, "0-menu", shot);
    std::fprintf(stderr, "  left click during the attract demo: %llu INT 74h entries (one per "
                         "packet byte, press and release), 3D-view pixels holding still across 4 "
                         "frames %ld -> %ld of %d -- BOOM's menu is now drawn over the demo\n",
                 (unsigned long long)(g_diag.irq12_entries - irq12_before),
                 stable_demo, stable_menu, 320 * 168);
    if (g_diag.irq12_entries == irq12_before) {
        std::fprintf(stderr, "FAILED: the click never reached an interrupt handler\n");
        return false;
    }

    // Walking that menu down into a live game is keyboard, deliberately: the
    // mouse is the thing under test, and a standing player in a freshly
    // started level is the still screen that makes a turn measurable. Four
    // ENTERs cover both menu shapes (an episode picker or straight to skill);
    // a spare ENTER does nothing in play.
    for (int i = 0; i < 4; ++i) {
        SendKey(m, kScanEnter);
        RunAndPoll(m, 200'000'000, &probe);
    }
    RunAndPoll(m, 600'000'000, &probe);
    Snap(m, rest);
    DumpFrame(bmp_prefix, "1-ingame", rest);

    // What the view does on its own over the span a turn will take: animated
    // textures, a monster in sight, the status bar's own face sprite. Every
    // measurement below is judged against this, not against zero.
    RunAndPoll(m, 300'000'000, &probe);
    Snap(m, moved_frame);
    long idle = ViewDiff(rest, moved_frame);

    Snap(m, rest);
    for (int i = 0; i < 6; ++i) {
        m.chipset.inject_mouse_event(80, 0, 0);
        RunAndPoll(m, 20'000'000, &probe);
    }
    RunAndPoll(m, 180'000'000, &probe);
    Snap(m, moved_frame);
    long moved = ViewDiff(rest, moved_frame);
    DumpFrame(bmp_prefix, "2-turned", moved_frame);

    // The same count of counts back the other way. A view that turns and then
    // returns to the frame it started from is the mouse driving the player's
    // angle -- and it is the arithmetic, not just the delivery: BOOM has to
    // have seen the same total movement in both directions.
    for (int i = 0; i < 6; ++i) {
        m.chipset.inject_mouse_event(-80, 0, 0);
        RunAndPoll(m, 20'000'000, &probe);
    }
    RunAndPoll(m, 180'000'000, &probe);
    Snap(m, back_frame);
    long back = ViewDiff(rest, back_frame);
    DumpFrame(bmp_prefix, "3-returned", back_frame);

    std::fprintf(stderr, "  motion: 3D-view pixels changed -- %ld standing still, %ld after 6x "
                         "dx=+80, %ld after turning the same distance back (of %d)\n",
                 idle, moved, back, 320 * 168);

    // Button 1 in play is the fire key (BOOM.CFG's mouseb_fire 0). A shot
    // lights the room and draws the firing frame of the weapon, so it lands
    // as a large transient against a standing player's still view.
    Snap(m, rest);
    m.chipset.inject_mouse_event(0, 0, I8042::kMouseLeft);
    long fired = 0;
    for (int i = 0; i < 24; ++i) {
        RunAndPoll(m, 2'000'000, &probe);
        Snap(m, shot);
        long d = ViewDiff(rest, shot);
        if (d > fired) { fired = d; if (d > idle * 4) DumpFrame(bmp_prefix, "4-fired", shot); }
    }
    m.chipset.inject_mouse_event(0, 0, 0);
    RunAndPoll(m, 100'000'000, &probe);
    std::fprintf(stderr, "  button: pressing mouse button 1 in play changed up to %ld view pixels "
                         "against %ld standing still; %llu INT 74h entries in the phase\n",
                 fired, idle, (unsigned long long)g_diag.irq12_entries);

    bool ok = true;
    const long kView = 320L * 168;
    // Every comparison below is against a view that is supposed to be
    // standing still; if it isn't, BOOM never left its attract demo and none
    // of the numbers mean anything.
    if (idle > kView / 10) {
        std::fprintf(stderr, "FAILED: never reached a standing-still view -- the menu walk did not "
                             "start a game\n");
        return false;
    }
    if (moved < idle * 4 || moved < kView / 8) {
        std::fprintf(stderr, "FAILED: mouse motion did not turn the player's view\n");
        ok = false;
    }
    if (back > moved / 8) {
        std::fprintf(stderr, "FAILED: turning back the same distance did not restore the view, so "
                             "the motion counts BOOM saw were not the ones injected\n");
        ok = false;
    }
    if (fired < idle * 4 || fired < 1000) {
        std::fprintf(stderr, "FAILED: the mouse button did not fire the weapon\n");
        ok = false;
    }
    if (ok)
        std::fprintf(stderr, "OK: BOOM answers the mouse -- a click brought up its menu, injected "
                             "motion turned the player's view and gave it back, and button 1 "
                             "fired.\n");
    return ok;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <bios> <vgabios> <hdd.img> [--trace] [--trace-cr2 HEX]"
                             " [--bmp PREFIX] [--budget CYCLES]\n", argv[0]);
        return 2;
    }
    bool want_trace = false;
    bool have_trace_cr2 = false;
    uint32_t trace_cr2 = 0;
    std::string bmp_prefix;
    uint64_t run_budget = 40'000'000'000ull;
    uint64_t dump_at = 0;
    for (int i = 4; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--trace") want_trace = true;
        else if (a == "--trace-cr2" && i + 1 < argc) { have_trace_cr2 = true; trace_cr2 = uint32_t(std::strtoul(argv[++i], nullptr, 16)); want_trace = true; }
        else if (a == "--bmp" && i + 1 < argc) bmp_prefix = argv[++i];
        else if (a == "--budget" && i + 1 < argc) run_budget = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--dump-at" && i + 1 < argc) { dump_at = std::strtoull(argv[++i], nullptr, 10); want_trace = true; }
        else if (a == "--catch-runaway") { g_diag.catch_runaway = true; want_trace = true; }
        else if (a == "--watch" && i + 1 < argc) { g_diag.watch_on = true; want_trace = true; g_diag.watch_lin = uint32_t(std::strtoul(argv[++i], nullptr, 16)); }
        else if (a == "--ring" && i + 1 < argc) g_diag.runaway_dump_count = std::strtoul(argv[++i], nullptr, 10);
        else if (a == "--sb-trace") g_sound.trace = true;
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }

    Machine m;
    m.reset();
    {
        auto bios = ReadFile(argv[1]);
        auto vga = ReadFile(argv[2]);
        if (bios.empty() || vga.empty()) { std::fprintf(stderr, "cannot open BIOS/VGABIOS\n"); return 2; }
        m.chipset.load_rom(0x100000 - uint32_t(bios.size()), bios.data(), bios.size());
        m.chipset.load_rom(0xC0000, vga.data(), vga.size());
    }
    {
        auto hdd = ReadFile(argv[3]);
        if (hdd.empty()) { std::fprintf(stderr, "cannot open %s\n", argv[3]); return 2; }
        m.chipset.hdd.mount(0, hdd.data(), hdd.size());
    }

    m.on_instruction = &RecordInstruction;
    m.cpu.on_unimplemented = [&](uint16_t cs, uint32_t eip, uint16_t opword) {
        ++g_diag.unimpl[{(uint64_t(cs) << 32) | eip, opword}];
    };

    // Fault log. Every fault is counted; the ones that matter are the ones
    // CWSDPMI does not repair, which show up as the same (vector, CR2)
    // firing over and over.
    bool dump_armed = want_trace;
    bool dumped = false;
    m.cpu.on_fault = [&](int vec, uint32_t err, uint16_t cs, uint32_t eip) {
        ++g_diag.fault_counts[vec];
        uint32_t cr2 = m.cpu.cr(2);
        if (vec == 14) ++g_diag.pf_by_cr2[{vec, cr2 & 0xFFFFF000u}];
        if (g_diag.faults.size() < 4096) {
            g_diag.faults.push_back({g_diag.seq, m.total_cycles(), vec, err, cs, eip, cr2,
                                     m.cpu.cr(3), m.cpu.cpl()});
        }
        if (dump_armed && !dumped && g_diag.trace_on) {
            bool interesting = have_trace_cr2 ? (vec == 14 && (cr2 & 0xFFFFF000u) == (trace_cr2 & 0xFFFFF000u))
                                              : (vec == 14 || vec == 13 || vec == 8);
            if (interesting) {
                dumped = true;
                std::fprintf(stderr,
                             "\n=== FAULT vector=%d error=%04X cs=%04X eip=%08X cr2=%08X cr3=%08X cpl=%d "
                             "cycle=%llu ===\n",
                             vec, err, cs, eip, cr2, m.cpu.cr(3), m.cpu.cpl(),
                             (unsigned long long)m.total_cycles());
                DumpRing(m, 400);
            }
        }
    };

    // --- boot to an idle C:\> --------------------------------------------
    std::string screen, prev;
    uint64_t still_since = 0;
    const uint64_t kChunk = 500'000;
    bool booted = false;
    for (uint64_t used = 0; used < 40'000'000'000ull && !booted; used += kChunk) {
        m.run_cycles(int64_t(kChunk));
        screen = ScreenText(m);
        if (screen != prev) { still_since = m.total_cycles(); prev = screen; }
        if (Contains(screen, "C:\\>") && m.total_cycles() - still_since > 200'000'000) booted = true;
    }
    if (!booted) {
        std::fprintf(stderr, "FAILED: never reached an idle C:\\> prompt.\nLast screen:\n%s", screen.c_str());
        return 1;
    }
    std::fprintf(stderr, "booted to C:\\> at cycle %llu\n", (unsigned long long)m.total_cycles());

    // --- cd \games\boom --------------------------------------------------
    SendString(m, "cd \\games\\boom");
    SendKey(m, kScanEnter);
    m.run_cycles(200'000'000);
    screen = ScreenText(m);
    if (!Contains(screen, "BOOM>")) {
        std::fprintf(stderr, "FAILED: no C:\\GAMES\\BOOM> prompt after cd.\nScreen:\n%s", screen.c_str());
        return 1;
    }
    std::fprintf(stderr, "at C:\\GAMES\\BOOM> (cycle %llu)\n", (unsigned long long)m.total_cycles());

    // --- run BOOM --------------------------------------------------------
    g_diag.trace_on = want_trace;
    for (uint32_t i = 0; i < 1024; ++i) g_diag.ivt_at_start[i] = m.chipset.mem_read(i);
    uint64_t start = m.total_cycles();
    SendString(m, "boom");
    SendKey(m, kScanEnter);

    RenderedFrame frame;
    std::set<uint64_t> frame_hashes;
    int bmp_index = 0;
    uint64_t last_hash = 0;
    int distinct_run = 0;               // consecutive differing mode-13h frames
    int best_distinct_run = 0;
    bool saw_graphics = false;
    std::string worst_screen;
    // Report the guest's own output as a transcript, plus a periodic
    // CS:EIP hot-address histogram -- "where is it spinning?" answered by
    // sampling rather than by guessing (PC486_REVIEW.md §5.9).
    std::string last_logged;
    uint64_t next_report = 1'000'000'000;
    const uint64_t kSample = 2'000'000;  // ~30ms of emulated time
    bool render_ok = false;
    for (uint64_t used = 0; used < run_budget && !render_ok; used += kSample) {
        m.run_cycles(int64_t(kSample));
        PollSound(m);
        if (g_diag.watch_fired) {
            std::fprintf(stderr, "FAILED: watchpoint fired (see the ring dump above)\n");
            return 4;
        }
        if (g_diag.runaway_dumped) {
            std::fprintf(stderr, "FAILED: guest control flow ran away into zeroed memory "
                                 "(see the ring dump above)\n=== screen ===\n%s",
                         ScreenText(m).c_str());
            return 3;
        }
        if (dump_at && used >= dump_at) {
            // One-shot instruction dump at a chosen point in the run: what a
            // guest that is stuck rather than crashed needs, since there is
            // no fault to hang the ring dump off.
            dump_at = 0;
            std::fprintf(stderr, "\n=== ring dump at %llu cycles into BOOM ===\n",
                         (unsigned long long)used);
            DumpRing(m, 300);
        }
        if (used >= next_report) {
            next_report += 1'000'000'000;
            std::fprintf(stderr, "[%6llu Mcyc] mode=%d cs:eip=%04X:%08X cpl=%d pg=%d  hot:",
                         (unsigned long long)(m.total_cycles() / 1'000'000),
                         int(DetectScreenMode(m.chipset.vga)), m.cpu.cs, m.cpu.eip,
                         m.cpu.cpl(), m.cpu.paging_enabled() ? 1 : 0);
            std::vector<std::pair<uint64_t, uint64_t>> top(g_diag.hot.begin(), g_diag.hot.end());
            std::sort(top.begin(), top.end(),
                      [](auto &a, auto &b) { return a.second > b.second; });
            for (std::size_t i = 0; i < top.size() && i < 6; ++i)
                std::fprintf(stderr, " %04X:%08X=%llu", unsigned(top[i].first >> 32),
                             unsigned(top[i].first & 0xFFFFFFFFu), (unsigned long long)top[i].second);
            std::fprintf(stderr, "\n");
            g_diag.hot.clear();
            for (auto &kv : g_diag.unimpl)
                std::fprintf(stderr, "   unimplemented opcode %04X at %04X:%08X x%llu\n",
                             kv.first.second, unsigned(kv.first.first >> 32),
                             unsigned(kv.first.first & 0xFFFFFFFFu), (unsigned long long)kv.second);
        }
        ScreenMode mode = DetectScreenMode(m.chipset.vga);
        if (mode == ScreenMode::kText) {
            screen = ScreenText(m);
            if (screen != last_logged) {
                last_logged = screen;
                // Only the tail matters for a scrolling transcript, and
                // printing the whole screen every change is unreadable.
                std::size_t nl = screen.find_last_not_of('\n');
                std::string trimmed = nl == std::string::npos ? screen : screen.substr(0, nl + 1);
                std::size_t cut = trimmed.rfind('\n');
                std::fprintf(stderr, "[%6llu Mcyc] > %s\n",
                             (unsigned long long)(m.total_cycles() / 1'000'000),
                             cut == std::string::npos ? trimmed.c_str() : trimmed.c_str() + cut + 1);
            }
            for (const char *marker : kFailureMarkers) {
                if (Contains(screen, marker)) {
                    std::fprintf(stderr,
                                 "\nFAILED: guest reported a failure (%s) at cycle %llu "
                                 "(%llu cycles into BOOM)\n=== screen ===\n%s",
                                 marker, (unsigned long long)m.total_cycles(),
                                 (unsigned long long)(m.total_cycles() - start), screen.c_str());
                    std::fprintf(stderr, "=== fault summary ===\n");
                    for (auto &kv : g_diag.fault_counts)
                        std::fprintf(stderr, "  vector %2d: %llu\n", kv.first, (unsigned long long)kv.second);
                    std::fprintf(stderr, "=== #PF by page (top) ===\n");
                    int shown = 0;
                    for (auto &kv : g_diag.pf_by_cr2) {
                        std::fprintf(stderr, "  cr2 page %08X: %llu\n", kv.first.second,
                                     (unsigned long long)kv.second);
                        if (++shown > 40) break;
                    }
                    std::fprintf(stderr, "=== first 60 faults ===\n");
                    for (std::size_t i = 0; i < g_diag.faults.size() && i < 60; ++i) {
                        const auto &f = g_diag.faults[i];
                        std::fprintf(stderr,
                                     "  #%zu vec=%2d err=%04X %04X:%08X cr2=%08X cr3=%08X cpl=%d cyc=%llu\n",
                                     i, f.vector, f.error, f.cs, f.eip, f.cr2, f.cr3, f.cpl,
                                     (unsigned long long)f.cycle);
                    }
                    if (!bmp_prefix.empty()) {
                        RenderScreen(m.chipset.vga, frame, true);
                        WriteBmp(bmp_prefix + "-crash.bmp", frame);
                    }
                    return 1;
                }
            }
            worst_screen = screen;
            continue;
        }
        if (mode != ScreenMode::kVga256) continue;
        saw_graphics = true;
        RenderScreen(m.chipset.vga, frame, true);
        uint64_t h = HashFrame(frame);
        if (h != last_hash) {
            last_hash = h;
            frame_hashes.insert(h);
            if (DistinctColors(frame) > 8) {
                ++distinct_run;
                if (distinct_run > best_distinct_run) best_distinct_run = distinct_run;
            }
            if (!bmp_prefix.empty() && bmp_index < 60 && (frame_hashes.size() % 8) == 0) {
                char name[512];
                std::snprintf(name, sizeof name, "%s-%03d.bmp", bmp_prefix.c_str(), bmp_index++);
                WriteBmp(name, frame);
            }
        }
        // 24 successive *different* multi-color 320x200x256 frames is the
        // renderer running, not a loaded title bitmap sitting still.
        if (best_distinct_run >= 24) {
            std::fprintf(stderr,
                         "\nOK: BOOM is rendering -- %d consecutive differing 320x200x256 frames, "
                         "%zu distinct frames total, at cycle %llu (%llu cycles into BOOM)\n",
                         best_distinct_run, frame_hashes.size(), (unsigned long long)m.total_cycles(),
                         (unsigned long long)(m.total_cycles() - start));
            std::fprintf(stderr, "faults during the run:");
            for (auto &kv : g_diag.fault_counts)
                std::fprintf(stderr, " v%d=%llu", kv.first, (unsigned long long)kv.second);
            std::fprintf(stderr, "\n");
            if (!bmp_prefix.empty()) {
                RenderScreen(m.chipset.vga, frame, true);
                WriteBmp(bmp_prefix + "-gameplay.bmp", frame);
            }
            render_ok = true;
        }
    }
    if (render_ok) {
        std::fprintf(stderr, "=== BOOM's own startup output ===\n%s", worst_screen.c_str());
        int rc = 0;
        if (!CheckMouse(m, bmp_prefix)) rc = 6;
        if (!CheckSound(m)) rc = 5;
        return rc;
    }

    std::fprintf(stderr,
                 "FAILED: BOOM did not reach a live renderer within %llu cycles "
                 "(graphics seen=%d, distinct frames=%zu, best differing run=%d)\n",
                 (unsigned long long)run_budget, saw_graphics ? 1 : 0, frame_hashes.size(),
                 best_distinct_run);
    std::fprintf(stderr, "=== last text screen ===\n%s", worst_screen.c_str());
    std::fprintf(stderr, "=== fault summary ===\n");
    for (auto &kv : g_diag.fault_counts)
        std::fprintf(stderr, "  vector %2d: %llu\n", kv.first, (unsigned long long)kv.second);
    std::fprintf(stderr, "=== first 60 faults ===\n");
    for (std::size_t i = 0; i < g_diag.faults.size() && i < 60; ++i) {
        const auto &f = g_diag.faults[i];
        std::fprintf(stderr, "  #%zu vec=%2d err=%04X %04X:%08X cr2=%08X cr3=%08X cpl=%d cyc=%llu\n",
                     i, f.vector, f.error, f.cs, f.eip, f.cr2, f.cr3, f.cpl,
                     (unsigned long long)f.cycle);
    }
    for (auto &kv : g_diag.unimpl)
        std::fprintf(stderr, "  unimplemented opcode %04X at %04X:%08X x%llu\n",
                     kv.first.second, unsigned(kv.first.first >> 32),
                     unsigned(kv.first.first & 0xFFFFFFFFu), (unsigned long long)kv.second);
    return 1;
}
