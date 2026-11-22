// Raw corrections applied to the Bayer data before it is written to DNG: vertical stripes (5D
// Mark III), focus pixels (EOS M, 100D, 650D, 700D) and bad pixels. MLV-App's
// applyLLRawProcObject() (src/mlv/llrawproc/llrawproc.c), the parts that don't need dual ISO or
// dark frames, without chroma smoothing.
#pragma once

#include "llrawproc/pixelproc.h"  // third_party/mlvapp

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace mlv {
class Clip;
}

namespace raw {

struct FixSettings {
    int verticalStripes = -1;  // -1 auto (on for the 5D Mark III), 0 off, 1 on
    int focusPixels = -1;      // -1 auto (on for cameras with focus pixels, or a clip with its own .fpm), 0 off, 1 on
    // 0 off, 1 searched on frame 0, 3 from a .bpm map. (MLV-App's 2, searched on every frame,
    // isn't offered: it rewrites the clip's map for each frame, which frames made on several
    // threads at once can't share; 2 counts as 1.)
    int badPixels = 0;
    int badPixelSearch = 0;    // 0 normal, 1 aggressive
};

// The vector (SSE4.1) code where there is one, or the scalar code it must equal (for comparing).
// On by default; process-wide.
void setSimd(bool on);

// Pixel maps (focus .fpm, bad .bpm; named <camera id>_<raw width>x<raw height>, as MLV-App's) are
// looked up, for a clip: beside it as <clip name>.fpm / .bpm (a map for that clip alone), beside
// it under the generic name, then in these folders in order (UTF-8 paths). Process-wide.
void setPixelMapFolders(const std::vector<std::string>& folders);
// The usual folders: <settings folder>\pixelmaps (%APPDATA%\MLV Drive\pixelmaps), then the
// pixelmaps folder beside the program (macOS: also the app bundle's Resources/pixelmaps).
std::vector<std::string> defaultPixelMapFolders();

// One per clip and settings. apply() may run on several threads at once: the first call works
// out what the clip needs (stripe factors, pixel maps) under a lock, always from frame 0 (decoded
// for it if the first call is for another frame), the rest only read that.
class Fixer {
public:
    Fixer(const mlv::Clip& clip, const FixSettings& settings);
    ~Fixer();
    Fixer(const Fixer&) = delete;
    Fixer& operator=(const Fixer&) = delete;

    bool active() const { return active_; }  // false: apply() changes nothing
    void apply(uint16_t* image, uint32_t frameIndex);

private:
    void run(uint16_t* image, uint32_t frameIndex, bool first);

    const mlv::Clip& clip_;
    FixSettings settings_;
    int stripes_ = 0, focus_ = 0, cropRec_ = 0;
    bool active_ = false;

    int black14_ = 0, white14_ = 0;  // levels at 14 bits, where the fixes work
    int* raw2ev_ = nullptr;
    int* ev2raw_ = nullptr;
    // Filled by the first apply() (under mutex_), only read after it.
    pixel_map focusMap_{};
    pixel_map badMap_{};
    int focusStatus_ = 0, badStatus_ = 0;
    bool stripesNeeded_ = false;
    int stripeCoefficients_[8] = {};

    std::mutex mutex_;
    std::atomic<bool> primed_{false};
};

}  // namespace raw
