#include "mlv/clip.h"

#include "liblj92/lj92.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>

namespace mlv {

namespace {

bool Is(const char type[4], const char* name) { return std::memcmp(type, name, 4) == 0; }

std::string FourCC(const char type[4]) {
    std::string s;
    for (int i = 0; i < 4; ++i) {
        unsigned char c = static_cast<unsigned char>(type[i]);
        s += (c >= 32 && c < 127) ? char(c) : '?';
    }
    return s;
}

// Blocks a damaged file is searched for (as MLV-App's seek_to_next_known_block, plus the headers).
bool IsKnownBlock(const char* p) {
    static const char* const kKnown[] = {"VIDF", "AUDF", "NULL", "RTCI", "EXPO", "LENS", "WBAL", "IDNT"};
    for (const char* k : kKnown) {
        if (std::memcmp(p, k, 4) == 0) return true;
    }
    return false;
}

std::string TrimmedText(const char* data, size_t size) {
    std::string s(data, size);
    size_t end = s.find('\0');
    if (end != std::string::npos) s.resize(end);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

}  // namespace

std::unique_ptr<Clip> Clip::open(const std::string& utf8Path, std::string* error) {
    std::unique_ptr<Clip> clip(new Clip());
    clip->path_ = utf8Path;
    if (!clip->load(error)) return nullptr;
    return clip;
}

std::string Clip::name() const { return util::u8(util::u8(path_).stem()); }

uint64_t Clip::totalBytes() const {
    uint64_t total = 0;
    for (const auto& c : chunks_) total += c->size();
    return total;
}

Compression Clip::compression() const {
    if (mlvi.videoClass & kVideoFlagMcraw) return Compression::Unsupported;
    if (mlvi.videoClass & (kVideoFlagLzma | kVideoFlagDelta)) return Compression::Unsupported;
    if (mlvi.videoClass & kVideoFlagJpeg2k) return Compression::Jpeg2000;
    if (mlvi.videoClass & kVideoFlagLj92) return Compression::Lossless;
    return Compression::Uncompressed;
}

std::string Clip::compressionName() const {
    switch (compression()) {
        case Compression::Uncompressed: return "Uncompressed";
        case Compression::Lossless: return "Lossless (LJ92)";
        case Compression::Jpeg2000: return "JPEG 2000";
        default: return "Unsupported";
    }
}

void Clip::verticalStretch(int* num, int* den) const {
    *num = 1;
    *den = 1;
    if (hasRawc()) {
        int sx = rawc.binning_x + rawc.skipping_x;
        int sy = rawc.binning_y + rawc.skipping_y;
        if (sx > 0 && sy > 0) {
            *num = sy;
            *den = sx;
        }
        return;
    }
    // Without RAWC (old recordings): as MLV-App's DNG export guesses it.
    const auto& a = rawi.raw_info.active_area;
    double w = a.x2 - a.x1, h = a.y2 - a.y1;
    if (h > 0 && w / h > 2.0 && h <= 720) {
        *num = 5;
        *den = 3;
    }
}

bool Clip::load(std::string* error) {
    // --- The chunks: NAME.MLV, then NAME.M00 ... NAME.M98 while they exist ---
    auto first = std::make_unique<util::RandomAccessFile>();
    if (!first->open(path_)) {
        *error = "Cannot open " + path_;
        return false;
    }
    chunks_.push_back(std::move(first));

    std::string ext = util::u8(util::u8(path_).extension());
    std::string lower = ext;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (lower == ".mlv") {
        const std::string base = path_.substr(0, path_.size() - 2);  // "NAME.M" or "NAME.m"
        for (int seq = 0; seq < 99; ++seq) {
            char digits[4];
            std::snprintf(digits, sizeof digits, "%02d", seq);
            std::string name = base + digits;
            if (!util::fileExists(name)) {
                // Case-sensitive file systems: the chunks may be named in the other case.
                std::string other = name;
                char& m = other[other.size() - 3];
                m = (m == 'M') ? 'm' : 'M';
                if (!util::fileExists(other)) break;
                name = other;
            }
            auto chunk = std::make_unique<util::RandomAccessFile>();
            if (!chunk->open(name)) break;
            chunks_.push_back(std::move(chunk));
        }
    }

    for (int i = 0; i < int(chunks_.size()); ++i) {
        if (!scanChunk(i, error)) {
            if (i == 0) return false;
            // A broken further chunk: keep what was read before it.
            chunks_.resize(size_t(i));
            break;
        }
    }

    if (frames_.empty()) {
        *error = "No video frames in " + path_;
        return false;
    }
    if (rawi.type[0] == 0 || rawi.xRes == 0 || rawi.yRes == 0) {
        *error = "No raw format information (RAWI block) in " + path_;
        return false;
    }
    const int bpp = rawi.raw_info.bits_per_pixel;
    if (bpp < 8 || bpp > 16) {
        *error = "Unsupported bit depth " + std::to_string(bpp) + " in " + path_;
        return false;
    }

    // Frames (and audio) in recording order: chunks may hold them out of order.
    std::stable_sort(frames_.begin(), frames_.end(),
                     [](const FrameIndex& a, const FrameIndex& b) { return a.timestamp < b.timestamp; });
    std::stable_sort(audio_.begin(), audio_.end(),
                     [](const AudioIndex& a, const AudioIndex& b) { return a.timestamp < b.timestamp; });
    return true;
}

// A metadata block's fields (the first of EXPO, LENS, ELNS, WBAL, RTCI, CURV; the last of RAWI,
// RAWC, WAVI, IDNT).
void Clip::parseMetadata(const util::RandomAccessFile& file, uint64_t start, const BlockHeader& block) {
    auto readBlock = [&](uint64_t at, uint32_t blockSize, void* out, size_t size) {
        std::memset(out, 0, size);
        return file.readAt(at, out, std::min<size_t>(size, blockSize));
    };
    if (Is(block.type, "RAWI")) {
        readBlock(start, block.size, &rawi, sizeof rawi);
    } else if (Is(block.type, "RAWC")) {
        readBlock(start, block.size, &rawc, sizeof rawc);
    } else if (Is(block.type, "WAVI")) {
        readBlock(start, block.size, &wavi, sizeof wavi);
    } else if (Is(block.type, "EXPO")) {
        if (expo.type[0] == 0) readBlock(start, block.size, &expo, sizeof expo);
    } else if (Is(block.type, "LENS")) {
        if (lens.type[0] == 0) {
            readBlock(start, block.size, &lens, sizeof lens);
            lens.lensName[31] = 0;
            lens.lensSerial[31] = 0;
        }
    } else if (Is(block.type, "ELNS")) {
        if (elnsName.empty() && block.size > sizeof(ExtendedLensBlock)) {
            // A lens name: not more than a few hundred bytes (a damaged size isn't allocated).
            std::vector<char> text(std::min<size_t>(block.size - sizeof(ExtendedLensBlock), 1024));
            if (file.readAt(start + sizeof(ExtendedLensBlock), text.data(), text.size()))
                elnsName = TrimmedText(text.data(), text.size());
        }
    } else if (Is(block.type, "WBAL")) {
        if (wbal.type[0] == 0) readBlock(start, block.size, &wbal, sizeof wbal);
    } else if (Is(block.type, "RTCI")) {
        if (rtci.type[0] == 0) readBlock(start, block.size, &rtci, sizeof rtci);
    } else if (Is(block.type, "IDNT")) {
        readBlock(start, block.size, &idnt, sizeof idnt);
    } else if (Is(block.type, "CURV")) {
        if (lineariseLut.empty() && block.size > sizeof(BlockHeader)) {
            size_t entries = std::min<size_t>((block.size - sizeof(BlockHeader)) / 2, 65536);
            lineariseLut.assign(65536, 0);
            if (!file.readAt(start + sizeof(BlockHeader), lineariseLut.data(), entries * 2)) lineariseLut.clear();
        }
    }
    // STYL, DISO, INFO, VERS, NULL, BKUP, MARK, ELVL, DEBG, DARK, XREF, MLVI: nothing needed.
}

bool Clip::scanChunk(int chunk, std::string* error) {
    const util::RandomAccessFile& file = *chunks_[size_t(chunk)];
    const uint64_t fileSize = file.size();

    FileHeader header{};
    if (fileSize < sizeof(FileHeader) || !file.readAt(0, &header, sizeof header) || !Is(header.magic, "MLVI")) {
        *error = "Not an MLV file (no MLVI header): chunk " + std::to_string(chunk) + " of " + path_;
        return false;
    }
    if (chunk == 0) {
        mlvi = header;
    } else if (header.fileGuid != mlvi.fileGuid) {
        *error = "Chunk " + std::to_string(chunk) + " belongs to another recording";
        return false;
    }

    // Reads a block's fixed part into `out` (zero-filled when the block is shorter).
    auto readBlock = [&](uint64_t at, uint32_t blockSize, void* out, size_t size) {
        std::memset(out, 0, size);
        return file.readAt(at, out, std::min<size_t>(size, blockSize));
    };

    uint64_t pos = 0;
    while (pos + sizeof(BlockHeader) <= fileSize) {
        BlockHeader block{};
        if (!file.readAt(pos, &block, sizeof block)) break;

        const bool sizeValid = block.size >= sizeof(BlockHeader) && pos + block.size <= fileSize;
        if (!sizeValid || !(IsKnownBlock(block.type) || Is(block.type, "MLVI") || Is(block.type, "RAWI") ||
                            Is(block.type, "RAWC") || Is(block.type, "WAVI") || Is(block.type, "ELNS") ||
                            Is(block.type, "INFO") || Is(block.type, "DISO") || Is(block.type, "MARK") ||
                            Is(block.type, "STYL") || Is(block.type, "ELVL") || Is(block.type, "DEBG") ||
                            Is(block.type, "VERS") || Is(block.type, "DARK") || Is(block.type, "CURV") ||
                            Is(block.type, "BKUP") || Is(block.type, "XREF"))) {
            if (block.size >= sizeof(BlockHeader) && pos + block.size > fileSize && IsKnownBlock(block.type)) {
                break;  // the last block was cut off (card full, camera stopped): the end
            }
            // Damaged: search the next block we know, as MLV-App does.
            const size_t window = 4 << 20;
            std::vector<uint8_t> buffer(window);
            uint64_t from = pos + 1, found = 0;
            bool hit = false;
            while (from + 4 <= fileSize && from - pos < (uint64_t(256) << 20)) {
                size_t n = size_t(std::min<uint64_t>(window, fileSize - from));
                if (!file.readAt(from, buffer.data(), n)) break;
                for (size_t i = 0; i + 4 <= n; ++i) {
                    if (IsKnownBlock(reinterpret_cast<const char*>(&buffer[i]))) {
                        found = from + i;
                        hit = true;
                        break;
                    }
                }
                if (hit || n < 4) break;
                from += n - 3;
            }
            if (!hit) {
                if (!frames_.empty()) break;  // keep the frames found so far
                *error = "Damaged file: unknown block '" + FourCC(block.type) + "' in " + path_;
                return false;
            }
            pos = found;
            continue;
        }

        const uint64_t start = pos;
        pos += block.size;

        if (Is(block.type, "VIDF")) {
            VideoFrameHeader vidf;
            if (!readBlock(start, block.size, &vidf, sizeof vidf)) break;
            if (block.size < sizeof vidf + vidf.frameSpace) continue;
            FrameIndex f;
            f.chunk = uint16_t(chunk);
            f.dataOffset = start + sizeof vidf + vidf.frameSpace;
            f.dataSize = block.size - uint32_t(sizeof vidf) - vidf.frameSpace;
            f.frameNumber = vidf.frameNumber;
            f.timestamp = vidf.timestamp;
            f.panPosX = vidf.panPosX;
            f.panPosY = vidf.panPosY;
            frames_.push_back(f);
        } else if (Is(block.type, "AUDF")) {
            AudioFrameHeader audf;
            if (!readBlock(start, block.size, &audf, sizeof audf)) break;
            if (block.size < sizeof audf + audf.frameSpace) continue;
            audio_.push_back({start + sizeof audf + audf.frameSpace,
                              block.size - uint32_t(sizeof audf) - audf.frameSpace, uint16_t(chunk), audf.timestamp});
        } else if (!Is(block.type, "NULL") && !Is(block.type, "MLVI")) {
            parseMetadata(file, start, block);
        }
    }
    return true;
}

bool Clip::readFrameData(uint32_t index, std::vector<uint8_t>& out, std::string* error) const {
    if (index >= frames_.size()) {
        if (error) *error = "Frame " + std::to_string(index) + " out of range";
        return false;
    }
    const FrameIndex& f = frames_[index];
    out.resize(size_t(f.dataSize) + 8);  // slack for the bit unpacker's reads
    std::memset(out.data() + f.dataSize, 0, 8);
    if (!chunks_[f.chunk]->readAt(f.dataOffset, out.data(), f.dataSize)) {
        if (error) *error = "Read error at frame " + std::to_string(index);
        return false;
    }
    out.resize(f.dataSize);
    return true;
}

void unpackBits(const uint8_t* packed, uint16_t* out, size_t pixels, int bpp) {
    if (bpp == 16) {
        std::memcpy(out, packed, pixels * 2);
        return;
    }
    const uint32_t mask = (1u << bpp) - 1;
    // Word k of the little-endian 16-bit words; the bit stream runs from each word's top bit.
    auto word = [packed](size_t k) -> uint32_t { return uint32_t(packed[2 * k]) | (uint32_t(packed[2 * k + 1]) << 8); };
    if (bpp == 14) {
        // 8 pixels in 7 words (raw_pixblock_14).
        size_t blocks = pixels / 8;
        for (size_t b = 0; b < blocks; ++b) {
            const uint8_t* p = packed + b * 14;
            uint32_t w0 = p[0] | p[1] << 8, w1 = p[2] | p[3] << 8, w2 = p[4] | p[5] << 8, w3 = p[6] | p[7] << 8;
            uint32_t w4 = p[8] | p[9] << 8, w5 = p[10] | p[11] << 8, w6 = p[12] | p[13] << 8;
            uint16_t* o = out + b * 8;
            o[0] = uint16_t(w0 >> 2);
            o[1] = uint16_t(((w0 & 0x3) << 12) | (w1 >> 4));
            o[2] = uint16_t(((w1 & 0xF) << 10) | (w2 >> 6));
            o[3] = uint16_t(((w2 & 0x3F) << 8) | (w3 >> 8));
            o[4] = uint16_t(((w3 & 0xFF) << 6) | (w4 >> 10));
            o[5] = uint16_t(((w4 & 0x3FF) << 4) | (w5 >> 12));
            o[6] = uint16_t(((w5 & 0xFFF) << 2) | (w6 >> 14));
            o[7] = uint16_t(w6 & 0x3FFF);
        }
        for (size_t i = blocks * 8; i < pixels; ++i) {
            size_t bit = i * 14, k = bit >> 4;
            uint32_t v = (word(k) << 16) | word(k + 1);
            out[i] = uint16_t((v >> (32 - 14 - (bit & 15))) & mask);
        }
        return;
    }
    for (size_t i = 0; i < pixels; ++i) {
        size_t bit = i * size_t(bpp), k = bit >> 4;
        uint32_t v = (word(k) << 16) | word(k + 1);
        out[i] = uint16_t((v >> (32 - bpp - (bit & 15))) & mask);
    }
}

bool Clip::decodeFrame(uint32_t index, uint16_t* out, std::vector<uint8_t>& scratch, std::string* error) const {
    const int w = width(), h = height(), bpp = bitsPerPixel();
    const size_t pixels = size_t(w) * h;

    switch (compression()) {
        case Compression::Lossless: {
            if (!readFrameData(index, scratch, error)) return false;
            int jw = 0, jh = 0, jbits = 0, components = 0;
            lj92 decoder = nullptr;
            int ret = lj92_open(&decoder, scratch.data(), int(scratch.size()), &jw, &jh, &jbits, &components);
            if (ret != LJ92_ERROR_NONE) {
                if (error) *error = "Lossless decoder: corrupt frame " + std::to_string(index) + " (" + std::to_string(ret) + ")";
                return false;
            }
            if (size_t(jw) * jh * components != pixels) {
                lj92_close(decoder);
                if (error) *error = "Lossless frame " + std::to_string(index) + " has the wrong size";
                return false;
            }
            ret = lj92_decode(decoder, out, jw * jh * components, 0, nullptr, 0);
            lj92_close(decoder);
            if (ret != LJ92_ERROR_NONE) {
                if (error) *error = "Lossless decoder: corrupt frame " + std::to_string(index) + " (" + std::to_string(ret) + ")";
                return false;
            }
            break;
        }
        case Compression::Uncompressed: {
            const size_t needed = pixels * size_t(bpp) / 8;
            if (!readFrameData(index, scratch, error)) return false;
            if (scratch.size() < needed) {
                if (error) *error = "Frame " + std::to_string(index) + " is too short";
                return false;
            }
            scratch.resize(needed + 8, 0);  // slack for the unpacker
            unpackBits(scratch.data(), out, pixels, bpp);
            break;
        }
        case Compression::Jpeg2000:
            if (error) *error = "JPEG 2000 compressed MLV is not supported";
            return false;
        default:
            if (error) *error = "This MLV's compression is not supported";
            return false;
    }

    if (!lineariseLut.empty()) {
        const uint16_t* lut = lineariseLut.data();
        for (size_t i = 0; i < pixels; ++i) out[i] = lut[out[i]];
    }
    return true;
}

bool Clip::readSyncedAudio(std::vector<uint8_t>& pcm, std::string* error) const {
    pcm.clear();
    if (!hasAudio()) return true;
    std::vector<uint8_t> raw;
    raw.reserve(size_t(audioStarts().back()));
    for (const auto& a : audio_) {
        const size_t at = raw.size();
        raw.resize(at + a.dataSize);
        if (!chunks_[a.chunk]->readAt(a.dataOffset, raw.data() + at, a.dataSize)) {
            if (error) *error = "Audio read error";
            return false;
        }
    }
    const AudioSync s = audioSync();
    pcm.assign(size_t(s.size), 0);
    const uint64_t copy = std::min<uint64_t>(s.total - s.drop, s.size > s.pad ? s.size - s.pad : 0);
    if (copy) std::memcpy(pcm.data() + s.pad, raw.data() + s.drop, size_t(copy));
    return true;
}

const std::vector<uint64_t>& Clip::audioStarts() const {
    std::call_once(audioStartsOnce_, [this] {
        audioStarts_.reserve(audio_.size() + 1);
        uint64_t at = 0;
        for (const auto& a : audio_) {
            audioStarts_.push_back(at);
            at += a.dataSize;
        }
        audioStarts_.push_back(at);
    });
    return audioStarts_;
}

// MLV-App's readMlvAudioData(): the offset of the first sample against the first frame (whole
// samples), the length rounded up to 1024 samples and cut to the frames' length at the
// recorded rate.
Clip::AudioSync Clip::audioSync() const {
    AudioSync s;
    if (!hasAudio()) return s;
    s.total = audioStarts().back();
    const uint64_t sampleSize = uint64_t(wavi.channels) * (wavi.bitsPerSample / 8);
    if (sampleSize == 0 || wavi.samplingRate == 0) return s;
    const uint64_t blockAlign = sampleSize * 1024;
    const double bytesPerMicrosecond = double(wavi.samplingRate) * double(sampleSize) / 1000000.0;
    const int64_t sync = int64_t((double(frames_[0].timestamp) - double(audio_[0].timestamp)) * bytesPerMicrosecond);
    if (sync >= 0) s.drop = uint64_t(sync) - uint64_t(sync) % sampleSize;
    else s.pad = uint64_t(-sync) - uint64_t(-sync) % sampleSize;
    s.drop = std::min(s.drop, s.total);
    const uint64_t synced = s.total - s.drop + s.pad;
    const uint64_t syncedAligned = synced - synced % blockAlign + blockAlign;
    const double seconds = fps() > 0 ? double(frames_.size()) / fps() : 0.0;
    const uint64_t theoretic = uint64_t(double(wavi.samplingRate) * double(sampleSize) * seconds);
    const uint64_t theoreticAligned = theoretic - theoretic % blockAlign + blockAlign;
    s.size = std::min(theoreticAligned, syncedAligned);
    return s;
}

bool Clip::readSyncedAudioRange(uint64_t offset, size_t n, uint8_t* out, std::string* error) const {
    std::memset(out, 0, n);
    const AudioSync s = audioSync();
    // Synced byte o is recorded byte o - pad + drop: silence before `pad`, nothing past `size`
    // or past the recording.
    const uint64_t begin = std::max(offset, s.pad);
    const uint64_t end = std::min(offset + n, s.size);
    if (begin >= end) return true;
    uint64_t from = begin - s.pad + s.drop;
    const uint64_t to = std::min(end - s.pad + s.drop, s.total);
    if (from >= to) return true;
    uint8_t* dst = out + (begin - offset);
    const auto& starts = audioStarts();
    size_t i = size_t(std::upper_bound(starts.begin(), starts.end(), from) - starts.begin()) - 1;
    while (from < to) {
        const AudioIndex& a = audio_[i];
        const uint64_t within = from - starts[i];
        const uint64_t take = std::min<uint64_t>(a.dataSize - within, to - from);
        if (!chunks_[a.chunk]->readAt(a.dataOffset + within, dst, size_t(take))) {
            if (error) *error = "Audio read error";
            return false;
        }
        dst += take;
        from += take;
        ++i;
    }
    return true;
}

}  // namespace mlv
