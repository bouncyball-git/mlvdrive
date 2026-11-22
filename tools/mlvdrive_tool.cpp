// mlvdrive_tool: the core without a filesystem.
//
//   mlvdrive_tool info  <clip.MLV> [frame]               clip details; decodes one frame (timings)
//   mlvdrive_tool dng   <clip.MLV> <frame> <out.dng>     writes one frame as a DNG
//   mlvdrive_tool check <clip.MLV> <frame> <file.dng>    reads a DNG back: its image must equal the
//                                                        frame decoded and corrected now
//   mlvdrive_tool bench <clip.MLV> [frames]              ms a frame per stage, then frames/s on
//                                                        every thread
//   mlvdrive_tool wav   <clip.MLV> <out.wav>             the clip's audio as the drive shows it
//   mlvdrive_tool patterns <folder>                      the first frame of each test pattern as
//                                                        a DNG (01.dng ... 12.dng)
//   mlvdrive_tool selftest <clip.MLV>                    every frame: the fast decoder against the
//                                                        original one, the vector raw corrections
//                                                        against the scalar ones (value for value);
//                                                        every 32nd: the fast lossless encoder
//                                                        against the original (byte for byte);
//                                                        the audio read in pieces against whole
//   mlvdrive_tool read  <folder>                         reads every .dng in a folder (a clip's
//                                                        folder on the drive) in name order:
//                                                        frames/s, MB/s, ms a file
//
// Options (dng, check, bench): --dng=16|packed|lj92|pass  (default 16), --no-fix (no raw corrections)
// Options (dng, check, bench, selftest): --focus-pixels=auto|on|off, --bad-pixels=off|auto|aggressive|map
// Options (dng, wav): --timecode=time|count  (DNG TimeCodes / WAV time reference: time of day, the
//                     default, or frames from the start)
// Options (read): --threads=N readers at once (default 1), --passes=N (default 2), --buffered
//                 (through Windows' file cache; by default unbuffered, what the drive itself gives)
#include "dng/dng_writer.h"
#include "liblj92/lj92.h"
#include "mlv/clip.h"
#include "mlv/color.h"
#include "mlv/wav.h"
#include "raw/rawfix.h"
#include "util/file.h"
#include "vfs/test_patterns.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

int usage() {
    std::fprintf(stderr,
                 "MLV Drive " MLVDRIVE_VERSION ", the core on the command line\n"
                 "usage: mlvdrive_tool info  <clip.MLV> [frame]\n"
                 "       mlvdrive_tool dng   <clip.MLV> <frame> <out.dng> [--dng=16|packed|lj92|pass] [--no-fix]\n"
                 "       mlvdrive_tool check <clip.MLV> <frame> <file.dng> [--no-fix]\n"
                 "       mlvdrive_tool bench <clip.MLV> [frames] [--dng=16|packed|lj92|pass] [--no-fix]\n"
                 "       mlvdrive_tool wav   <clip.MLV> <out.wav> [--timecode=time|count]\n"
                 "       mlvdrive_tool patterns <folder>\n"
                 "       mlvdrive_tool selftest <clip.MLV>\n"
                 "       mlvdrive_tool read  <folder> [--threads=N] [--passes=N] [--buffered]\n");
    return 2;
}

struct Args {
    std::vector<std::string> positional;
    dng::Output output = dng::Output::Uncompressed16;
    bool fix = true;
    int threads = 1;
    int passes = 2;
    bool buffered = false;
    dng::Timecode timecode = dng::Timecode::TimeOfDay;
    raw::FixSettings fixes;  // focus and bad pixels
};

