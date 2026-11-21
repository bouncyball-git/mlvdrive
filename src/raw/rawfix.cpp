#include "raw/rawfix.h"

#include "mlv/clip.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <random>
#include <thread>
#include <vector>

// SSE4.1 for the stripe correction on x86 (checked at run time; the scalar code otherwise).
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#define RAWFIX_SSE41 1
#include <smmintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#define RAWFIX_SSE41_FN
#else
#define RAWFIX_SSE41_FN __attribute__((target("sse4.1")))
#endif
#else
#define RAWFIX_SSE41 0
#endif

namespace raw {

namespace {

constexpr int kFixpOne = 65536;
constexpr int kFixpRange = 65536;

// MLV-App's llrpDetectFocusDotFixMode(): 0 none, 1 normal map, 2 crop_rec map.
int FocusDotMode(const mlv::Clip& clip) {
    switch (clip.idnt.cameraModel) {
        case 0x80000331:  // EOS M
        case 0x80000355:  // EOS M2
        case 0x80000346:  // 100D
        case 0x80000301:  // 650D
        case 0x80000326:  // 700D
            if (clip.hasRawc()) {
                int sx = clip.rawc.binning_x + clip.rawc.skipping_x;
                int sy = clip.rawc.binning_y + clip.rawc.skipping_y;
                if (clip.rawi.raw_info.height < 900 && !(sy == 5 && sx == 3)) return 2;
            }
            return 1;
        default:
            return 0;
    }
}

// --- Vertical stripes (raw2dng by a1ex, via MLV-App's llrawproc/stripes.c) ------------------

struct Stripes {
    bool needed = false;
    int coefficients[8] = {};
};

int F2H(double ev) { return std::clamp(int(kFixpRange / 2 + ev * kFixpRange / 2), 0, kFixpRange - 1); }
double H2F(int x) { return double(x - kFixpRange / 2) / (kFixpRange / 2); }

// The histograms of one band of rows (y0 to y1, even) into `hist` / `num`, with its own dither.
void StripeHistogram(const uint16_t* img, int black, int white, int width, int y0, int y1, uint32_t seed,
                     int* hist, int* num) {
    std::mt19937 noise(seed);  // dithering against quantisation spikes; seeded: same result every run

    auto add = [&](int offset, int a, int b) {
        if (std::min(a, b) < 32) return;            // too noisy
        if (std::max(a, b) > white / 1.1) return;   // too bright
        double af = a + (noise() % 1024) / 1024.0 - 0.5;
        double bf = b + (noise() % 1024) / 1024.0 - 0.5;
        double ev = std::log2(af / bf);
        // int(log2(a)) for a >= 32: the highest bit set, without floating point
        int weight = 0;
        for (unsigned v = unsigned(a); v > 1; v >>= 1) ++weight;
        hist[size_t(offset) * kFixpRange + size_t(F2H(ev))] += weight;
        num[offset] += weight;
    };

    const int groups = width / 8;
    // Green pixels only, assuming RGGB: RG rows have green at odd columns, GB rows at even ones.
    for (int y = y0; y + 1 < y1; y += 2) {
        const uint16_t* rgRow = img + size_t(y) * width;
        const uint16_t* gbRow = rgRow + width;
        for (int g = 0; g + 1 < groups; ++g) {
            const uint16_t* rg = rgRow + g * 8;
            const uint16_t* gb = gbRow + g * 8;
            int pb = rg[1] - black, pd = rg[3] - black, pf = rg[5] - black, ph = rg[7] - black;
            int pb2 = rg[9] - black, pd2 = rg[11] - black, pf2 = rg[13] - black, ph2 = rg[15] - black;
            int pc = gb[2] - black, pe = gb[4] - black, pg = gb[6] - black;
            int pa2 = gb[8] - black, pc2 = gb[10] - black, pe2 = gb[12] - black, pg2 = gb[14] - black;
            // Every column as bright as a2, interpolated so smooth gradients don't fool it.
            add(1, pa2, (pb * 1 + pb2 * 7) / 8);
            add(2, pa2, (pc * 2 + pc2 * 6) / 8);
            add(3, pa2, (pd * 3 + pd2 * 5) / 8);
            add(4, pa2, (pe * 4 + pe2 * 4) / 8);
            add(5, pa2, (pf * 5 + pf2 * 3) / 8);
            add(6, pa2, (pg * 6 + pg2 * 2) / 8);
            add(7, pa2, (ph * 7 + ph2 * 1) / 8);
        }
    }
}

Stripes FactorsFromHistogram(const std::vector<int>& hist, const int num[8], int frameSize);

Stripes DetectStripes(const uint16_t* img, int black, int white, int frameSize, int width, int height,
                      bool onePass = false) {
    if (onePass) {  // as it was: one dither sequence through the whole frame, on one thread
        std::vector<int> hist(size_t(8) * kFixpRange, 0);
        int num[8] = {};
        StripeHistogram(img, black, white, width, 0, height, 12345u, hist.data(), num);
        return FactorsFromHistogram(hist, num, frameSize);
    }
    // 16 fixed bands of rows, each with its own dither (seeded by its number), counted on up to 8
    // threads and added up: the same result whatever the thread count, every run.
    constexpr int kBands = 16;
    const int threads = std::clamp(int(std::thread::hardware_concurrency()), 1, 8);
    const int rows = (height / 2 + kBands - 1) / kBands * 2;  // even
    std::vector<std::vector<int>> hists(size_t(threads), std::vector<int>(size_t(8) * kFixpRange, 0));
    std::vector<std::array<int, 8>> nums(size_t(threads), std::array<int, 8>{});
    std::atomic<int> nextBand{0};
    auto work = [&](int t) {
        for (int b; (b = nextBand++) < kBands;) {
            const int y0 = b * rows, y1 = std::min(height, y0 + rows);
            if (y0 < y1) StripeHistogram(img, black, white, width, y0, y1, 12345u + uint32_t(b), hists[t].data(), nums[t].data());
        }
    };
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; ++t) pool.emplace_back(work, t);
    work(0);
    for (auto& t : pool) t.join();
    std::vector<int>& hist = hists[0];
    int num[8] = {};
    for (int t = 0; t < threads; ++t) {
        for (int j = 0; j < 8; ++j) num[j] += nums[t][j];
        if (t > 0)
            for (size_t i = 0; i < hist.size(); ++i) hist[i] += hists[t][i];
    }
    return FactorsFromHistogram(hist, num, frameSize);
}

Stripes FactorsFromHistogram(const std::vector<int>& hist, const int num[8], int frameSize) {
    Stripes s;
    for (int j = 0; j < 8; j++) {  // the median factor of each column rejects outliers
        if (num[j] < frameSize / 128) continue;
        int t = 0;
        for (int k = 0; k < kFixpRange; k++) {
            t += hist[size_t(j) * kFixpRange + size_t(k)];
            if (t >= num[j] / 2) {
                s.coefficients[j] = int(std::pow(2.0, H2F(k)) * kFixpOne);
                break;
            }
        }
    }
    s.coefficients[0] = kFixpOne;
    for (int j = 0; j < 8; j++) {
        double c = double(s.coefficients[j]) / kFixpOne;
        if (c < 0.998 || c > 1.002) s.needed = true;
    }
    return s;
}

#if RAWFIX_SSE41
// The brightest pixel of the 8-column groups (as the scalar loop in ApplyStripes), 8 at a time.
RAWFIX_SSE41_FN int BrightestSse41(const uint16_t* img, int width, int height) {
    const int groups = width / 8;
    __m128i m = _mm_setzero_si128();
    for (int y = 0; y < height; ++y) {
        const uint16_t* row = img + size_t(y) * width;
        for (int g = 0; g < groups; ++g)
            m = _mm_max_epu16(m, _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + g * 8)));
    }
    m = _mm_max_epu16(m, _mm_srli_si128(m, 8));
    m = _mm_max_epu16(m, _mm_srli_si128(m, 4));
    m = _mm_max_epu16(m, _mm_srli_si128(m, 2));
    return _mm_extract_epi16(m, 0);
}

