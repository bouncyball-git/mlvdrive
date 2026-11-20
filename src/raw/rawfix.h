// Raw corrections applied to the Bayer data before it is shown or written to DNG: vertical
// stripes (5D Mark III), focus pixels (EOS M, 100D, 650D, 700D), bad pixels and chroma
// smoothing. MLV-App's applyLLRawProcObject() (src/mlv/llrawproc/llrawproc.c), the parts that
// don't need dual ISO or dark frames.
#pragma once

#include "llrawproc/pixelproc.h"  // third_party/mlvapp

#include <atomic>
#include <cstdint>
#include <mutex>

namespace mlv {
class Clip;
}

namespace raw {

struct FixSettings {
    bool enabled = true;
    int verticalStripes = -1;  // -1 auto (on for the 5D Mark III), 0 off, 1 on, 2 forced (every frame measured)
    int focusPixels = -1;      // -1 auto (on for cameras with focus pixels), 0 off, 1 on, 2 on with crop_rec map
    int focusMethod = 0;       // interpolation: 0 MLVFS, 1 raw2dng, 2 rewind
    int badPixels = 0;         // 0 off, 1 auto, 2 forced
    int badPixelSearch = 0;    // 0 normal, 1 aggressive
    int badPixelMethod = 0;    // interpolation: 0 MLVFS, 1 raw2dng, 2 rewind
    int chromaSmooth = 0;      // 0 off, 2 (2x2), 3 (3x3), 5 (5x5)

    bool operator==(const FixSettings& o) const {
        return enabled == o.enabled && verticalStripes == o.verticalStripes && focusPixels == o.focusPixels &&
               focusMethod == o.focusMethod && badPixels == o.badPixels && badPixelSearch == o.badPixelSearch &&
               badPixelMethod == o.badPixelMethod && chromaSmooth == o.chromaSmooth;
    }
    bool operator!=(const FixSettings& o) const { return !(*this == o); }
};

// For checking: a frame's vertical stripe factors (8 columns, 1 = none), counted as the fixer does
// (16 bands of rows on several threads) or in one pass on one thread as it used to be.
void stripeFactors(const mlv::Clip& clip, const uint16_t* image, bool onePass, double out[8]);

// Whether a camera has focus pixels (and the auto setting turns their fix on).
bool cameraHasFocusPixels(uint32_t cameraModel);

// One per clip and settings. apply() may run on several threads at once: the first call works
// out what the clip needs (stripe factors, pixel maps) under a lock, the rest only read that.
class Fixer {
public:
    Fixer(const mlv::Clip& clip, const FixSettings& settings);
    ~Fixer();
    Fixer(const Fixer&) = delete;
    Fixer& operator=(const Fixer&) = delete;

    const FixSettings& settings() const { return settings_; }
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
    pixel_map focusMap_{};
    pixel_map badMap_{};
    int focusStatus_ = 0, badStatus_ = 0;
    bool stripesNeeded_ = false;
    int stripeCoefficients_[8] = {};

    std::mutex mutex_;
    std::atomic<bool> primed_{false};
};

}  // namespace raw
