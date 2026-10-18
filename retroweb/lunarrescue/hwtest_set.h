// The generated self-test ROM and colour PROM as a RomSet.

#ifndef LUNARRESCUE_HWTEST_SET_H
#define LUNARRESCUE_HWTEST_SET_H

#include "hwtest_roms.h"
#include "machine.h"

namespace lunarrescue {

inline RomSet hwtest_set() {
    RomSet s;
    s.program = hwtest::program;
    s.program_hi = hwtest::program_hi;
    s.prom = hwtest::prom;
    return s;
}

}  // namespace lunarrescue

#endif
