#include "timer.h"

namespace galaxian {

uint8_t konami_sound_timer(uint64_t sound_cpu_cycles) {
    constexpr uint32_t kPeriod = 16u * 16u * 2u * 8u * 5u * 2u;  // 40960
    constexpr uint32_t kHalf = 16u * 16u * 2u * 8u * 5u;         // 20480
    uint32_t clocks = uint32_t((sound_cpu_cycles * 8) % kPeriod);
    uint8_t hibit = 0;
    if (clocks >= kHalf) {
        hibit = 1;
        clocks -= kHalf;
    }
    return uint8_t((hibit << 7) | (uint8_t((clocks >> 14) & 1) << 6) |
                   (uint8_t((clocks >> 13) & 1) << 5) |
                   (uint8_t((clocks >> 11) & 1) << 4) | 0x0E);
}

}  // namespace galaxian
