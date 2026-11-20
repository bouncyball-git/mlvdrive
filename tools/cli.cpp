// mlvdrive_cli: the core without a filesystem.
//
//   mlvdrive_cli info  <clip.MLV> [frame]               clip details; decodes one frame (timings)
//   mlvdrive_cli dng   <clip.MLV> <frame> <out.dng>     writes one frame as a DNG
//   mlvdrive_cli check <clip.MLV> <frame> <file.dng>    reads a DNG back: its image must equal the
//                                                       frame decoded and corrected now
//   mlvdrive_cli bench <clip.MLV> [frames]              ms a frame per stage, then frames/s on
//                                                       every thread
//
// Options (dng, check, bench): --dng=16|packed|lj92|pass  (default 16), --no-fix (no raw corrections)
#include "dng/dng_writer.h"
#include "liblj92/lj92.h"
#include "mlv/clip.h"
#include "mlv/color.h"
#include "parallel.h"
#include "raw/rawfix.h"
#include "util/file.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

int usage() {
    std::fprintf(stderr,
                 "usage: mlvdrive_cli info  <clip.MLV> [frame]\n"
                 "       mlvdrive_cli dng   <clip.MLV> <frame> <out.dng> [--dng=16|packed|lj92|pass] [--no-fix]\n"
                 "       mlvdrive_cli check <clip.MLV> <frame> <file.dng> [--no-fix]\n"
                 "       mlvdrive_cli bench <clip.MLV> [frames] [--dng=16|packed|lj92|pass] [--no-fix]\n");
    return 2;
}

struct Args {
    std::vector<std::string> positional;
    dng::Output output = dng::Output::Uncompressed16;
    bool fix = true;
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
    raw::Fixer fixer(*clip, raw::FixSettings{});
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
    if (a.fix) fixer = std::make_unique<raw::Fixer>(*clip, raw::FixSettings{});
    dng::Options options;
    options.output = a.output;
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
        raw::Fixer fixer(*clip, raw::FixSettings{});
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
    if (a.fix) fixer = std::make_unique<raw::Fixer>(*clip, raw::FixSettings{});
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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    Args a;
    if (!parseArgs(argc, argv, &a)) return usage();
    std::string cmd = argv[1];
    int rc;
    if (cmd == "info") rc = info(a);
    else if (cmd == "dng") rc = writeDng(a);
    else if (cmd == "check") rc = check(a);
    else if (cmd == "bench") rc = bench(a);
    else return usage();
    par::shutdown();
    return rc;
}