bool parseArgs(int argc, char** argv, Args* a) {
    for (int i = 2; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "--no-fix") {
            a->fix = false;
        } else if (s.rfind("--dng=", 0) == 0) {
            std::string v = s.substr(6);
            if (v == "16") a->output = dng::Output::Uncompressed16;
            else if (v == "packed") a->output = dng::Output::Uncompressed;
            else if (v == "lj92") a->output = dng::Output::Lossless;
            else if (v == "pass") a->output = dng::Output::PassThrough;
            else return false;
        } else if (s.rfind("--threads=", 0) == 0) {
            a->threads = std::max(1, std::atoi(s.c_str() + 10));
        } else if (s.rfind("--passes=", 0) == 0) {
            a->passes = std::max(1, std::atoi(s.c_str() + 9));
        } else if (s == "--buffered") {
            a->buffered = true;
        } else if (s == "--focus-pixels=auto") {
            a->fixes.focusPixels = -1;
        } else if (s == "--focus-pixels=on") {
            a->fixes.focusPixels = 1;
        } else if (s == "--focus-pixels=off") {
            a->fixes.focusPixels = 0;
        } else if (s == "--bad-pixels=off") {
            a->fixes.badPixels = 0;
        } else if (s == "--bad-pixels=auto") {
            a->fixes.badPixels = 1;
            a->fixes.badPixelSearch = 0;
        } else if (s == "--bad-pixels=aggressive") {
            a->fixes.badPixels = 1;
            a->fixes.badPixelSearch = 1;
        } else if (s == "--bad-pixels=map") {
            a->fixes.badPixels = 3;
        } else if (s == "--timecode=time") {
            a->timecode = dng::Timecode::TimeOfDay;
        } else if (s == "--timecode=count") {
            a->timecode = dng::Timecode::FrameCount;
        } else if (s.rfind("--", 0) == 0) {
            return false;
        } else {
            a->positional.push_back(s);
        }
    }
    return true;
}

const char* outputName(dng::Output o) {
    switch (o) {
        case dng::Output::Uncompressed16: return "16-bit";
        case dng::Output::Uncompressed: return "packed";
        case dng::Output::Lossless: return "LJ92";
        case dng::Output::PassThrough: return "pass-through";
    }
    return "?";
}

std::unique_ptr<mlv::Clip> openClip(const std::string& path) {
    std::string error;
    auto clip = mlv::Clip::open(path, &error);
    if (!clip) std::fprintf(stderr, "can't open %s: %s\n", path.c_str(), error.c_str());
    else if (clip->frameCount() == 0) {
        std::fprintf(stderr, "%s has no frames\n", path.c_str());
        clip.reset();
    }
    return clip;
}

bool parseFrame(const std::string& text, const mlv::Clip& clip, uint32_t* frame) {
    char* end = nullptr;
    unsigned long v = std::strtoul(text.c_str(), &end, 10);
    if (text.empty() || *end || v >= clip.frameCount()) {
        std::fprintf(stderr, "frame '%s' out of range (0..%u)\n", text.c_str(), clip.frameCount() - 1);
        return false;
    }
    *frame = uint32_t(v);
    return true;
}

// --- info ---

