// The generated self-test ROM as a RomSet.

#ifndef FROGGER_HWTEST_SET_H
#define FROGGER_HWTEST_SET_H

#include "hwtest_roms.h"
#include "machine.h"

#include <algorithm>

namespace frogger {

inline RomSet hwtest_set() {
    frogger::RomSet s;
    std::copy(frogger::hwtest::program.begin(), frogger::hwtest::program.end(), s.program.begin());
    std::copy(frogger::hwtest::sound.begin(), frogger::hwtest::sound.end(), s.sound.begin());
    std::copy(frogger::hwtest::gfx.begin(), frogger::hwtest::gfx.end(), s.gfx.begin());
    std::copy(frogger::hwtest::color_prom.begin(), frogger::hwtest::color_prom.end(),
              s.color_prom.begin());
    return s;
}

}  // namespace frogger

#endif
