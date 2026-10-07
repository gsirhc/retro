// Native OPL3 trace player: replays a `?fmtrace` capture (web/app.js
// window.__fm) through pc486::Opl3 to a WAV, with no browser. Answers whether
// bad FM music comes from the chip emulation or the front end's audio path.
//
// Usage: fm_trace_render <trace.json> <out.wav> [--rate N]

#include "opl3.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using pc486::Opl3;

namespace {

std::string ReadFile(const char *path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Scanner for the fixed `[{"cycle":N,"reg":N,"value":N}, ...]` shape. No JSON
// library. Stops at anything unrecognized, so a truncated trace loads up to the cut.

std::size_t SkipWs(const std::string &s, std::size_t i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return i;
}

bool MatchLiteral(const std::string &s, std::size_t &i, const char *lit) {
    std::size_t len = std::strlen(lit);
    if (s.compare(i, len, lit) != 0) return false;
    i += len;
    return true;
}

bool ParseUint(const std::string &s, std::size_t &i, uint64_t &out) {
    std::size_t start = i;
    out = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        out = out * 10 + uint64_t(s[i] - '0');
        ++i;
    }
    return i != start;
}

// One record. `i` advances only on success.
bool ParseRecord(const std::string &s, std::size_t &i, Opl3::TraceEvent &ev) {
    std::size_t p = SkipWs(s, i);
    uint64_t cycle, reg, value;
    if (p >= s.size() || s[p] != '{') return false;
    ++p;
    p = SkipWs(s, p);
    if (!MatchLiteral(s, p, "\"cycle\"")) return false;
    p = SkipWs(s, p);
    if (p >= s.size() || s[p] != ':') return false;
    p = SkipWs(s, p + 1);
    if (!ParseUint(s, p, cycle)) return false;
    p = SkipWs(s, p);
    if (p >= s.size() || s[p] != ',') return false;
    p = SkipWs(s, p + 1);
    if (!MatchLiteral(s, p, "\"reg\"")) return false;
    p = SkipWs(s, p);
    if (p >= s.size() || s[p] != ':') return false;
    p = SkipWs(s, p + 1);
    if (!ParseUint(s, p, reg)) return false;
    p = SkipWs(s, p);
    if (p >= s.size() || s[p] != ',') return false;
    p = SkipWs(s, p + 1);
    if (!MatchLiteral(s, p, "\"value\"")) return false;
    p = SkipWs(s, p);
    if (p >= s.size() || s[p] != ':') return false;
    p = SkipWs(s, p + 1);
    if (!ParseUint(s, p, value)) return false;
    p = SkipWs(s, p);
    if (p >= s.size() || s[p] != '}') return false;
    ++p;
    ev = {cycle, uint16_t(reg), uint8_t(value)};
    i = p;
    return true;
}

// True if the array closed with `]`; false on an unparseable trailing record.
// `out` holds every record read either way.
bool ParseTrace(const std::string &json, std::vector<Opl3::TraceEvent> &out) {
    std::size_t i = SkipWs(json, 0);
    if (i >= json.size() || json[i] != '[') return false;
    i = SkipWs(json, i + 1);
    while (i < json.size() && json[i] != ']') {
        Opl3::TraceEvent ev;
        if (!ParseRecord(json, i, ev)) return false;
        out.push_back(ev);
        i = SkipWs(json, i);
        if (i < json.size() && json[i] == ',') i = SkipWs(json, i + 1);
    }
    return i < json.size() && json[i] == ']';
}

// --- WAV output ------------------------------------------------------
// 44-byte PCM header written by hand (cf. render_screen.cpp WriteBmp)
void WriteWav(const char *path, uint32_t sample_rate, const std::vector<int16_t> &interleaved) {
    uint32_t data_size = uint32_t(interleaved.size() * sizeof(int16_t));
    uint32_t byte_rate = sample_rate * 2 /* channels */ * 2 /* bytes/sample */;
    std::ofstream f(path, std::ios::binary);
    auto w32 = [&](uint32_t v) { f.put(char(v)); f.put(char(v >> 8)); f.put(char(v >> 16)); f.put(char(v >> 24)); };
    auto w16 = [&](uint16_t v) { f.put(char(v)); f.put(char(v >> 8)); };
    f.write("RIFF", 4); w32(36 + data_size); f.write("WAVE", 4);
    f.write("fmt ", 4); w32(16); w16(1) /* PCM */; w16(2) /* stereo */;
    w32(sample_rate); w32(byte_rate); w16(4) /* block align */; w16(16) /* bits/sample */;
    f.write("data", 4); w32(data_size);
    f.write(reinterpret_cast<const char *>(interleaved.data()), std::streamsize(data_size));
}

// Linear interpolation at fractional source positions; `src` is uniform at src_hz
std::vector<int16_t> Resample(const std::vector<Opl3::Sample> &src, double src_hz, double dst_hz) {
    std::vector<int16_t> out;
    if (src.empty()) return out;
    double duration = double(src.size()) / src_hz;
    std::size_t out_frames = std::size_t(duration * dst_hz);
    out.reserve(out_frames * 2);
    for (std::size_t n = 0; n < out_frames; ++n) {
        double pos = (double(n) / dst_hz) * src_hz;
        std::size_t i0 = std::min(std::size_t(pos), src.size() - 1);
        std::size_t i1 = std::min(i0 + 1, src.size() - 1);
        double frac = pos - double(i0);
        double l = double(src[i0].left) + frac * double(src[i1].left - src[i0].left);
        double r = double(src[i0].right) + frac * double(src[i1].right - src[i0].right);
        out.push_back(int16_t(std::lround(l)));
        out.push_back(int16_t(std::lround(r)));
    }
    return out;
}

std::vector<int16_t> Interleave(const std::vector<Opl3::Sample> &src) {
    std::vector<int16_t> out;
    out.reserve(src.size() * 2);
    for (const auto &s : src) { out.push_back(s.left); out.push_back(s.right); }
    return out;
}

// Advances the chip to target_cycle in small steps, draining after each.
// Opl3::advance() caps frames per tick() (kMaxCatchUpFrames), so one big jump
// across a long gap would drop audio.
void TickTo(Opl3 &chip, std::vector<Opl3::Sample> &samples, uint64_t &cur_cycle, uint64_t target_cycle,
            uint64_t chunk_cycles) {
    while (cur_cycle < target_cycle) {
        cur_cycle = std::min(cur_cycle + chunk_cycles, target_cycle);
        chip.tick(cur_cycle);
        auto batch = chip.drain_samples();
        samples.insert(samples.end(), batch.begin(), batch.end());
    }
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <trace.json> <out.wav> [--rate N]\n", argv[0]);
        return 2;
    }
    const char *trace_path = argv[1];
    const char *out_path = argv[2];
    bool has_rate = false;
    uint32_t out_rate = 0;
    for (int a = 3; a < argc; ++a) {
        if (std::strcmp(argv[a], "--rate") == 0 && a + 1 < argc) {
            out_rate = uint32_t(std::strtoul(argv[++a], nullptr, 10));
            has_rate = true;
        }
    }