// uint16_t(min(white, (v - black) * c / 65536 + black)) for 4 values (32-bit lanes).
RAWFIX_SSE41_FN inline __m128i ScaleSse41(__m128i v32, __m128i c, __m128i black32, __m128i white32) {
    const __m128i prod = _mm_mullo_epi32(_mm_sub_epi32(v32, black32), c);
    const __m128i neg = _mm_srai_epi32(prod, 31);  // -1 where negative
    const __m128i q = _mm_srai_epi32(_mm_add_epi32(prod, _mm_and_si128(neg, _mm_set1_epi32(0xFFFF))), 16);  // / 65536 toward 0
    return _mm_and_si128(_mm_min_epi32(white32, _mm_add_epi32(q, black32)), _mm_set1_epi32(0xFFFF));  // uint16_t(...)
}

// The factors applied to 8 columns at a time: the same values as the scalar loop in ApplyStripes,
// its integer arithmetic step for step (C's division rounding toward zero, the 16-bit wrap of a
// result below 0).
RAWFIX_SSE41_FN void ApplyStripesSse41(const int coefficients[8], uint16_t* img, int black, int white, int width,
                                       int height) {
    const int groups = width / 8;
    const __m128i c0 = _mm_setr_epi32(coefficients[0], coefficients[1], coefficients[2], coefficients[3]);
    const __m128i c1 = _mm_setr_epi32(coefficients[4], coefficients[5], coefficients[6], coefficients[7]);
    // Lanes whose factor is 0 (not measured) are left alone.
    const __m128i hasFactor = _mm_packs_epi32(_mm_xor_si128(_mm_cmpeq_epi32(c0, _mm_setzero_si128()), _mm_set1_epi32(-1)),
                                              _mm_xor_si128(_mm_cmpeq_epi32(c1, _mm_setzero_si128()), _mm_set1_epi32(-1)));
    const __m128i black32 = _mm_set1_epi32(black);
    const __m128i white32 = _mm_set1_epi32(white);
    const __m128i whiteBelow = _mm_set1_epi16(int16_t(uint16_t(white - 1)));  // v < white <=> min(v, white-1) == v
    const __m128i zero = _mm_setzero_si128();
    for (int y = 0; y < height; ++y) {
        uint16_t* row = img + size_t(y) * width;
        for (int g = 0; g < groups; ++g) {
            uint16_t* p = row + g * 8;
            if (!(p[0] > black + 64)) continue;  // the group's first pixel: not in the darkest tones
            const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
            const __m128i lo = ScaleSse41(_mm_cvtepu16_epi32(v), c0, black32, white32);
            const __m128i hi = ScaleSse41(_mm_cvtepu16_epi32(_mm_srli_si128(v, 8)), c1, black32, white32);
            const __m128i scaled = _mm_packus_epi32(lo, hi);  // exact: both halves are 0..65535
            __m128i take = _mm_andnot_si128(_mm_cmpeq_epi16(v, zero), hasFactor);           // v != 0
            take = _mm_and_si128(take, _mm_cmpeq_epi16(_mm_min_epu16(v, whiteBelow), v));  // v < white
            _mm_storeu_si128(reinterpret_cast<__m128i*>(p), _mm_blendv_epi8(v, scaled, take));
        }
    }
}

