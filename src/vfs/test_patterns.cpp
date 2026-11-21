#include "vfs/test_patterns.h"

#include "dng/dng_writer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <ctime>
#include <mutex>

namespace vfs {

namespace {

constexpr int kWidth = 1920, kHeight = 1080;
constexpr int kFps = 25, kSeconds = 5;
constexpr int kFramesEach = kFps * kSeconds;
constexpr int kBlack = 4096, kWhite = 60000;  // room below black for sub-black levels
constexpr double kPi = 3.14159265358979323846;

const char* const kNames[kTestPatternCount] = {
    "SMPTE RP 219 HD colour bars", "SMPTE colour bars (SD layout)", "EBU colour bars 100%",
    "EBU colour bars 75%",         "PLUGE",                         "Grey scale, 11 steps",
    "Ramps: grey, red, green, blue", "Crosshatch 16 x 9 with circle", "Framing and safe areas",
    "Checkerboard",                "Multiburst",                    "Zone plate"};

// A colour in video levels: 0 black, 1 white (100%), R'G'B' as a monitor shows them.
struct Rgb {
    double r, g, b;
};
Rgb Grey(double v) { return {v, v, v}; }

// The seven bars in their usual order (white or grey, yellow, cyan, green, magenta, red, blue)
// and black, at `level`.
Rgb Bar(int k, double level) {
    static const int bits[8] = {7, 6, 3, 2, 5, 4, 1, 0};  // r g b as bits 4 2 1
    const int m = bits[k];
    return {m & 4 ? level : 0, m & 2 ? level : 0, m & 1 ? level : 0};
}

// The BT.709 curve undone: video level -> linear light (odd: sub-black stays below black).
double ToLinear(double v) {
    if (v < 0) return -ToLinear(-v);
    return v < 0.081 ? v / 4.5 : std::pow((v + 0.099) / 1.099, 1 / 0.45);
}

// x of segment boundaries: `units` widths laid end to end from `x0` over `width`.
int Segment(double x, double x0, double width, const double* units, int n) {
    double total = 0;
    for (int i = 0; i < n; ++i) total += units[i];
    double at = x0;
    for (int i = 0; i < n; ++i) {
        at += units[i] / total * width;
        if (x < at) return i;
    }
    return n - 1;
}

bool Line(int v, int at, int width = 2) { return v >= at - width / 2 && v < at - width / 2 + width; }

// A line on every `step` pixels, the last one inside the frame.
bool GridLine(int v, int size, int step) {
    if (v >= size - 2) return true;
    return v % step < 2;
}

// An outlined rectangle `fraction` of the frame, centred.
bool Outline(int x, int y, double fw, double fh) {
    const int x0 = int(std::lround(kWidth * (1 - fw) / 2)), x1 = kWidth - x0;
    const int y0 = int(std::lround(kHeight * (1 - fh) / 2)), y1 = kHeight - y0;
    const bool inX = x >= x0 && x < x1, inY = y >= y0 && y < y1;
    return (inY && (Line(x, x0 + 1) || Line(x, x1 - 1))) || (inX && (Line(y, y0 + 1) || Line(y, y1 - 1)));
}

Rgb Pixel(int p, int x, int y) {
    const double W = kWidth, H = kHeight;
    switch (p) {
        case 0: {  // SMPTE RP 219 (ARIB STD-B28) HD bars, its layout simplified
            const int a = kWidth / 8, barsW = kWidth - 2 * a;
            const int h1 = kHeight * 7 / 12, h2 = kHeight / 12, h3 = kHeight / 12;
            const bool left = x < a, right = x >= kWidth - a;
            const int bx = x - a;
            if (y < h1) return left || right ? Grey(0.40) : Bar(bx * 7 / barsW, 0.75);
            if (y < h1 + h2) return left ? Rgb{0, 1, 1} : right ? Rgb{0, 0, 1} : Grey(0.75);
            if (y < h1 + h2 + h3) {
                if (left) return {1, 1, 0};
                if (right) return {1, 0, 0};
                const int barW = barsW / 7;
                if (bx < barW) return Grey(0);
                return Grey(double(bx - barW) / (barsW - barW - 1));  // the Y ramp, 0 to 100%
            }
            if (left || right) return Grey(0.15);
            static const double units[] = {1.5, 2, 1, 1.0 / 3, 1.0 / 3, 1.0 / 3, 1.0 / 3, 1.0 / 3, 1};
            static const double level[] = {0, 1, 0, -0.02, 0, 0.02, 0, 0.04, 0};  // PLUGE -2 0 +2 0 +4
            return Grey(level[Segment(x, a, barsW, units, 9)]);
        }
        case 1: {  // SMPTE colour bars, SD layout (75% bars, castellations, -I white +Q, PLUGE)
            const double b = W / 7;
            const int k = std::min(6, int(x / b));
            if (y < H * 2 / 3) return Bar(k, 0.75);
            if (y < H * 3 / 4) {
                static const int cast[7] = {6, 7, 4, 7, 2, 7, 0};  // blue, black, magenta, black, cyan, black, grey
                return Bar(cast[k], 0.75);
            }
            static const double units[] = {1.25, 1.25, 1.25, 1.25, 1.0 / 3, 1.0 / 3, 1.0 / 3, 1};
            switch (Segment(x, 0, W, units, 8)) {
                case 0: return {0, 0.129, 0.298};      // -I (as commonly drawn)
                case 1: return Grey(1);
                case 2: return {0.196, 0, 0.416};      // +Q
                case 4: return Grey(-0.04);            // PLUGE: sub-black
                case 6: return Grey(0.04);
                default: return Grey(0);
            }
        }
        case 2:  // EBU bars 100/0/100/0
            return Bar(std::min(7, x * 8 / kWidth), 1.0);
        case 3: {  // EBU bars 100/0/75/0: white at 100%, the colours at 75%
            const int k = std::min(7, x * 8 / kWidth);
            return k == 0 ? Grey(1) : Bar(k, 0.75);
        }
        case 4: {  // PLUGE (after ITU-R BT.814): -2 +2 +4% bars on black; a column of 100 75 50 25%
            if (y < H / 6 || y >= H * 5 / 6) return Grey(0);
            if (x >= W * 0.25 && x < W * 0.30) return Grey(-0.02);
            if (x >= W * 0.35 && x < W * 0.40) return Grey(0.02);
            if (x >= W * 0.45 && x < W * 0.50) return Grey(0.04);
            if (x >= W * 0.62 && x < W * 0.75) {
                static const double level[4] = {1.0, 0.75, 0.5, 0.25};
                return Grey(level[std::min(3, int((y - H / 6) / (H * 2 / 3 / 4)))]);
            }
            return Grey(0);
        }
        case 5:  // grey scale: 0, 10, ... 100%
            return Grey(std::min(10, x * 11 / kWidth) / 10.0);
        case 6: {  // ramps, left to right 0 to 100%: grey, red, green, blue
            const double v = x / (W - 1);
            switch (std::min(3, y * 4 / kHeight)) {
                case 0: return Grey(v);
                case 1: return {v, 0, 0};
                case 2: return {0, v, 0};
                default: return {0, 0, v};
            }
        }
        case 7: {  // crosshatch: 16 x 9 squares of 120 pixels, a centre cross, the inscribed circle
            const bool grid = GridLine(x, kWidth, kWidth / 16) || GridLine(y, kHeight, kHeight / 9);
            const bool cross = Line(x, kWidth / 2, 4) || Line(y, kHeight / 2, 4);
            const double dx = x + 0.5 - W / 2, dy = y + 0.5 - H / 2;
            const bool circle = std::fabs(std::sqrt(dx * dx + dy * dy) - (H / 2 - 4)) < 1.0;
            return grid || cross || circle ? Grey(1) : Grey(0);
        }
        case 8: {  // framing: the frame, 93% action safe, 90% title safe, 4:3, 1.85:1, 2.39:1, centre
            if (Outline(x, y, 1, 1)) return Grey(1);
            if (Outline(x, y, 0.93, 0.93)) return Grey(0.75);           // action safe (SMPTE ST 2046-1, EBU R 95)
            if (Outline(x, y, 0.90, 0.90)) return {0.75, 0.75, 0};      // title / graphics safe
            if (Outline(x, y, 0.75, 1)) return {0, 0.75, 0.75};         // 4:3 centre cut (1440 wide)
            if (Outline(x, y, 1, (W / 1.85) / H)) return {0, 0.75, 0};   // 1.85:1
            if (Outline(x, y, 1, (W / 2.39) / H)) return {0.75, 0, 0.75};  // 2.39:1
            const bool cross = (std::abs(x - kWidth / 2) < 40 && Line(y, kHeight / 2)) ||
                               (std::abs(y - kHeight / 2) < 40 && Line(x, kWidth / 2));
            return cross ? Grey(1) : Grey(0);
        }
        case 9:  // checkerboard of 120-pixel squares
            return ((x / 120) + (y / 120)) % 2 ? Grey(1) : Grey(0);
        case 10: {  // multiburst: white and black flags, then 6 bursts from 0.067 to 0.40 cycles/pixel
            if (y < H / 6 || y >= H * 5 / 6) return Grey(0);
            static const double units[] = {1, 1, 2, 2, 2, 2, 2, 2};
            static const double freq[6] = {0.067, 0.135, 0.20, 0.27, 0.34, 0.40};
            const int s = Segment(x, 0, W, units, 8);
            if (s == 0) return Grey(1);
            if (s == 1) return Grey(0);
            const double start = W * (2 + 2 * (s - 2)) / 14;
            return Grey(0.5 + 0.4 * std::sin(2 * kPi * freq[s - 2] * (x - start)));
        }
        default: {  // zone plate: cos(k r^2), 0.5 cycles/pixel (Nyquist) at half the height
            const double dx = x + 0.5 - W / 2, dy = y + 0.5 - H / 2;
            const double k = kPi * 0.5 / (H / 2);
            return Grey(0.5 + 0.5 * std::cos(k * (dx * dx + dy * dy)));
        }
    }
}

// The pattern's image, mosaicked: RGGB, 16-bit, black and white levels as above.
std::vector<uint16_t> Render(int p) {
    std::vector<uint16_t> img(size_t(kWidth) * kHeight);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const Rgb c = Pixel(p, x, y);
            const double v = (y & 1) ? ((x & 1) ? c.b : c.g) : ((x & 1) ? c.g : c.r);
            const double raw = kBlack + ToLinear(v) * (kWhite - kBlack);
            img[size_t(y) * kWidth + x] = uint16_t(std::clamp(std::lround(raw), 0L, 65535L));
        }
    }
    return img;
}

