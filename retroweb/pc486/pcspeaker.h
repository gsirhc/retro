// PC speaker: port 0x61 bit 1 (Speaker Data Enable) ANDed with PIT channel 2's
// output (see pit8253.h). That one gate serves both tone generation (PIT gated on,
// speaker follows the square wave) and direct-toggle sample playback (PIT parked
// high, software toggles bit 1).
// No audio is synthesized here. Each change of the gate output is recorded as
// (cpu_cycle, new_level), the edge list a Web Audio renderer resamples.
#ifndef PC486_PCSPEAKER_H
#define PC486_PCSPEAKER_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace pc486 {

class PcSpeaker {
public:
    struct Edge {
        uint64_t cpu_cycle;
        bool level;
    };

    void reset();

    // Recomputes the AND gate from its two inputs and appends an edge on change.
    // Chipset calls it per instruction, inline since it is a no-op on a silent
    // machine (PC486_REVIEW.md §8).
    void update(uint64_t cpu_cycle, bool speaker_data_enable, bool pit_channel2_output) {
        bool new_level = speaker_data_enable && pit_channel2_output;
        if (new_level == level_) return;
        level_ = new_level;
        if (edges_.size() >= kMaxEdges) edges_.pop_front();  // drop oldest -- see below
        edges_.push_back({cpu_cycle, level_});
    }

    bool level() const { return level_; }

    // Hands the edges to the consumer and clears the log
    std::vector<Edge> drain_edges();

private:
    bool level_ = false;
    std::deque<Edge> edges_;
    // Bounds memory if a consumer stops draining; drops the oldest edges.
    static constexpr std::size_t kMaxEdges = 1 << 16;
};

}  // namespace pc486

#endif  // PC486_PCSPEAKER_H
