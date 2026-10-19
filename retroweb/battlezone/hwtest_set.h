// The generated self-test ROM as a RomSet.

#ifndef BATTLEZONE_HWTEST_SET_H
#define BATTLEZONE_HWTEST_SET_H

#include "hwtest_roms.h"
#include "machine.h"

#include <algorithm>

namespace battlezone {

inline RomSet hwtest_set() {
    RomSet s;
    std::copy(hwtest::program.begin(), hwtest::program.end(), s.program.begin());
    std::copy(hwtest::vector.begin(), hwtest::vector.end(), s.vector.begin());
    std::copy(hwtest::mb_map.begin(), hwtest::mb_map.end(), s.mb_map.begin());
    const std::array<uint8_t, 256>* u[6] = {&hwtest::mb_ucode0, &hwtest::mb_ucode1, &hwtest::mb_ucode2,
                                            &hwtest::mb_ucode3, &hwtest::mb_ucode4, &hwtest::mb_ucode5};
    for (size_t i = 0; i < 6; i++) s.mb_ucode[i] = *u[i];
    return s;
}

}  // namespace battlezone

#endif