bool HasSse41() {
    static const bool has = [] {
#if defined(_MSC_VER)
        int info[4];
        __cpuid(info, 1);
        return (info[2] & (1 << 19)) != 0;
#else
        return __builtin_cpu_supports("sse4.1") != 0;
#endif
    }();
    return has;
}
#endif

std::atomic<bool> g_simd{true};

void ApplyStripes(const Stripes& s, uint16_t* img, int black, int whiteLevel, int width, int height) {
#if RAWFIX_SSE41
    // SSE4.1 (every x64 CPU Windows 11 runs on): the same values, ~4x faster.
    if (g_simd.load(std::memory_order_relaxed) && HasSse41() && whiteLevel * 2 / 3 >= 1) {
        const int white = std::max(whiteLevel * 2 / 3, BrightestSse41(img, width, height));
        ApplyStripesSse41(s.coefficients, img, black, white, width, height);
        return;
    }
#endif
    // The exact white level: a gross under-estimate, raised to the brightest pixel.
    int white = whiteLevel * 2 / 3;
    const int groups = width / 8;
    for (int y = 0; y < height; ++y) {
        const uint16_t* row = img + size_t(y) * width;
        for (int i = 0; i < groups * 8; ++i) white = std::max(white, int(row[i]));
    }
    for (int y = 0; y < height; ++y) {
        uint16_t* row = img + size_t(y) * width;
        for (int g = 0; g < groups; ++g) {
            uint16_t* p = row + g * 8;
            const int pa = p[0];
            for (int j = 0; j < 8; ++j) {
                const int v = p[j];
                const int c = s.coefficients[j];
                // As raw2dng: never past the white level, and not in the darkest tones (round-off).
                if (c && v && v < white && pa > black + 64) {
                    p[j] = uint16_t(std::min(white, ((v - black) * c / kFixpOne) + black));
                }
            }
        }
    }
}

}  // namespace

