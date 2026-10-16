// The generated self-test ROM as a RomSet.

#ifndef CENTIPEDE_HWTEST_SET_H
#define CENTIPEDE_HWTEST_SET_H

#include "hwtest_roms.h"
#include "machine.h"

#include <algorithm>

namespace centipede {

inline RomSet hwtest_set() {
    centipede::RomSet s;
    std::copy(centipede::hwtest::program.begin(), centipede::hwtest::program.end(),
              s.program.begin());
    std::copy(centipede::hwtest::gfx.begin(), centipede::hwtest::gfx.end(),
              s.gfx.begin());
    return s;
}

}  // namespace centipede

#endif
