// An MLV clip: its chunks (.MLV, .M00, .M01, ...), every block's metadata and an index of the
// video and audio frames. Reading frames is thread-safe: any number of threads may decode
// frames of one clip at once.
//
// Follows MLV-App's openMlvClip() and getMlvRawFrameUint16() (src/mlv/video_mlv.c).
#pragma once

#include "mlv/mlv_format.h"
#include "util/file.h"

#include <cstdint>
#include <functional>
#include <memory>
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
    uint64_t blockOffset = 0; // the VIDF block in its chunk
};

// Where a metadata block (any block but VIDF, AUDF, NULL, BKUP, XREF) sits, for copying it.
struct BlockRef {
    char type[4];
    uint16_t chunk = 0;
    uint64_t offset = 0;
    uint32_t size = 0;
    uint64_t timestamp = 0;
};

enum class Compression { Uncompressed, Lossless, Jpeg2000, Unsupported };

// How to open a clip (MLV-App's three modes: MLV_OPEN_FULL, MLV_OPEN_MAPP, MLV_OPEN_PREVIEW):
//   Full       indexes every frame and audio block of every chunk.
//   IndexFile  as Full, but reads the index from the clip's index file when there is a valid one
//              (instant, whatever the clip's size), and writes one after a full scan. The file is
//              NAME.MIDX beside the clip, or, where that can't be written (a locked card), in the
//              index folder (setIndexFolder). Its own format, not MLV-App's .MAPP: it also keeps
//              where every metadata block is, which MLV export copies. It is valid while every
//              chunk has the size and modification time it had.
//   Preview    stops at the first video frame: the metadata before it and that one frame, read in
//              milliseconds whatever the size (for the clips list: details and a thumbnail).
enum class OpenMode { Full, IndexFile, Preview };

class Clip {
public:
    // Opens the clip and its further chunks. Returns null with `error` set when it can't be read.
    // `progress`, if given, is called (on this thread) with how much has been read, 0..1, as the
    // chunks are scanned: about every 1%.
    static std::unique_ptr<Clip> open(const std::string& utf8Path, std::string* error,
                                      const std::function<void(double)>& progress = {},
                                      OpenMode mode = OpenMode::Full);
    bool isPreview() const { return preview_; }
    bool fromIndexFile() const { return fromIndexFile_; }  // IndexFile: read from one, not scanned
    // Where IndexFile mode keeps index files that can't be written beside their clips.
    static void setIndexFolder(const std::string& utf8Folder);
    // The frames: those indexed, or for a preview the count the camera wrote in the MLVI header
    // (0 if it didn't: a recording cut short).
    uint32_t expectedFrames() const { return preview_ ? mlvi.videoFrameCount : uint32_t(frames_.size()); }

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
    DualIsoBlock diso{};
    StyleBlock styl{};
    std::string info;             // INFO text
    std::vector<std::string> versions;  // VERS texts
    std::vector<uint16_t> lineariseLut;  // CURV: 65536 entries, or empty

    bool hasRawc() const { return rawc.type[0] != 0; }
    bool hasDiso() const { return diso.type[0] != 0 && diso.dualMode != 0; }
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
    // The frame's stored bytes (compressed or bit-packed) into `out`.
    bool readFrameData(uint32_t index, std::vector<uint8_t>& out, std::string* error) const;
    // The frame as 16-bit values (width x height, no black level subtracted), linearised by CURV
    // if the clip has one. `scratch` is reused between calls to save allocations.
    // `linearise` false keeps the values as stored (for re-encoding a clip with a CURV block).
    bool decodeFrame(uint32_t index, uint16_t* out, std::vector<uint8_t>& scratch, std::string* error,
                     bool linearise = true) const;

    // --- Blocks as stored (for writing MLV) ---
    const std::vector<BlockRef>& metadataBlocks() const { return blocks_; }
    // Reads `size` bytes at `offset` of a chunk.
    bool readRaw(uint16_t chunk, uint64_t offset, void* out, size_t size) const;

    // --- Audio ---
    bool hasAudio() const { return mlvi.audioClass != 0 && !audio_.empty() && wavi.channels != 0; }
    // The audio as 16-bit PCM, aligned with the first video frame (MLV-App's readMlvAudioData()):
    // silence added in front, or audio before the first frame dropped. Empty if there is none.
    bool readSyncedAudio(std::vector<uint8_t>& pcm, std::string* error) const;

private:
    Clip() = default;
    bool load(std::string* error);
    bool scanChunk(int chunk, std::string* error);
    // A metadata block (not VIDF / AUDF) at `start` of a chunk: into the fields above.
    void parseMetadata(const util::RandomAccessFile& file, uint64_t start, const BlockHeader& block);
    // The index file (IndexFile mode): read (false if there is no valid one), written.
    bool loadIndex();
    void saveIndex() const;
    std::string indexFileBeside() const;
    std::string indexFileInFolder() const;

    std::string path_;
    // While opening: the progress callback, the bytes of the chunks before the one scanned, of all
    // of them, and the last value reported.
    std::function<void(double)> progress_;
    bool preview_ = false;  // opened with OpenMode::Preview: one frame indexed
    OpenMode mode_ = OpenMode::Full;
    bool fromIndexFile_ = false;
    std::vector<std::string> chunkPaths_;  // the chunks' files, as chunks_
    uint64_t scannedBefore_ = 0, scanTotal_ = 0;
    double reported_ = -1;
    std::vector<std::unique_ptr<util::RandomAccessFile>> chunks_;
    std::vector<FrameIndex> frames_;
    struct AudioIndex {
        uint64_t dataOffset;
        uint32_t dataSize;
        uint16_t chunk;
        uint64_t timestamp;
    };
    std::vector<AudioIndex> audio_;
    std::vector<BlockRef> blocks_;
};

// Unpacks MLV's bit-packed raw data (10, 12, 14 or 16 bits: a big-endian bit stream in
// little-endian 16-bit words) into 16-bit values. `packed` must have 4 readable bytes beyond
// the data.
void unpackBits(const uint8_t* packed, uint16_t* out, size_t pixels, int bitsPerPixel);

}  // namespace mlv
