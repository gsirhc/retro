// Konami sound-board timer, read back on AY port B.
// 16*16*2*8*5*2 = 40960 crystal clocks (14.31818 MHz); Computer Archaeology,
// MAME konami_sound_timer_r as cross-check. Frogger's bit 3/5 swap lives on
// the Frogger board.

#ifndef GALAXIAN_TIMER_H
#define GALAXIAN_TIMER_H

#include <cstdint>

namespace galaxian {

uint8_t konami_sound_timer(uint64_t sound_cpu_cycles);

}  // namespace galaxian

#endif