    std::string json = ReadFile(trace_path);
    if (json.empty()) { std::fprintf(stderr, "cannot open/empty trace: %s\n", trace_path); return 2; }

    std::vector<Opl3::TraceEvent> events;
    bool clean = ParseTrace(json, events);
    std::fprintf(stderr, "read %zu trace record(s)%s\n", events.size(),
        clean ? "" : " (stopped early -- trailing record did not parse, likely a truncated capture)");
    if (events.empty()) { std::fprintf(stderr, "no usable records\n"); return 2; }

    const uint64_t first_cycle = events.front().cycle, last_cycle = events.back().cycle;
    const uint64_t span_cycles = last_cycle - first_cycle;
    const double span_sec = double(span_cycles) / Opl3::kCpuHz;

    Opl3 chip;  // reset() runs in the constructor.
    std::vector<Opl3::Sample> samples;
    std::vector<bool> reg_seen(0x200, false);
    bool bank1_seen = false, new_bit_seen = false;

    // 10ms steps, well under the per-call catch-up cap
    const uint64_t kChunkCycles = uint64_t(0.01 * Opl3::kCpuHz);
    uint64_t cur_cycle = 0;
    // Walk the chip's clock to the trace's first cycle before any write; otherwise
    // the first tick() sees a bogus multi-billion-cycle delta (prev_cycles_ starts at 0)
    TickTo(chip, samples, cur_cycle, first_cycle, kChunkCycles);

    for (const auto &ev : events) {
        TickTo(chip, samples, cur_cycle, ev.cycle, kChunkCycles);

        int bank = (ev.reg >> 8) & 1;
        uint8_t reg = uint8_t(ev.reg & 0xFF);
        chip.write_address(bank, reg);
        chip.write_data(bank, ev.value);

        reg_seen[ev.reg < 0x200 ? ev.reg : 0x1FF] = true;
        if (bank == 1) bank1_seen = true;
        if (ev.reg == 0x105) new_bit_seen = true;
    }
    const std::size_t trace_frame_count = samples.size();