int info(const Args& a) {
    if (a.positional.empty() || a.positional.size() > 2) return usage();
    const std::string& path = a.positional[0];
    std::string error;
    auto t0 = Clock::now();
    auto clip = mlv::Clip::open(path, &error);
    if (!clip) {
        std::fprintf(stderr, "can't open %s: %s\n", path.c_str(), error.c_str());
        return 1;
    }
    double openMs = msSince(t0);
    const auto& ri = clip->rawi.raw_info;

    std::printf("clip        %s (%d chunk%s, %.1f MB)\n", clip->name().c_str(), clip->chunkCount(),
                clip->chunkCount() == 1 ? "" : "s", clip->totalBytes() / 1e6);
    std::printf("camera      %.32s (model 0x%08X)\n", clip->idnt.cameraName, clip->idnt.cameraModel);
    std::printf("image       %d x %d, %d-bit, %s, black %d, white %d, CFA 0x%08X\n", clip->width(),
                clip->height(), clip->bitsPerPixel(), clip->compressionName().c_str(), clip->blackLevel(),
                clip->whiteLevel(), clip->cfaPattern());
    std::printf("sensor      raw buffer %d x %d, active area (y1 x1 y2 x2) %d %d %d %d, crop origin %d %d\n",
                ri.width, ri.height, ri.active_area.y1, ri.active_area.x1, ri.active_area.y2, ri.active_area.x2,
                ri.jpeg.x, ri.jpeg.y);
    if (clip->hasRawc())
        std::printf("capture     binning %d x %d, skipping %d x %d\n", clip->rawc.binning_x, clip->rawc.binning_y,
                    clip->rawc.skipping_x, clip->rawc.skipping_y);
    std::printf("frames      %u at %.3f fps\n", clip->frameCount(), clip->fps());
    std::printf("exposure    ISO %u, 1/%.0f s\n", clip->expo.isoValue,
                clip->expo.shutterValue ? 1e6 / double(clip->expo.shutterValue) : 0.0);
    std::printf("white       %.0f K as shot\n", color::asShotKelvin(*clip));
    if (clip->hasAudio())
        std::printf("audio       %u ch, %u Hz\n", clip->wavi.channels, clip->wavi.samplingRate);
    else
        std::printf("audio       none\n");
    std::printf("opened in   %.1f ms\n", openMs);
    if (clip->frameCount() == 0) return 0;

    dng::Writer w16(*clip, dng::Options{}, nullptr);
    dng::Options packed;
    packed.output = dng::Output::Uncompressed;
    dng::Writer wPacked(*clip, packed, nullptr);
    std::printf("DNG         header %llu bytes; 16-bit file %llu bytes, packed %llu bytes\n",
                (unsigned long long)w16.headerSize(), (unsigned long long)w16.fileSize(),
                (unsigned long long)wPacked.fileSize());

    uint32_t frame = 0;
    if (a.positional.size() == 2 && !parseFrame(a.positional[1], *clip, &frame)) return 1;
    std::vector<uint16_t> image(size_t(clip->width()) * clip->height());
    std::vector<uint8_t> scratch;
    raw::Fixer fixer(*clip, a.fixes);
    // Twice: the first decode and fix pay for allocations and the fixer's one-off work.
    double decodeMs = 0, fixMs = 0;
    for (int pass = 0; pass < 2; ++pass) {
        t0 = Clock::now();
        if (!clip->decodeFrame(frame, image.data(), scratch, &error)) {
            std::fprintf(stderr, "frame %u: %s\n", frame, error.c_str());
            return 1;
        }
        decodeMs = msSince(t0);
        t0 = Clock::now();
        fixer.apply(image.data(), frame);
        fixMs = msSince(t0);
    }
    auto [lo, hi] = std::minmax_element(image.begin(), image.end());
    std::printf("frame %-5u decode %.1f ms, corrections %.1f ms (%s), values %u..%u\n", frame, decodeMs, fixMs,
                fixer.active() ? "on" : "none needed", *lo, *hi);
    return 0;
}

// --- dng ---