int64_t LocalNow() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    // Days from 1970-01-01 to the date (H. Hinnant's days_from_civil), then the time of day.
    int64_t y = tm.tm_year + 1900;
    const unsigned m = unsigned(tm.tm_mon + 1), d = unsigned(tm.tm_mday);
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = era * 146097 + int64_t(doe) - 719468;
    return days * 86400 + tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
}

class TestPatternSource : public FrameSource {
public:
    explicit TestPatternSource(bool tenHours) : tcStart_(tenHours ? int64_t(10) * 3600 * kFps : 0) {
        base_.width = kWidth;
        base_.height = kHeight;
        base_.black = kBlack;
        base_.white = kWhite;
        base_.fpsNum = kFps;
        base_.fpsDen = 1;
        base_.dateTime = LocalNow();
        base_.make = "MLV Drive";
        base_.model = "Test Pattern";
        base_.reelName = "TestPatterns";
        headerSize_ = header(0).size();
    }

    uint32_t frameCount() const override { return kTestPatternCount * kFramesEach; }
    uint64_t headerSize() const override { return headerSize_; }
    uint64_t fileSize() const override { return headerSize_ + uint64_t(kWidth) * kHeight * 2; }
    uint64_t frameOffset(uint32_t frame) const override { return uint64_t(frame) * 10000000 / kFps; }

