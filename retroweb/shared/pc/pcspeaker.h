// PC/AT speaker: an AND gate of Port 0x61 bit 1 (Speaker Data Enable) and PIT
// channel 2's output (pit8253.h). Tones gate the PIT on and let it square-wave
// the cone. Direct-toggle sample playback (e.g. RealSound) parks the PIT (output
// forced high) and toggles bit 1 itself.
// No audio is synthesized here. Each change of the gate output is recorded as
// (cpu_cycle, new_level), the edge list a Web Audio renderer resamples.
#ifndef SHARED_PC_PCSPEAKER_H
#define SHARED_PC_PCSPEAKER_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace pc {

class PcSpeaker {
public:
    struct Edge {
        uint64_t cpu_cycle;
        bool level;
    };

    void reset();

    // Recomputes the gate and appends an edge on change. Chipset calls it every
    // instruction, inline since it's a no-op on a silent machine.
    void update(uint64_t cpu_cycle, bool speaker_data_enable, bool pit_channel2_output) {
        bool new_level = speaker_data_enable && pit_channel2_output;
        if (new_level == level_) return;
        level_ = new_level;
        if (edges_.size() >= kMaxEdges) edges_.pop_front();  // drop oldest
        edges_.push_back({cpu_cycle, level_});
    }

    bool level() const { return level_; }

    // Hands the accumulated edges to the consumer and clears the log.
    std::vector<Edge> drain_edges();

private:
    bool level_ = false;
    std::deque<Edge> edges_;
    // Bounds memory if a consumer stops draining; drops the oldest edges.
    static constexpr std::size_t kMaxEdges = 1 << 16;
};

}  // namespace pc

#endif  // SHARED_PC_PCSPEAKER_H
