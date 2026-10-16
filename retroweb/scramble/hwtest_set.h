// The generated self-test ROM as a RomSet.

#ifndef SCRAMBLE_HWTEST_SET_H
#define SCRAMBLE_HWTEST_SET_H

#include "hwtest_roms.h"
#include "machine.h"

#include <algorithm>

namespace scramble {

inline RomSet hwtest_set() {
    scramble::RomSet s;
    std::copy(scramble::hwtest::program.begin(), scramble::hwtest::program.end(), s.program.begin());
    std::copy(scramble::hwtest::sound.begin(), scramble::hwtest::sound.end(), s.sound.begin());
    std::copy(scramble::hwtest::gfx.begin(), scramble::hwtest::gfx.end(), s.gfx.begin());
    std::copy(scramble::hwtest::color_prom.begin(), scramble::hwtest::color_prom.end(),
              s.color_prom.begin());
    return s;
}

}  // namespace scramble

#endif