    bool renderHeader(uint32_t frame, std::vector<uint8_t>& out) const override {
        if (frame >= frameCount()) return false;
        out = header(frame);
        return true;
    }

    bool renderFrame(uint32_t frame, std::vector<uint8_t>& out, std::string* error) const override {
        if (frame >= frameCount()) {
            if (error) *error = "No frame " + std::to_string(frame);
            return false;
        }
        const int p = int(frame / kFramesEach);
        std::call_once(once_[size_t(p)], [&] { images_[size_t(p)] = Render(p); });
        const std::vector<uint16_t>& img = images_[size_t(p)];
        out = header(frame);
        const size_t at = out.size();
        out.resize(at + img.size() * 2);
        std::memcpy(out.data() + at, img.data(), img.size() * 2);  // little-endian, as the header says
        return true;
    }

private:
    std::vector<uint8_t> header(uint32_t frame) const {
        dng::GeneratedFrame g = base_;
        g.timecodeFrame = tcStart_ + frame;
        return dng::generatedHeader(g);
    }

    dng::GeneratedFrame base_;
    int64_t tcStart_;
    uint64_t headerSize_ = 0;
    // Each pattern's image, made the first time one of its frames is read.
    mutable std::array<std::once_flag, kTestPatternCount> once_;
    mutable std::array<std::vector<uint16_t>, kTestPatternCount> images_;
};

}  // namespace

const char* testPatternName(int index) {
    return index >= 0 && index < kTestPatternCount ? kNames[index] : "";
}

std::unique_ptr<FrameSource> makeTestPatterns(bool tenHours) {
    return std::make_unique<TestPatternSource>(tenHours);
}

}  // namespace vfs
