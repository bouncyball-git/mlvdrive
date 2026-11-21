// An MLV clip: its chunks (.MLV, .M00, .M01, ...), the metadata the DNG and WAV files need and an
// index of the video and audio frames. Reading frames is thread-safe: any number of threads may
// decode frames of one clip at once.
//
// Follows MLV-App's openMlvClip() and getMlvRawFrameUint16() (src/mlv/video_mlv.c).
#pragma once

#include "mlv/mlv_format.h"
#include "util/file.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mlv {

struct FrameIndex {
    uint64_t dataOffset = 0;  // the frame's data in its chunk
    uint32_t dataSize = 0;
    uint16_t chunk = 0;
    uint32_t frameNumber = 0;
    uint64_t timestamp = 0;   // microseconds since recording started
    uint16_t panPosX = 0, panPosY = 0;
};

enum class Compression { Uncompressed, Lossless, Jpeg2000, Unsupported };

class Clip {
public:
    // Opens the clip and its further chunks, indexing every frame and audio block. Returns null
    // with `error` set when it can't be read.
    static std::unique_ptr<Clip> open(const std::string& utf8Path, std::string* error);

    const std::string& path() const { return path_; }
    std::string name() const;           // file name without folder and extension ("M21-1642")
    int chunkCount() const { return int(chunks_.size()); }
    uint64_t totalBytes() const;

    // --- Metadata (zeroed when the clip has no such block) ---
    FileHeader mlvi{};
    RawInfoBlock rawi{};
    RawCaptureBlock rawc{};       // rawc.type[0] == 0: none
    IdentityBlock idnt{};
    ExposureBlock expo{};
    LensBlock lens{};
    std::string elnsName;         // the extended lens name, if any
    RealTimeClockBlock rtci{};
    WhiteBalanceBlock wbal{};
    WaveInfoBlock wavi{};
    std::vector<uint16_t> lineariseLut;  // CURV: 65536 entries, or empty

    bool hasRawc() const { return rawc.type[0] != 0; }
    int width() const { return rawi.xRes; }
    int height() const { return rawi.yRes; }
    int bitsPerPixel() const { return rawi.raw_info.bits_per_pixel; }
    int blackLevel() const { return rawi.raw_info.black_level; }
    int whiteLevel() const { return rawi.raw_info.white_level; }
    uint32_t cfaPattern() const { return rawi.raw_info.cfa_pattern ? uint32_t(rawi.raw_info.cfa_pattern) : 0x02010100u; }
    double fps() const { return mlvi.sourceFpsDenom ? double(mlvi.sourceFpsNom) / mlvi.sourceFpsDenom : 0.0; }
    Compression compression() const;
    std::string compressionName() const;
    // Pixel aspect (vertical stretch), from RAWC's binning and skipping: 1 for most modes, 5/3 for
    // 5x3 line skipping. num/den as DNG's DefaultScale wants it.
    void verticalStretch(int* num, int* den) const;

    // --- Frames ---
    uint32_t frameCount() const { return uint32_t(frames_.size()); }
    const FrameIndex& frame(uint32_t index) const { return frames_[index]; }
    // The frame's stored bytes (compressed or bit-packed) into `out`. Its capacity holds 8 zero
    // bytes past the data (slack for the decoders' reads).
    bool readFrameData(uint32_t index, std::vector<uint8_t>& out, std::string* error) const;
    // The frame as 16-bit values (width x height, no black level subtracted), linearised by CURV
    // if the clip has one. `scratch` is reused between calls to save allocations.
    bool decodeFrame(uint32_t index, uint16_t* out, std::vector<uint8_t>& scratch, std::string* error) const;

    // --- Audio (16-bit PCM) ---
    bool hasAudio() const {
        return mlvi.audioClass != 0 && !audio_.empty() && wavi.channels != 0 && wavi.bitsPerSample == 16;
    }
    // The audio aligned with the first video frame (MLV-App's readMlvAudioData()): silence added
    // in front, or audio before the first frame dropped. Empty if there is none. Read whole: the
    // reference readSyncedAudioRange() is checked against.
    bool readSyncedAudio(std::vector<uint8_t>& pcm, std::string* error) const;
    // Its length, without reading it.
    uint64_t syncedAudioBytes() const { return audioSync().size; }
    // `n` bytes of it from `offset` (zeros past its end), read from just the blocks they come
    // from. Thread-safe.
    bool readSyncedAudioRange(uint64_t offset, size_t n, uint8_t* out, std::string* error) const;

private:
    // How the recorded audio lines up with the first frame: the recorded bytes, those dropped
    // from the start, the silence added in front, the result's length.
    struct AudioSync {
        uint64_t total = 0, drop = 0, pad = 0, size = 0;
    };
    AudioSync audioSync() const;
    const std::vector<uint64_t>& audioStarts() const;  // where each audio block starts in the recorded audio, + the total
    mutable std::once_flag audioStartsOnce_;
    mutable std::vector<uint64_t> audioStarts_;

    Clip() = default;
    bool load(std::string* error);
    bool scanChunk(int chunk, std::string* error);
    // A metadata block (not VIDF / AUDF) at `start` of a chunk: into the fields above.
    void parseMetadata(const util::RandomAccessFile& file, uint64_t start, const BlockHeader& block);

    std::string path_;
    std::vector<std::unique_ptr<util::RandomAccessFile>> chunks_;
    std::vector<FrameIndex> frames_;
    struct AudioIndex {
        uint64_t dataOffset;
        uint32_t dataSize;
        uint16_t chunk;
        uint64_t timestamp;
    };
    std::vector<AudioIndex> audio_;
};

// Unpacks MLV's bit-packed raw data (10, 12, 14 or 16 bits: a big-endian bit stream in
// little-endian 16-bit words) into 16-bit values. `packed` must have 4 readable bytes beyond
// the data.
void unpackBits(const uint8_t* packed, uint16_t* out, size_t pixels, int bitsPerPixel);

}  // namespace mlv