int writeDng(const Args& a) {
    if (a.positional.size() != 3) return usage();
    auto clip = openClip(a.positional[0]);
    if (!clip) return 1;
    uint32_t frame;
    if (!parseFrame(a.positional[1], *clip, &frame)) return 1;

    std::unique_ptr<raw::Fixer> fixer;
    if (a.fix) fixer = std::make_unique<raw::Fixer>(*clip, a.fixes);
    dng::Options options;
    options.output = a.output;
    options.timecode = a.timecode;
    dng::Writer writer(*clip, options, fixer.get());
    dng::Writer::Scratch scratch;
    std::string error;
    uint64_t bytes = 0;
    auto t0 = Clock::now();
    if (!writer.writeFrame(frame, a.positional[2], scratch, &bytes, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::printf("%s: frame %u, %s, %llu bytes (header %llu), %.1f ms\n", a.positional[2].c_str(), frame,
                outputName(writer.effectiveOutput()), (unsigned long long)bytes,
                (unsigned long long)writer.headerSize(), msSince(t0));
    return 0;
}

// --- check ---

// The tags of a DNG's first IFD that say where its image is and how it is stored.
struct DngImage {
    uint32_t width = 0, height = 0, bits = 0, compression = 0, offset = 0, bytes = 0;
};

bool readDngImage(const std::vector<uint8_t>& f, DngImage* d, std::string* error) {
    auto u16 = [&](size_t at) { return uint32_t(f[at]) | uint32_t(f[at + 1]) << 8; };
    auto u32 = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };
    if (f.size() < 8 || f[0] != 'I' || f[1] != 'I' || u16(2) != 42) {
        *error = "not a little-endian TIFF/DNG";
        return false;
    }
    size_t ifd = u32(4);
    if (ifd + 2 > f.size()) {
        *error = "IFD0 out of the file";
        return false;
    }
    uint32_t n = u16(ifd);
    if (ifd + 2 + size_t(n) * 12 > f.size()) {
        *error = "IFD0 runs past the file";
        return false;
    }
    for (uint32_t i = 0; i < n; ++i) {
        size_t e = ifd + 2 + size_t(i) * 12;
        uint32_t tag = u16(e), type = u16(e + 2);
        uint32_t value = type == 3 ? u16(e + 8) : u32(e + 8);  // SHORT or LONG, one value
        switch (tag) {
            case 256: d->width = value; break;
            case 257: d->height = value; break;
            case 258: d->bits = value; break;
            case 259: d->compression = value; break;
            case 273: d->offset = value; break;
            case 279: d->bytes = value; break;
        }
    }
    if (!d->width || !d->height || !d->bits || !d->bytes || uint64_t(d->offset) + d->bytes > f.size()) {
        *error = "image tags missing or out of the file";
        return false;
    }
    return true;
}

// The DNG's image as 16-bit values.
bool unpackDngImage(const std::vector<uint8_t>& f, const DngImage& d, std::vector<uint16_t>& out,
                    std::string* error) {
    const uint8_t* p = f.data() + d.offset;
    out.assign(size_t(d.width) * d.height, 0);
    if (d.compression == 1 && d.bits == 16) {
        if (d.bytes < out.size() * 2) {
            *error = "strip too short";
            return false;
        }
        std::memcpy(out.data(), p, out.size() * 2);
        return true;
    }
    if (d.compression == 1) {  // bit-packed, big-endian, rows start on a byte
        const size_t rowBytes = (size_t(d.width) * d.bits + 7) / 8;
        if (d.bytes < rowBytes * d.height) {
            *error = "strip too short";
            return false;
        }
        for (uint32_t y = 0; y < d.height; ++y) {
            const uint8_t* row = p + rowBytes * y;
            uint64_t acc = 0;
            int have = 0;
            size_t next = 0;
            for (uint32_t x = 0; x < d.width; ++x) {
                while (have < int(d.bits)) {
                    acc = (acc << 8) | row[next++];
                    have += 8;
                }
                have -= d.bits;
                out[size_t(y) * d.width + x] = uint16_t((acc >> have) & ((1u << d.bits) - 1));
            }
        }
        return true;
    }
    if (d.compression == 7) {
        lj92 lj = nullptr;
        int w = 0, h = 0, bits = 0, comps = 0;
        int ret = lj92_open(&lj, const_cast<uint8_t*>(p), int(d.bytes), &w, &h, &bits, &comps);
        if (ret != LJ92_ERROR_NONE) {
            *error = "lossless JPEG header unreadable";
            return false;
        }
        if (size_t(w) * h * comps != out.size()) {
            lj92_close(lj);
            *error = "lossless JPEG size doesn't match the image";
            return false;
        }
        ret = lj92_decode(lj, out.data(), w * comps, 0, nullptr, 0);
        lj92_close(lj);
        if (ret != LJ92_ERROR_NONE) {
            *error = "lossless JPEG doesn't decode";
            return false;
        }
        return true;
    }
    *error = "compression " + std::to_string(d.compression) + " not handled";
    return false;
}

int check(const Args& a) {
    if (a.positional.size() != 3) return usage();
    auto clip = openClip(a.positional[0]);
    if (!clip) return 1;
    uint32_t frame;
    if (!parseFrame(a.positional[1], *clip, &frame)) return 1;

    std::vector<uint8_t> file;
    {
        util::RandomAccessFile f;
        if (!f.open(a.positional[2])) {
            std::fprintf(stderr, "can't open %s\n", a.positional[2].c_str());
            return 1;
        }
        file.resize(size_t(f.size()));
        if (!f.readAt(0, file.data(), file.size())) {
            std::fprintf(stderr, "can't read %s\n", a.positional[2].c_str());
            return 1;
        }
    }
    DngImage d;
    std::string error;
    std::vector<uint16_t> fromDng;
    if (!readDngImage(file, &d, &error) || !unpackDngImage(file, d, fromDng, &error)) {
        std::fprintf(stderr, "%s: %s\n", a.positional[2].c_str(), error.c_str());
        return 1;
    }
    if (d.width != uint32_t(clip->width()) || d.height != uint32_t(clip->height())) {
        std::fprintf(stderr, "size %u x %u, the clip's is %d x %d\n", d.width, d.height, clip->width(),
                     clip->height());
        return 1;
    }

    std::vector<uint16_t> expected(size_t(clip->width()) * clip->height());
    std::vector<uint8_t> scratch;
    if (!clip->decodeFrame(frame, expected.data(), scratch, &error)) {
        std::fprintf(stderr, "frame %u: %s\n", frame, error.c_str());
        return 1;
    }
    if (a.fix) {
        raw::Fixer fixer(*clip, a.fixes);
        fixer.apply(expected.data(), frame);
    }
    size_t differ = 0, first = 0;
    for (size_t i = 0; i < expected.size(); ++i) {
        if (expected[i] != fromDng[i] && differ++ == 0) first = i;
    }
    std::printf("%s: %u x %u, %u bits, compression %u, image at %u (%u bytes)\n", a.positional[2].c_str(), d.width,
                d.height, d.bits, d.compression, d.offset, d.bytes);
    if (differ) {
        std::printf("DIFFERENT: %zu of %zu values; first at x %zu y %zu: DNG %u, frame %u\n", differ,
                    expected.size(), first % d.width, first / d.width, fromDng[first], expected[first]);
        return 1;
    }
    std::printf("same as frame %u decoded%s\n", frame, a.fix ? " and corrected" : "");
    return 0;
}

// --- bench ---

int bench(const Args& a) {
    if (a.positional.empty() || a.positional.size() > 2) return usage();
    auto clip = openClip(a.positional[0]);
    if (!clip) return 1;
    uint32_t count = std::min<uint32_t>(48, clip->frameCount());
    if (a.positional.size() == 2) {
        count = uint32_t(std::strtoul(a.positional[1].c_str(), nullptr, 10));
        count = std::max<uint32_t>(1, std::min(count, clip->frameCount()));
    }

    std::unique_ptr<raw::Fixer> fixer;
    if (a.fix) fixer = std::make_unique<raw::Fixer>(*clip, a.fixes);
    dng::Options options;
    options.output = a.output;
    dng::Writer writer(*clip, options, fixer.get());
    std::string error;

    // One thread, stage by stage (frame 0 first, unmeasured: allocations, the fixer's set-up).
    std::vector<uint8_t> data;
    std::vector<uint16_t> pixels(size_t(clip->width()) * clip->height());
    dng::Writer::Scratch scratch;
    std::vector<uint8_t> file;
    if (!clip->decodeFrame(0, pixels.data(), data, &error) || !writer.renderFrame(0, file, scratch, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    if (fixer) fixer->apply(pixels.data(), 0);
    double readMs = 0, decodeMs = 0, fixMs = 0, renderMs = 0;
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < count; ++i) {
        auto t0 = Clock::now();
        if (!clip->readFrameData(i, data, &error)) break;
        readMs += msSince(t0);
        t0 = Clock::now();
        if (!clip->decodeFrame(i, pixels.data(), data, &error)) break;
        decodeMs += msSince(t0);
        if (fixer) {
            t0 = Clock::now();
            fixer->apply(pixels.data(), i);
            fixMs += msSince(t0);
        }
        t0 = Clock::now();
        if (!writer.renderFrame(i, file, scratch, &error)) break;
        renderMs += msSince(t0);
        bytes += file.size();
    }
    if (!error.empty()) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::printf("%s, %u frames, DNG %s%s\n", clip->name().c_str(), count, outputName(writer.effectiveOutput()),
                a.fix && writer.effectiveOutput() != dng::Output::PassThrough ? ", raw corrections" : "");
    std::printf("one thread, ms a frame:  read %.2f   decode (with read) %.2f   corrections %.2f   whole DNG %.2f"
                "   (%.2f MB a DNG)\n",
                readMs / count, decodeMs / count, fixMs / count, renderMs / count, bytes / 1e6 / count);

    // Every thread making whole DNGs, as the drive will while Resolve plays.
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    std::atomic<uint32_t> next{0};
    std::atomic<uint64_t> total{0};
    std::atomic<bool> failed{false};
    auto t0 = Clock::now();
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back([&] {
            dng::Writer::Scratch s;
            std::vector<uint8_t> out;
            std::string e;
            for (uint32_t i; (i = next++) < count;) {
                if (!writer.renderFrame(i, out, s, &e)) {
                    failed = true;
                    return;
                }
                total += out.size();
            }
        });
    }
    for (auto& t : pool) t.join();
    double ms = msSince(t0);
    if (failed) {
        std::fprintf(stderr, "a frame failed\n");
        return 1;
    }
    std::printf("%u threads:  %.1f frames/s, %.0f MB/s of DNG (real time: %.3f fps)\n", threads, count * 1000.0 / ms,
                total / 1e3 / ms, clip->fps());
    return 0;
}

// --- wav ---

int writeWavFile(const Args& a) {
    if (a.positional.size() != 2) return usage();
    auto clip = openClip(a.positional[0]);
    if (!clip) return 1;
    if (!clip->hasAudio()) {
        std::fprintf(stderr, "%s has no audio\n", a.positional[0].c_str());
        return 1;
    }
    dng::Options options;
    options.timecode = a.timecode;
    dng::Writer writer(*clip, options, nullptr);
    const mlv::WavLayout wav =
        mlv::wavLayout(*clip, mlv::wavTimeReference(*clip, writer.timecodeFrame(0), writer.frameRate()));
    std::vector<uint8_t> bytes(size_t(wav.size()));
    std::string error;
    if (!mlv::readWav(*clip, wav, 0, bytes.size(), bytes.data(), &error) ||
        !util::writeFile(a.positional[1], bytes.data(), bytes.size(), nullptr, 0, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    const double seconds = double(wav.dataBytes) / (clip->wavi.samplingRate * clip->wavi.channels * 2.0);
    std::printf("%s: %u ch, %u Hz, %.3f s (the frames: %.3f s), %zu bytes (header %zu); timecode frame %lld\n",
                a.positional[1].c_str(), clip->wavi.channels, clip->wavi.samplingRate, seconds,
                clip->frameCount() / clip->fps(), bytes.size(), wav.header.size(), (long long)writer.timecodeFrame(0));
    return 0;
}

// --- patterns ---

int writePatterns(const Args& a) {
    if (a.positional.size() != 1) return usage();
    auto source = vfs::makeTestPatterns(true);
    const uint32_t each = source->frameCount() / vfs::kTestPatternCount;
    std::vector<uint8_t> bytes;
    std::string error;
    for (int p = 0; p < vfs::kTestPatternCount; ++p) {
        char name[16];
        std::snprintf(name, sizeof name, "%02d.dng", p + 1);
        const std::string path = a.positional[0] + "/" + name;
        auto t0 = Clock::now();
        if (!source->renderFrame(uint32_t(p) * each, bytes, &error) ||
            !util::writeFile(path, bytes.data(), bytes.size(), nullptr, 0, &error)) {
            std::fprintf(stderr, "%s: %s\n", path.c_str(), error.c_str());
            return 1;
        }
        std::printf("%s  %-32s %.0f ms\n", name, vfs::testPatternName(p), msSince(t0));
    }
    return 0;
}

// --- selftest ---

int selftest(const Args& a) {
    if (a.positional.size() != 1) return usage();
    auto clip = openClip(a.positional[0]);
    if (!clip) return 1;
    const size_t px = size_t(clip->width()) * clip->height();
    std::vector<uint16_t> fast(px), ref(px), fixedRef(px);
    std::vector<uint8_t> scratch;
    std::string error;
    raw::Fixer fixVector(*clip, a.fixes), fixScalar(*clip, a.fixes);
    uint32_t decodeDiffer = 0, fixDiffer = 0, encodeDiffer = 0, encoded = 0;
    double fastMs = 0, refMs = 0, vecMs = 0, scalarMs = 0;
    // The lossless encoder as the DNG writer calls it (two rows per JPEG row), fast or original.
    const int w = clip->width(), h = clip->height(), bpp = clip->bitsPerPixel();
    auto encode = [&](uint16_t* image, bool fastCode, std::vector<uint8_t>& out) {
        lj92_fast = fastCode ? 1 : 0;
        uint8_t* bytes = nullptr;
        int size = 0;
        const int ret = lj92_encode(image, w * 2, h / 2, bpp, w * 2 * (h / 2), 0, nullptr, 0, &bytes, &size);
        lj92_fast = 1;
        const bool ok = ret == LJ92_ERROR_NONE && bytes;
        out.assign(bytes, bytes + (ok ? size : 0));
        std::free(bytes);
        return ok;
    };
    std::vector<uint8_t> encFast, encRef;
    for (uint32_t f = 0; f < clip->frameCount(); ++f) {
        lj92_fast = 1;
        auto t0 = Clock::now();
        bool ok = clip->decodeFrame(f, fast.data(), scratch, &error);
        fastMs += msSince(t0);
        lj92_fast = 0;
        t0 = Clock::now();
        ok = ok && clip->decodeFrame(f, ref.data(), scratch, &error);
        refMs += msSince(t0);
        lj92_fast = 1;
        if (!ok) {
            std::fprintf(stderr, "frame %u: %s\n", f, error.c_str());
            return 1;
        }
        if (fast != ref && decodeDiffer++ == 0) std::printf("frame %u: the decoders differ\n", f);

        fixedRef = ref;
        raw::setSimd(true);
        t0 = Clock::now();
        fixVector.apply(fast.data(), f);
        vecMs += msSince(t0);
        raw::setSimd(false);
        t0 = Clock::now();
        fixScalar.apply(fixedRef.data(), f);
        scalarMs += msSince(t0);
        raw::setSimd(true);
        if (fast != fixedRef && fixDiffer++ == 0) std::printf("frame %u: the raw corrections differ\n", f);

        // Every 32nd frame (the original encoder is slow): the same bytes from both.
        if (f % 32 == 0) {
            ++encoded;
            const bool same = encode(fast.data(), true, encFast) && encode(fast.data(), false, encRef) && encFast == encRef;
            if (!same && encodeDiffer++ == 0) std::printf("frame %u: the lossless encoders differ\n", f);
        }
    }
    // The audio: read whole (the reference) and in odd-sized pieces, from the blocks they come from.
    uint32_t audioDiffer = 0;
    std::vector<uint8_t> whole, pieces;
    if (!clip->readSyncedAudio(whole, &error)) {
        std::fprintf(stderr, "audio: %s\n", error.c_str());
        return 1;
    }
    const uint64_t audioBytes = clip->syncedAudioBytes();
    if (audioBytes != whole.size()) audioDiffer = 1;
    pieces.assign(size_t(audioBytes) + 5000, 0xAB);  // past the end too: must read as zeros
    for (size_t at = 0; at < pieces.size();) {
        const size_t n = std::min<size_t>(7919, pieces.size() - at);
        if (!clip->readSyncedAudioRange(at, n, pieces.data() + at, &error)) {
            std::fprintf(stderr, "audio: %s\n", error.c_str());
            return 1;
        }
        at += n;
    }
    if (!std::equal(whole.begin(), whole.end(), pieces.begin()) ||
        std::any_of(pieces.begin() + ptrdiff_t(whole.size()), pieces.end(), [](uint8_t b) { return b != 0; }))
        audioDiffer = 1;

    const uint32_t n = clip->frameCount();
    std::printf("%s, %u frames (%s)\n", clip->name().c_str(), n,
                fixVector.active() ? "raw corrections on" : "no corrections needed");
    std::printf("decoder:     %u frames differ; fast %.2f ms a frame, original %.2f\n", decodeDiffer, fastMs / n,
                refMs / n);
    std::printf("corrections: %u frames differ; vector %.2f ms a frame, scalar %.2f\n", fixDiffer, vecMs / n,
                scalarMs / n);
    std::printf("encoder:     %u of %u frames differ (fast and original lossless encoder, byte for byte)\n",
                encodeDiffer, encoded);
    std::printf("audio:       %s (%zu bytes)\n", audioDiffer ? "DIFFERS" : "the same in pieces as whole", whole.size());
    return decodeDiffer || fixDiffer || encodeDiffer || audioDiffer ? 1 : 0;
}

// --- read ---

// Reads a whole file in 1 MB pieces; the bytes read, or -1.
int64_t readWholeFile(const std::filesystem::path& path, uint8_t* buffer, size_t size, bool buffered) {
#ifdef _WIN32
    DWORD flags = FILE_FLAG_SEQUENTIAL_SCAN | (buffered ? 0 : FILE_FLAG_NO_BUFFERING);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, flags, nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;
    int64_t total = 0;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buffer, DWORD(size), &got, nullptr)) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            total = -1;
            break;
        }
        total += got;
        if (got < size) break;
    }
    CloseHandle(h);
    return total;
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return -1;
#ifdef __APPLE__
    if (!buffered) ::fcntl(fd, F_NOCACHE, 1);  // past the page cache: what the drive itself gives
