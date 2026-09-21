// Konami sound-board timer, read back on AY port B.
//
// Crystal 14.31818 MHz → ÷8 = sound Z80. The timer chain is
// 16×16×2×8×5×2 = 40960 crystal clocks (sound T-states × 8). Computer
// Archaeology / Konami sound board; MAME konami_sound_timer_r is a
// cross-check of the counter taps, not a source. Frogger's PCB swaps
// bits 3 and 5 of this generic reading — that wrapper lives on the
// Frogger board, not here.

#ifndef GALAXIAN_TIMER_H
#define GALAXIAN_TIMER_H

#include <cstdint>

namespace galaxian {

uint8_t konami_sound_timer(uint64_t sound_cpu_cycles);

}  // namespace galaxian

#endif
