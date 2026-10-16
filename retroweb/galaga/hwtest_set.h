// The generated self-test ROM as a RomSet.

#ifndef GALAGA_HWTEST_SET_H
#define GALAGA_HWTEST_SET_H

#include "hwtest_roms.h"
#include "machine.h"

#include <algorithm>

namespace galaga {

inline RomSet hwtest_set() {
    galaga::RomSet s;
    auto copy = [](const auto& src, auto& dst) {
        std::copy(src.begin(), src.end(), dst.begin());
    };
    copy(galaga::hwtest::main, s.main);
    copy(galaga::hwtest::sub, s.sub);
    copy(galaga::hwtest::sound, s.sound);
    copy(galaga::hwtest::tiles, s.tiles);
    copy(galaga::hwtest::sprites, s.sprites);
    copy(galaga::hwtest::palette, s.palette);
    copy(galaga::hwtest::char_lut, s.char_lut);
    copy(galaga::hwtest::sprite_lut, s.sprite_lut);
    copy(galaga::hwtest::wave, s.wave);
    return s;
}

}  // namespace galaga

#endif
