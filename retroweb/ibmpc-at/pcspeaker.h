// IBM PC/AT speaker: an AND gate of Port 0x61 bit 1 (Speaker Data Enable)
// and PIT channel 2's output (pit8253.h). Tones gate the PIT on and let it
// square-wave the cone. Direct-toggle sample playback (e.g. RealSound) parks
// the PIT (output forced high) and toggles bit 1 itself.
//
// No audio is synthesized here. Every output level change is recorded as
// (cpu_cycle, new_level), the edge list a Web Audio renderer resamples.
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

    // Recomputes the gate output and appends an edge on change. Chipset calls
    // this every instruction, fine enough for direct-toggle playback.
    void update(uint64_t cpu_cycle, bool speaker_data_enable, bool pit_channel2_output);

    bool level() const { return level_; }

    // Hands the accumulated edges to the consumer and clears the log.
    std::vector<Edge> drain_edges();

private:
    bool level_ = false;
    std::deque<Edge> edges_;
    // Bounds memory if nothing drains; the oldest edges are dropped.
    static constexpr std::size_t kMaxEdges = 1 << 16;
};

}  // namespace ibmpcat

#endif  // IBMPCAT_PCSPEAKER_H
