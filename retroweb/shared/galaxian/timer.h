// Konami sound-board divider chain read on AY port B (Stern 33A-105 sheet 3D,
// Gremlin/Sega 834-0074). The sound CPU clock is the 14.318 MHz crystal / 8.

#ifndef GALAXIAN_TIMER_H
#define GALAXIAN_TIMER_H

#include <cstdint>

namespace galaxian {

uint8_t scramble_port_b(uint64_t sound_cpu_cycles);
uint8_t frogger_port_b(uint64_t sound_cpu_cycles);

}  // namespace galaxian

#endif
