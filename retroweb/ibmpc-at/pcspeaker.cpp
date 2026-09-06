#include "pcspeaker.h"

namespace ibmpcat {

void PcSpeaker::reset() {
    level_ = false;
    edges_.clear();
}

void PcSpeaker::update(uint64_t cpu_cycle, bool speaker_data_enable, bool pit_channel2_output) {
    bool new_level = speaker_data_enable && pit_channel2_output;
    if (new_level == level_) return;
    level_ = new_level;
    if (edges_.size() >= kMaxEdges) edges_.pop_front();  // drop oldest -- see header
    edges_.push_back({cpu_cycle, level_});
}

std::vector<PcSpeaker::Edge> PcSpeaker::drain_edges() {
    std::vector<Edge> out(edges_.begin(), edges_.end());
    edges_.clear();
    return out;
}

}  // namespace ibmpcat
