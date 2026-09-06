// IBM PC/AT PC speaker.
//
// The speaker cone is driven by a simple 2-input AND gate: Port 0x61 bit 1
// ("Speaker Data Enable") ANDed with PIT channel 2's output (already
// reflecting Port 0x61 bit 0's gate, including the real Mode 3 hardware
// behavior of forcing that output high while gated off, and reloading the
// counter on the gate's rising edge -- see pit8253.h). This one AND gate
// is *why* two completely different real programming techniques both
// work through the exact same two bits:
//   - Standard tone generation: gate the PIT on (bit 0 = 1), leave data
//     enable high (bit 1 = 1), and the speaker follows PIT channel 2's
//     square wave at whatever frequency its reload value selects.
//   - "Digitized"/direct-toggle playback (e.g. Access Software's
//     RealSound, and many disk-based PC speaker sample players): park the
//     PIT (bit 0 = 0, forcing its output permanently high) and toggle
//     bit 1 directly under CPU control -- the AND gate then just relays
//     bit 1 straight to the cone, letting software play arbitrary
//     PWM-ish waveforms sample by sample, no PIT programming involved.
//
// This device doesn't synthesize audio itself -- no browser exists yet
// (see IBM_PCAT_REVIEW.md; a Web Audio renderer is Phase 7's job). What it
// provides is a real, continuous *signal trace*: every time the AND
// gate's output actually changes level, it records (cpu_cycle,
// new_level) -- exactly the edge list a Web Audio renderer needs to
// resample into actual PCM, and the only representation faithful to a
// real speaker (an analog cone position that changes at discrete moments
// in continuous time, not a fixed-rate sample stream) rather than the
// native core committing prematurely to some particular sample rate.
#ifndef IBMPCAT_PCSPEAKER_H
#define IBMPCAT_PCSPEAKER_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace ibmpcat {

class PcSpeaker {
public:
    struct Edge {
        uint64_t cpu_cycle;
        bool level;
    };

    void reset();

    // Recomputes the AND gate's output from its two real inputs and, if it
    // actually changed, appends an edge. Chipset calls this once per
    // instruction (from its own per-instruction tick()), using the
    // current Port 0x61 Speaker Data Enable bit and Pit8253::channel2_output()
    // -- fine enough granularity to capture direct-toggle digitized
    // playback accurately, since real drivers toggle the bit across a
    // handful of instructions per sample, not faster.
    void update(uint64_t cpu_cycle, bool speaker_data_enable, bool pit_channel2_output);

    bool level() const { return level_; }

    // Hands the accumulated edges to a consumer (a future Web Audio
    // renderer) and clears the log -- draining, not peeking, since this
    // is a bounded real-time trace, not a replay buffer.
    std::vector<Edge> drain_edges();

private:
    bool level_ = false;
    std::deque<Edge> edges_;
    // Real hardware has no such limit (the cone just keeps moving); this
    // just bounds memory if a consumer stops draining. A genuinely silent
    // machine produces no edges at all, so this only bites a speaker
    // that's actively playing tones/samples with nobody listening, which
    // drops the oldest transitions the same way an unread hardware FIFO
    // would.
    static constexpr std::size_t kMaxEdges = 1 << 16;
};

}  // namespace ibmpcat

#endif  // IBMPCAT_PCSPEAKER_H
