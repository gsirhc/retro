// The generated self-test ROM as a RomSet.

#ifndef ASTEROIDS_HWTEST_SET_H
#define ASTEROIDS_HWTEST_SET_H

#include "hwtest_roms.h"
#include "machine.h"

#include <algorithm>

namespace asteroids {

inline RomSet hwtest_set() {
    asteroids::RomSet s;
    std::copy(asteroids::hwtest::program.begin(), asteroids::hwtest::program.end(),
              s.program.begin());
    std::copy(asteroids::hwtest::vector.begin(), asteroids::hwtest::vector.end(),
              s.vector.begin());
    return s;
}

}  // namespace asteroids

#endif
