#include "timer.h"

namespace galaxian {

namespace {

struct Taps {
    bool qa;
    bool qd;
    bool qc;
    bool ripple;
};

// Crystal /256 (two 4-bit binary stages), /16 (LS93), then an LS90 bi-quinary
// stage whose BD input counts the LS93 QD falling edges: QD QC QB = 0..4, QA = QD /2.
Taps taps(uint64_t sound_cpu_cycles) {
    const uint64_t clocks = sound_cpu_cycles * 8;
    const uint32_t stage = uint32_t((clocks >> 12) % 5);
    return {((clocks / (5u << 12)) & 1) != 0, stage == 4, (stage & 2) != 0,
            ((clocks >> 11) & 1) != 0};
}

}  // namespace

// Unconnected port bits read high (GI AY-3-8910 data manual, I/O ports).
// Scramble: B7 QA, B6 QD, B5 QC, B4 LS93 QD.
uint8_t scramble_port_b(uint64_t sound_cpu_cycles) {
    const Taps t = taps(sound_cpu_cycles);
    return uint8_t(0x0F | (t.qa << 7) | (t.qd << 6) | (t.qc << 5) | (t.ripple << 4));
}

// Frogger: same chain, QC on B3 and B5 unconnected.
uint8_t frogger_port_b(uint64_t sound_cpu_cycles) {
    const Taps t = taps(sound_cpu_cycles);
    return uint8_t(0x27 | (t.qa << 7) | (t.qd << 6) | (t.qc << 3) | (t.ripple << 4));
}

}  // namespace galaxian
