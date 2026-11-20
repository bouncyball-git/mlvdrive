// mlvdrive_cli: the core without a filesystem.
//
//   mlvdrive_cli info <clip.MLV> [frame]   clip details, then decodes one frame (default 0) with
//                                           raw corrections and prints the timings
#include "mlv/clip.h"
#include "mlv/color.h"
#include "parallel.h"
#include "raw/rawfix.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

int usage() {
    std::fprintf(stderr, "usage: mlvdrive_cli info <clip.MLV> [frame]\n");
    return 2;
}

int info(const std::string& path, uint32_t frame) {
    std::string error;
    auto t0 = Clock::now();
    auto clip = mlv::Clip::open(path, &error);
    if (!clip) {
        std::fprintf(stderr, "can't open %s: %s\n", path.c_str(), error.c_str());
        return 1;
    }
    double openMs = msSince(t0);

    std::printf("clip        %s (%d chunk%s, %.1f MB)\n", clip->name().c_str(), clip->chunkCount(),
                clip->chunkCount() == 1 ? "" : "s", clip->totalBytes() / 1e6);
    std::printf("camera      %.32s (model 0x%08X)\n", clip->idnt.cameraName, clip->idnt.cameraModel);
    std::printf("image       %d x %d, %d-bit, %s, black %d, white %d, CFA 0x%08X\n", clip->width(),
                clip->height(), clip->bitsPerPixel(), clip->compressionName().c_str(), clip->blackLevel(),
                clip->whiteLevel(), clip->cfaPattern());
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
    if (frame >= clip->frameCount()) {
        std::fprintf(stderr, "frame %u out of range (0..%u)\n", frame, clip->frameCount() - 1);
        return 1;
    }

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
    uint16_t lo = 0xFFFF, hi = 0;
    for (uint16_t v : image) {
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    std::printf("frame %-5u decode %.1f ms, corrections %.1f ms (%s), values %u..%u\n", frame, decodeMs, fixMs,
                fixer.active() ? "on" : "none needed", lo, hi);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    int rc = 2;
    if (std::strcmp(argv[1], "info") == 0 && (argc == 3 || argc == 4))
        rc = info(argv[2], argc == 4 ? uint32_t(std::strtoul(argv[3], nullptr, 10)) : 0);
    else
        return usage();
    par::shutdown();
    return rc;
}
