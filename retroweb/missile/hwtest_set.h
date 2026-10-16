// The generated self-test ROM as a RomSet.

#ifndef MISSILE_HWTEST_SET_H
#define MISSILE_HWTEST_SET_H

#include "hwtest_roms.h"
#include "machine.h"

#include <algorithm>

namespace missile {

inline RomSet hwtest_set() {
    missile::RomSet s;
    std::copy(missile::hwtest::program.begin(), missile::hwtest::program.end(),
              s.program.begin());
    return s;
}

}  // namespace missile

#endif