#else
    (void)buffered;
#endif
    int64_t total = 0;
    for (;;) {
        const ssize_t got = ::read(fd, buffer, size);
        if (got < 0) {
            if (errno == EINTR) continue;
            total = -1;
            break;
        }
        if (got == 0) break;
        total += got;
    }
    ::close(fd);
    return total;
#endif
}

int readFolder(const Args& a) {
    if (a.positional.size() != 1) return usage();
    namespace fs = std::filesystem;
    const fs::path folder = util::u8(a.positional[0]);
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(folder, ec)) {
        std::string ext = util::u8(e.path().extension());
        for (auto& ch : ext) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        if (ext == ".dng") files.push_back(e.path());
    }
    if (ec || files.empty()) {
        std::fprintf(stderr, "no .dng files in %s\n", a.positional[0].c_str());
        return 1;
    }
    std::sort(files.begin(), files.end());
    std::printf("%s: %zu files, %d reader%s, %s\n", a.positional[0].c_str(), files.size(), a.threads,
                a.threads == 1 ? "" : "s", a.buffered ? "buffered (Windows' cache)" : "unbuffered");

    constexpr size_t kChunk = 1 << 20;
    for (int pass = 1; pass <= a.passes; ++pass) {
        std::atomic<size_t> next{0};
        std::atomic<uint64_t> bytes{0};
        std::atomic<bool> failed{false};
        std::mutex timesMutex;
        std::vector<double> times;
        auto t0 = Clock::now();
        std::vector<std::thread> readers;
        for (int t = 0; t < a.threads; ++t) {
            readers.emplace_back([&] {
                // Unbuffered reads need a buffer aligned to the sector size.
                std::unique_ptr<uint8_t, void (*)(void*)> buffer(
                    static_cast<uint8_t*>(
#ifdef _WIN32
                        _aligned_malloc(kChunk, 4096)),
                    _aligned_free);
#else
                        std::aligned_alloc(4096, kChunk)),
                    std::free);
#endif
                std::vector<double> mine;
                for (size_t i; (i = next++) < files.size();) {
                    auto f0 = Clock::now();
                    int64_t n = readWholeFile(files[i], buffer.get(), kChunk, a.buffered);
                    if (n < 0) {
                        failed = true;
                        return;
                    }
                    mine.push_back(msSince(f0));
                    bytes += uint64_t(n);
                }
                std::lock_guard<std::mutex> lock(timesMutex);
                times.insert(times.end(), mine.begin(), mine.end());
            });
        }
        for (auto& r : readers) r.join();
        const double ms = msSince(t0);
        if (failed) {
            std::fprintf(stderr, "a read failed\n");
            return 1;
        }
        std::sort(times.begin(), times.end());
        double sum = 0;
        for (double t : times) sum += t;
        std::printf("pass %d: %.1f frames/s, %.0f MB/s; ms a file: mean %.1f, median %.1f, worst %.1f\n", pass,
                    files.size() * 1000.0 / ms, bytes / 1e3 / ms, sum / times.size(), times[times.size() / 2],
                    times.back());
    }
    return 0;
}

int run(int argc, char** argv) {
    if (argc < 2) return usage();
    Args a;
    if (!parseArgs(argc, argv, &a)) return usage();
    raw::setPixelMapFolders(raw::defaultPixelMapFolders());
    std::string cmd = argv[1];
    int rc;
    if (cmd == "info") rc = info(a);
    else if (cmd == "dng") rc = writeDng(a);
    else if (cmd == "check") rc = check(a);
    else if (cmd == "bench") rc = bench(a);
    else if (cmd == "read") rc = readFolder(a);
    else if (cmd == "selftest") rc = selftest(a);
    else if (cmd == "wav") rc = writeWavFile(a);
    else if (cmd == "patterns") rc = writePatterns(a);
    else return usage();
    return rc;
}

}  // namespace

#ifdef _WIN32
// The arguments as UTF-8, as the rest takes paths (main's are in the ANSI code page: a path
// with characters outside it wouldn't open).
int wmain(int argc, wchar_t** wargv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(util::narrow(wargv[i]));
    std::vector<char*> argv;
    for (auto& s : args) argv.push_back(s.data());
    argv.push_back(nullptr);
    return run(argc, argv.data());
}
#else
int main(int argc, char** argv) { return run(argc, argv); }
#endif
