// The generated self-test ROM as a RomSet.

#ifndef INVADERS_HWTEST_SET_H
#define INVADERS_HWTEST_SET_H

#include "hwtest_roms.h"
#include "machine.h"

#include <algorithm>

namespace invaders {

inline RomSet hwtest_set() {
    RomSet s;
    std::copy(hwtest::program.begin(), hwtest::program.end(), s.program.begin());
    return s;
}

}  // namespace invaders

#endif