    // An idle chip (nothing keyed, timers stopped) generates no frames, not silent
    // frames. Report gaps much wider than one frame period, since playback of the
    // concatenated samples compresses that time away.
    std::size_t gap_count = 0;
    double gap_seconds = 0.0;
    const double cycles_per_frame = Opl3::kCpuHz / Opl3::kSampleHz;
    for (std::size_t i = 1; i < trace_frame_count; ++i) {
        double delta = double(samples[i].cpu_cycle) - double(samples[i - 1].cpu_cycle);
        if (delta > 1.5 * cycles_per_frame) { ++gap_count; gap_seconds += delta / Opl3::kCpuHz; }
    }

    // 2s of trailing release tail
    const uint64_t kTailCycles = uint64_t(2.0 * Opl3::kCpuHz);
    TickTo(chip, samples, cur_cycle, last_cycle + kTailCycles, kChunkCycles);

    // --- report ---
    const double trace_audio_sec = double(trace_frame_count) / Opl3::kSampleHz;
    std::printf("records read:        %zu%s\n", events.size(), clean ? "" : " (truncated capture)");
    std::printf("first/last cycle:    %llu / %llu\n", (unsigned long long)first_cycle, (unsigned long long)last_cycle);
    std::printf("trace span:          %llu cycles (%.4f s at 66 MHz)\n", (unsigned long long)span_cycles, span_sec);
    std::printf("frames during trace: %zu (%.4f s at %.2f Hz)\n", trace_frame_count, trace_audio_sec, Opl3::kSampleHz);
    if (span_sec > 0.0) {
        double ratio = trace_audio_sec / span_sec;
        std::printf("duration ratio:      %.4f (audio-generated / trace-elapsed; 1.0 = chip keeps pace)\n", ratio);
        if (ratio < 0.98) {
            std::printf("  ** chip produced LESS audio than the trace's own elapsed time -- this is\n"
                        "     the signature of time-stretched/sped-up playback in the CHIP EMULATION,\n"
                        "     not the browser audio path. **\n");
        } else if (ratio > 1.02) {
            std::printf("  ** chip produced MORE audio than the trace's own elapsed time. **\n");
        } else {
            std::printf("  chip's own pacing agrees with the trace's elapsed CPU time.\n");
        }
    } else {
        std::printf("duration ratio:      n/a (zero-length trace span)\n");
    }
    if (gap_count > 0) {
        std::printf("  %zu idle gap(s) totalling %.4f s where the chip generated no frames at all\n"
                    "     (nothing keyed on, both timers stopped) -- this alone can explain a ratio\n"
                    "     below 1.0 without the chip's active-time pacing itself being wrong.\n",
            gap_count, gap_seconds);
    }
    std::printf("total frames (incl. 2s release tail): %zu (%.4f s)\n", samples.size(),
        double(samples.size()) / Opl3::kSampleHz);

    int32_t peak = 0;  // int32_t: abs(INT16_MIN) doesn't fit back in int16_t.
    std::size_t clipped = 0;
    for (const auto &s : samples) {
        peak = std::max({peak, std::abs(int32_t(s.left)), std::abs(int32_t(s.right))});
        if (s.left >= INT16_MAX || s.left <= INT16_MIN || s.right >= INT16_MAX || s.right <= INT16_MIN) ++clipped;
    }
    std::printf("peak |sample|:       %d / %d\n", peak, INT16_MAX);
    std::printf("clamped samples:     %zu\n", clipped);

    std::size_t distinct = 0;
    for (bool b : reg_seen) if (b) ++distinct;
    std::printf("distinct registers:  %zu\n", distinct);
    std::printf("bank 1 (0x100-0x1FF) written: %s\n", bank1_seen ? "yes" : "no");
    std::printf("register 0x105 (OPL3 NEW bit) written: %s\n", new_bit_seen ? "yes" : "no");
    std::printf("  ");
    std::size_t printed = 0;
    for (std::size_t r = 0; r < reg_seen.size(); ++r) {
        if (!reg_seen[r]) continue;
        if (printed == 64) { std::printf("... (%zu more)", distinct - printed); break; }
        std::printf("%03zx ", r);
        ++printed;
    }
    std::printf("\n");

    // --- WAV ---
    uint32_t rate = has_rate ? out_rate : uint32_t(std::lround(Opl3::kSampleHz));
    std::vector<int16_t> interleaved =
        has_rate ? Resample(samples, Opl3::kSampleHz, double(rate)) : Interleave(samples);
    WriteWav(out_path, rate, interleaved);
    std::printf("wrote %s: %u Hz, %zu frames (%.4f s)\n", out_path, rate, interleaved.size() / 2,
        double(interleaved.size() / 2) / double(rate));
    return 0;
}
