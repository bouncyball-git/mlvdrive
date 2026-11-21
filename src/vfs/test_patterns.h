// Test patterns as a clip of DNG frames: the professional charts for checking a raw pipeline's
// levels, colour, geometry and resolution, each held for 5 seconds, 1920 x 1080 at 25 fps.
//
//    1 SMPTE RP 219 HD colour bars      7 Ramps: grey, red, green, blue
//    2 SMPTE colour bars (SD layout)    8 Crosshatch 16 x 9 with circle
//    3 EBU colour bars 100%             9 Framing and safe areas
//    4 EBU colour bars 75%             10 Checkerboard
//    5 PLUGE                           11 Multiburst
//    6 Grey scale, 11 steps            12 Zone plate
//
// The frames are a Bayer mosaic (RGGB) whose colour the DNG states as linear Rec.709: a pattern's
// video levels are taken back through the BT.709 curve to linear light, so a raw developer that
// applies Rec.709 gamma shows the levels the chart defines (sub-black PLUGE bars sit below the
// black level, where they belong).
#pragma once

#include "vfs/source.h"

#include <memory>

namespace vfs {

constexpr int kTestPatternCount = 12;
const char* testPatternName(int index);  // 0-based

// `tenHours`: timecode from 10:00:00:00 (a broadcast programme's start), else from 00:00:00:00.
std::unique_ptr<FrameSource> makeTestPatterns(bool tenHours);

}  // namespace vfs
