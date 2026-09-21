// Frogger board aliases for shared Galaxian-family video. Defaults to
// Board::Frogger (nibble-swap, river split, PROM blue bit open).

#ifndef FROGGER_VIDEO_H
#define FROGGER_VIDEO_H

#include "galaxian/video.h"

namespace frogger {
using galaxian::kHTotal;
using galaxian::kVTotal;
using galaxian::kVisW;
using galaxian::kVisH;
using galaxian::kVisY0;
using galaxian::kUprightW;
using galaxian::kUprightH;
using galaxian::kCpuPerLine;
using galaxian::kCpuPerFrame;
using galaxian::kVBlankLine;
using galaxian::kRiverSplit;
using galaxian::kRiverBlue;
using galaxian::Video;
}

#endif
