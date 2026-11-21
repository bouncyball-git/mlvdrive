// CinemaDNG frames from an MLV clip: the tags MLV-App writes (src/dng/dng.c), with uncompressed
// (16-bit, or bit-packed big-endian), lossless JPEG (LJ92) or the clip's own data passed through.
//
// For the drive every file's size must be known before its frame is made: the header has the same
// size for every frame of a clip, and so has the image in the uncompressed outputs.
#pragma once

#include "mlv/color.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mlv {
class Clip;
}
namespace raw {
class Fixer;
}

namespace dng {

enum class Output {
    Uncompressed16,  // 16-bit little-endian values: no packing, the fastest to make (fixed size)
    Uncompressed,    // 16-bit values packed to the clip's bit depth, big-endian (fixed size)
    Lossless,        // lossless JPEG, compressed here (after the raw fixes); the size varies
    PassThrough,     // the clip's data as recorded: no raw fixes; the size varies
};

// The TimeCodes tag: the frame number from the start of recording (00:00:00:00), or the time of
// day the recording started (RTCI) plus the frame number, counted at the rounded frame rate
// (24 for 23.976, non-drop) as MLV-App does. Without RTCI, time of day falls back to the count.
enum class Timecode { FrameCount, TimeOfDay };

struct Options {
    Output output = Output::Uncompressed16;
    double fps = 0;                 // > 0: overrides the clip's frame rate (FrameRate tag, timecode)
    int stretchNum = 0;             // DefaultScale (vertical); 0: from the clip (binning/skipping)
    int stretchDen = 0;
    int hStretchNum = 1;            // DefaultScale (horizontal): anamorphic lenses
    int hStretchDen = 1;
    bool customWhiteBalance = false;
    color::Vec3 wbMultipliers{1, 1, 1};  // with customWhiteBalance: AsShotNeutral = 1 / these
    Timecode timecode = Timecode::TimeOfDay;
};

class Writer {
public:
    // `fixer` may be null (no raw fixes). Pass-through ignores it.
    Writer(const mlv::Clip& clip, const Options& options, raw::Fixer* fixer);

    // Buffers one thread reuses from frame to frame.
    struct Scratch {
        std::vector<uint8_t> data;
        std::vector<uint16_t> pixels;
        std::vector<uint8_t> image;
        std::vector<uint8_t> header;
    };

    // The same for every frame of the clip. imageSize() is 0 for the outputs whose size varies
    // (Lossless, PassThrough); fileSize() then too.
    uint64_t headerSize() const { return headerSize_; }
    uint64_t imageSize() const { return imageSize_; }
    uint64_t fileSize() const { return imageSize_ ? headerSize_ + imageSize_ : 0; }

    // The whole DNG file of frame `frameIndex` into `out` (resized). Thread-safe (one Scratch per
    // thread).
    bool renderFrame(uint32_t frameIndex, std::vector<uint8_t>& out, Scratch& scratch, std::string* error) const;
    // Only the header of frame `frameIndex` (headerSize() bytes), without making the image: for
    // reads of a file's start (apps reading metadata). Fixed-size outputs only (false otherwise).
    bool renderHeader(uint32_t frameIndex, std::vector<uint8_t>& out) const;

    // Writes frame `frameIndex` of the clip to `path`. Thread-safe (one Scratch per thread).
    bool writeFrame(uint32_t frameIndex, const std::string& path, Scratch& scratch, uint64_t* bytesWritten,
                    std::string* error) const;

    // Whether the output really is pass-through (a clip with a linearisation curve can't be).
    Output effectiveOutput() const { return output_; }

    // The FrameRate tag (the option, else the clip's), and the frame count the TimeCodes tag of
    // a frame holds (its timecode in frames at the rounded rate): for audio to line up with.
    double frameRate() const;
    int64_t timecodeFrame(uint32_t frameIndex) const;

private:
    // The image of a frame (not for Uncompressed16, which renderFrame() decodes in place).
    bool encodeImage(uint32_t frameIndex, Scratch& s, const uint8_t** image, size_t* imageBytes, bool* compressed,
                     std::string* error) const;
    void buildHeader(uint32_t frameIndex, size_t imageBytes, bool compressed, int bitsPerSample,
                     std::vector<uint8_t>& out) const;
    void frameRateRational(int32_t out[2]) const;

    const mlv::Clip& clip_;
    Options options_;
    raw::Fixer* fixer_;
    Output output_;
    uint64_t headerSize_ = 0;
    uint64_t imageSize_ = 0;
};

// A DNG header for a generated image (the test patterns), not from a clip: 16-bit, uncompressed,
// RGGB, its colour stated as linear Rec.709 (camera RGB = linear sRGB primaries, D65 white,
// AsShotNeutral 1 1 1). The image follows the header, which ends on 16 bytes. Every field that
// changes from frame to frame has a fixed size: the header's size is the same for all frames.
struct GeneratedFrame {
    int width = 0, height = 0;
    int black = 0, white = 65535;
    int fpsNum = 25, fpsDen = 1;
    int64_t timecodeFrame = 0;  // counted at round(fps)
    int64_t dateTime = -1;      // seconds since 1970, local time (-1: none)
    std::string make, model, reelName;
};
std::vector<uint8_t> generatedHeader(const GeneratedFrame& frame);

// Packs 16-bit values into a big-endian (most significant bit first) stream of `bits` per
// value, each row starting on a byte. Returns the bytes written to `out` (resized).
size_t packBitsBigEndian(const uint16_t* in, int width, int height, int bits, std::vector<uint8_t>& out);

}  // namespace dng
