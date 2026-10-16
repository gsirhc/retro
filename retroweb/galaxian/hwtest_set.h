// The generated self-test ROM as a RomSet.

#ifndef GALAXIAN_HWTEST_SET_H
#define GALAXIAN_HWTEST_SET_H

#include "hwtest_roms.h"
#include "machine.h"

#include <algorithm>

namespace galaxian {

inline RomSet hwtest_set() {
    galaxian::RomSet s;
    std::copy(galaxian::hwtest::program.begin(), galaxian::hwtest::program.end(), s.program.begin());
    std::copy(galaxian::hwtest::gfx.begin(), galaxian::hwtest::gfx.end(), s.gfx.begin());
    std::copy(galaxian::hwtest::color_prom.begin(), galaxian::hwtest::color_prom.end(),
              s.color_prom.begin());
    return s;
}

}  // namespace galaxian

#endif