void setSimd(bool on) {
    g_simd.store(on, std::memory_order_relaxed);
}

void stripeFactors(const mlv::Clip& clip, const uint16_t* image, bool onePass, double out[8]) {
    const int w = clip.width(), h = clip.height();
    const int shift = std::max(0, 14 - clip.bitsPerPixel());
    std::vector<uint16_t> img14(image, image + size_t(w) * h);
    for (auto& v : img14) v = uint16_t(v << shift);
    int frameSize = clip.rawi.raw_info.width * clip.rawi.raw_info.height * 14 / 8;
    if (frameSize <= 0) frameSize = w * h * 14 / 8;
    Stripes s = DetectStripes(img14.data(), clip.blackLevel() << shift, clip.whiteLevel() << shift, frameSize, w, h, onePass);
    for (int j = 0; j < 8; ++j) out[j] = double(s.coefficients[j]) / kFixpOne;
}

bool cameraHasFocusPixels(uint32_t model) {
    switch (model) {
        case 0x80000331: case 0x80000355: case 0x80000346: case 0x80000301: case 0x80000326: return true;
        default: return false;
    }
}

Fixer::Fixer(const mlv::Clip& clip, const FixSettings& settings) : clip_(clip), settings_(settings) {
    focusMap_.type = PIX_FOCUS;
    badMap_.type = PIX_BAD;
    if (!settings.enabled) return;

    stripes_ = settings.verticalStripes < 0 ? (clip.idnt.cameraModel == 0x80000285 ? 1 : 0) : settings.verticalStripes;
    const int dotMode = FocusDotMode(clip);
    focus_ = settings.focusPixels < 0 ? (dotMode != 0 ? 1 : 0) : settings.focusPixels;
    cropRec_ = dotMode == 2 ? 1 : (focus_ == 2 ? 1 : 0);
    active_ = stripes_ || focus_ || settings.badPixels || settings.chromaSmooth;

    const int shift = std::max(0, 14 - clip.bitsPerPixel());
    black14_ = clip.blackLevel() << shift;
    white14_ = clip.whiteLevel() << shift;
    if (active_) {
        raw2ev_ = get_raw2ev(black14_);
        ev2raw_ = get_ev2raw(black14_);
    }
}

Fixer::~Fixer() {
    free_luts(raw2ev_, ev2raw_);
    free_pixel_maps(&focusMap_, &badMap_);
}

