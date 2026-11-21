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
    Timecode timecode = Timecode::FrameCount;
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

private:
    // The image of a frame (not for Uncompressed16, which renderFrame() decodes in place).
    bool encodeImage(uint32_t frameIndex, Scratch& s, const uint8_t** image, size_t* imageBytes, bool* compressed,
                     std::string* error) const;
    void buildHeader(uint32_t frameIndex, size_t imageBytes, bool compressed, int bitsPerSample,
                     std::vector<uint8_t>& out) const;

    const mlv::Clip& clip_;
    Options options_;
    raw::Fixer* fixer_;
    Output output_;
    uint64_t headerSize_ = 0;
    uint64_t imageSize_ = 0;
};

// Packs 16-bit values into a big-endian (most significant bit first) stream of `bits` per
// value, each row starting on a byte. Returns the bytes written to `out` (resized).
size_t packBitsBigEndian(const uint16_t* in, int width, int height, int bits, std::vector<uint8_t>& out);

}  // namespace dng
