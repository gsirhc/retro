#include "pcspeaker.h"

namespace pc486 {

void PcSpeaker::reset() {
    level_ = false;
    edges_.clear();
}

std::vector<PcSpeaker::Edge> PcSpeaker::drain_edges() {
    std::vector<Edge> out(edges_.begin(), edges_.end());
    edges_.clear();
    return out;
}

}  // namespace pc486