void Fixer::apply(uint16_t* image, uint32_t frameIndex) {
    if (!active_) return;
    if (!primed_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!primed_.load(std::memory_order_relaxed)) {
            // What the clip needs is always measured on frame 0, whichever frame comes first: a
            // frame's output must not depend on the order frames are asked for (on the drive, any).
            bool primed = false;
            if (frameIndex != 0) {
                std::vector<uint16_t> first(size_t(clip_.width()) * clip_.height());
                std::vector<uint8_t> scratch;
                if (clip_.decodeFrame(0, first.data(), scratch, nullptr)) {
                    run(first.data(), 0, true);
                    primed = true;
                }
            }
            run(image, frameIndex, !primed);
            primed_.store(true, std::memory_order_release);
            return;
        }
    }
    run(image, frameIndex, false);
}

void Fixer::run(uint16_t* image, uint32_t frameIndex, bool first) {
    const int w = clip_.width(), h = clip_.height();
    const size_t pixels = size_t(w) * h;
    const int bpp = clip_.bitsPerPixel();
    const int shift = std::max(0, 14 - bpp);
    const mlv::FrameIndex& f = clip_.frame(frameIndex);

    // 10/12-bit data is worked on at 14 bits, as MLV-App does.
    if (shift) {
        for (size_t i = 0; i < pixels; ++i) image[i] = uint16_t(image[i] << shift);
    }

    if (stripes_) {
        int frameSize = clip_.rawi.raw_info.width * clip_.rawi.raw_info.height * 14 / 8;
        if (frameSize <= 0) frameSize = int(pixels * 14 / 8);
        if (stripes_ == 2) {
            Stripes s = DetectStripes(image, black14_, white14_, frameSize, w, h);  // forced: every frame
            if (s.needed) ApplyStripes(s, image, black14_, white14_, w, h);
        } else {
            if (first) {
                Stripes s = DetectStripes(image, black14_, white14_, frameSize, w, h);
                stripesNeeded_ = s.needed;
                std::copy(std::begin(s.coefficients), std::end(s.coefficients), stripeCoefficients_);
            }
            if (stripesNeeded_) {
                Stripes s;
                s.needed = true;
                std::copy(std::begin(stripeCoefficients_), std::end(stripeCoefficients_), s.coefficients);
                ApplyStripes(s, image, black14_, white14_, w, h);
            }
        }
    }

    if (focus_ && focusStatus_ < 3) {
        const int unified = clip_.compression() == mlv::Compression::Lossless ? 5 : 0;
        fix_focus_pixels(&focusMap_, &focusStatus_, image, clip_.idnt.cameraModel, uint16_t(w), uint16_t(h),
                         f.panPosX, f.panPosY, clip_.rawi.raw_info.width, clip_.rawi.raw_info.height, cropRec_,
                         unified, settings_.focusMethod, 0, raw2ev_, ev2raw_);
    }

    if (settings_.badPixels && badStatus_ < 3) {
        fix_bad_pixels(&badMap_, &badStatus_, image, clip_.idnt.cameraModel, uint16_t(w), uint16_t(h), f.panPosX,
                       f.panPosY, clip_.rawi.raw_info.width, clip_.rawi.raw_info.height, black14_,
                       settings_.badPixels, settings_.badPixelSearch, settings_.badPixelMethod, 0, raw2ev_, ev2raw_);
    }

    if (settings_.chromaSmooth == 2 || settings_.chromaSmooth == 3 || settings_.chromaSmooth == 5) {
        chroma_smooth(settings_.chromaSmooth, image, w, h, black14_, white14_, raw2ev_, ev2raw_);
    }

    if (shift) {  // back to the clip's bit depth, rounded
        const uint32_t round = 1u << (shift - 1);
        const uint32_t top = (1u << bpp) - 1;  // 16383 + round would carry past the bit depth
        for (size_t i = 0; i < pixels; ++i) image[i] = uint16_t(std::min(top, (image[i] + round) >> shift));
    }
}

}  // namespace raw
