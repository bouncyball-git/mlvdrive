// The tag set, white balance and timecode follow MLV-App's src/dng/dng.c:
//   Copyright (C) 2014 David Milligan (MLVFS); updated and modified by bouncyball (2016-2017).
//   GNU General Public License, version 2 or (at your option) any later version.
#include "dng/dng_writer.h"

#include "camid/camera_id.h"
#include "liblj92/lj92.h"
#include "mlv/clip.h"
#include "raw/rawfix.h"
#include "util/file.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dng {

namespace {

#ifndef MLVDRIVE_VERSION
#define MLVDRIVE_VERSION "dev"
#endif
constexpr char kSoftware[] = "MLV Drive " MLVDRIVE_VERSION;

enum Type : uint16_t { kByte = 1, kAscii = 2, kShort = 3, kLong = 4, kRational = 5, kUndefined = 7, kSRational = 10 };

// A TIFF directory under construction: entries with their values, laid out by write().
class Directory {
public:
    void add(uint16_t tag, Type type, uint32_t count, const void* data, size_t bytes) {
        Entry e{tag, type, count, std::vector<uint8_t>(static_cast<const uint8_t*>(data),
                                                       static_cast<const uint8_t*>(data) + bytes)};
        entries_.push_back(std::move(e));
    }
    void shortValue(uint16_t tag, uint16_t v) { add(tag, kShort, 1, &v, 2); }
    void shorts(uint16_t tag, std::initializer_list<uint16_t> v) { add(tag, kShort, uint32_t(v.size()), v.begin(), v.size() * 2); }
    void longValue(uint16_t tag, uint32_t v) { add(tag, kLong, 1, &v, 4); }
    void longs(uint16_t tag, const int32_t* v, size_t n) { add(tag, kLong, uint32_t(n), v, n * 4); }
    void bytes(uint16_t tag, const uint8_t* v, size_t n) { add(tag, kByte, uint32_t(n), v, n); }
    void ascii(uint16_t tag, const std::string& s) { add(tag, kAscii, uint32_t(s.size() + 1), s.c_str(), s.size() + 1); }
    void rational(uint16_t tag, uint32_t num, uint32_t den) {
        uint32_t v[2] = {num, den};
        add(tag, kRational, 1, v, 8);
    }
    void srational(uint16_t tag, int32_t num, int32_t den) {
        int32_t v[2] = {num, den};
        add(tag, kSRational, 1, v, 8);
    }
    // n rationals as num, den pairs (2n values).
    void rationals(uint16_t tag, Type type, const int32_t* pairs, size_t n) { add(tag, type, uint32_t(n), pairs, n * 8); }

    // Replaces a LONG value (an offset only known once the layout is done).
    void setLong(uint16_t tag, uint32_t v) {
        for (auto& e : entries_)
            if (e.tag == tag) std::memcpy(e.data.data(), &v, 4);
    }

    size_t tableSize() const { return 2 + entries_.size() * 12 + 4; }
    size_t dataSize() const {
        size_t n = 0;
        for (const auto& e : entries_)
            if (e.data.size() > 4) n += (e.data.size() + 1) & ~size_t(1);
        return n;
    }
    // Writes the table at `tableAt` and its out-of-line values from `dataAt` on.
    void write(std::vector<uint8_t>& out, size_t tableAt, size_t dataAt) {
        std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) { return a.tag < b.tag; });
        uint8_t* p = out.data() + tableAt;
        uint16_t count = uint16_t(entries_.size());
        std::memcpy(p, &count, 2);
        p += 2;
        for (const auto& e : entries_) {
            std::memcpy(p, &e.tag, 2);
            std::memcpy(p + 2, &e.type, 2);
            std::memcpy(p + 4, &e.count, 4);
            if (e.data.size() <= 4) {
                std::memset(p + 8, 0, 4);
                std::memcpy(p + 8, e.data.data(), e.data.size());
            } else {
                uint32_t offset = uint32_t(dataAt);
                std::memcpy(p + 8, &offset, 4);
                std::memcpy(out.data() + dataAt, e.data.data(), e.data.size());
                dataAt += (e.data.size() + 1) & ~size_t(1);
            }
            p += 12;
        }
        std::memset(p, 0, 4);  // no next IFD
    }

private:
    struct Entry {
        uint16_t tag;
        Type type;
        uint32_t count;
        std::vector<uint8_t> data;
    };
    std::vector<Entry> entries_;
};

std::string FixedText(const char* data, size_t size) {
    std::string s(data, strnlen(data, size));
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// Days since 1970-01-01 for a civil date, and back (H. Hinnant's algorithms).
int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}
void CivilFromDays(int64_t z, int* y, int* m, int* d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = unsigned(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = int(doy - (153 * mp + 2) / 5 + 1);
    *m = int(mp < 10 ? mp + 3 : mp - 9);
    *y = int(int64_t(yoe) + era * 400 + (*m <= 2));
}

// Seconds since 1970 (local camera time) when the frame was recorded; < 0 without RTCI.
int64_t FrameTime(const mlv::Clip& clip, uint32_t frameIndex) {
    const auto& r = clip.rtci;
    if (r.type[0] == 0) return -1;
    int64_t days = DaysFromCivil(1900 + r.tm_year, unsigned(r.tm_mon + 1), r.tm_mday);
    int64_t seconds = days * 86400 + r.tm_hour * 3600 + r.tm_min * 60 + r.tm_sec;
    const uint64_t ts = clip.frame(frameIndex).timestamp;
    if (ts > r.timestamp) seconds += int64_t((ts - r.timestamp) / 1000000);
    return seconds;
}

std::string DateTimeText(int64_t seconds) {
    if (seconds < 0) return "0000:00:00 00:00:00";
    int y, m, d;
    CivilFromDays(seconds / 86400, &y, &m, &d);
    int64_t s = seconds % 86400;
    char text[32];
    std::snprintf(text, sizeof text, "%04d:%02d:%02d %02d:%02d:%02d", y, m, d, int(s / 3600), int(s / 60 % 60), int(s % 60));
    return text;
}

uint8_t Bcd(int v) { return uint8_t(((v / 10) << 4) | (v % 10)); }

// SMPTE 12M timecode as CinemaDNG's TimeCodes tag stores it (MLV-App's add_timecode()).
void TimecodeBytes(double fps, int64_t frame, uint8_t out[8]) {
    std::memset(out, 0, 8);
    const double base = fps > 1 ? std::round(fps) : fps;
    const double time = base > 0 ? double(frame) / base : 0;
    int hours = int(std::floor(time / 3600)) % 24;
    int minutes = int(std::floor(time / 60)) % 60;
    int seconds = int(std::floor(time)) % 60;
    int frames = fps > 1 ? int(frame % int64_t(std::round(fps))) : 0;
    out[0] = Bcd(frames) & 0x3F;
    out[1] = Bcd(seconds) & 0x7F;
    out[2] = Bcd(minutes) & 0x7F;
    out[3] = Bcd(hours) & 0x3F;
}

bool MatrixValid(const int32_t* m) {
    bool any = false;
    for (int i = 0; i < 9; i++) {
        if (m[i * 2 + 1] == 0) return false;
        any = any || m[i * 2] != 0;
    }
    return any;
}

// Rationals with a fixed denominator from doubles.
void ToRationals(const double* v, size_t n, int32_t den, int32_t* out) {
    for (size_t i = 0; i < n; ++i) {
        out[i * 2] = int32_t(std::lround(v[i] * den));
        out[i * 2 + 1] = den;
    }
}

}  // namespace

std::vector<uint8_t> generatedHeader(const GeneratedFrame& g) {
    Directory ifd0, exif;
    const double fps = g.fpsDen ? double(g.fpsNum) / g.fpsDen : 0;
    uint8_t timecode[8];
    TimecodeBytes(fps, g.timecodeFrame, timecode);
    const std::string dateTime = DateTimeText(g.dateTime);
    const uint8_t cfaBytes[4] = {0, 1, 1, 2};  // RGGB
    const uint8_t dngVersion[4] = {1, 4, 0, 0};

    // Camera RGB is linear Rec.709: ColorMatrix (XYZ -> camera) = XYZ -> linear sRGB (D65), and
    // ForwardMatrix (white-balanced camera -> XYZ D50) = linear sRGB -> XYZ, Bradford-adapted.
    const double toCamera[9] = {3.2404542, -1.5371385, -0.4985314, -0.9692660, 1.8760108,
                                0.0415560, 0.0556434,  -0.2040259, 1.0572252};
    const double forward[9] = {0.4360747, 0.3850649, 0.1430804, 0.2225045, 0.7168786,
                               0.0606169, 0.0139322, 0.0971045, 0.7141733};
    int32_t colorMatrix[18], forwardMatrix[18];
    ToRationals(toCamera, 9, 10000000, colorMatrix);
    ToRationals(forward, 9, 10000000, forwardMatrix);
    const int32_t neutral[6] = {1, 1, 1, 1, 1, 1};
    const int32_t frameRate[2] = {g.fpsNum, g.fpsDen};
    const int32_t defaultScale[4] = {1, 1, 1, 1};

    ifd0.longValue(254, 0);                               // NewSubFileType: main image
    ifd0.longValue(256, uint32_t(g.width));               // ImageWidth
    ifd0.longValue(257, uint32_t(g.height));              // ImageLength
    ifd0.shortValue(258, 16);                             // BitsPerSample
    ifd0.shortValue(259, 1);                              // Compression: none
    ifd0.shortValue(262, 32803);                          // PhotometricInterpretation: CFA
    ifd0.ascii(271, g.make);                              // Make
    ifd0.ascii(272, g.model);                             // Model
    ifd0.longValue(273, 0);                               // StripOffsets (set below)
    ifd0.shortValue(274, 1);                              // Orientation
    ifd0.shortValue(277, 1);                              // SamplesPerPixel
    ifd0.longValue(278, uint32_t(g.height));              // RowsPerStrip
    ifd0.longValue(279, uint32_t(uint64_t(g.width) * g.height * 2));  // StripByteCounts
    ifd0.shortValue(284, 1);                              // PlanarConfiguration
    ifd0.ascii(305, kSoftware);                           // Software
    ifd0.ascii(306, dateTime);                            // DateTime
    ifd0.shorts(33421, {2, 2});                           // CFARepeatPatternDim
    ifd0.bytes(33422, cfaBytes, 4);                       // CFAPattern
    ifd0.longValue(34665, 0);                             // ExifIFD (set below)
    ifd0.bytes(50706, dngVersion, 4);                     // DNGVersion 1.4
    ifd0.ascii(50708, g.make + " " + g.model);            // UniqueCameraModel
    ifd0.longValue(50714, uint32_t(g.black));             // BlackLevel
    ifd0.longValue(50717, uint32_t(g.white));             // WhiteLevel
    ifd0.rationals(50718, kRational, defaultScale, 2);    // DefaultScale
    ifd0.shorts(50719, {0, 0});                           // DefaultCropOrigin
    ifd0.shorts(50720, {uint16_t(g.width), uint16_t(g.height)});  // DefaultCropSize
    ifd0.rationals(50721, kSRational, colorMatrix, 9);    // ColorMatrix1
    ifd0.rationals(50728, kRational, neutral, 3);         // AsShotNeutral
    ifd0.srational(50730, 0, 1);                          // BaselineExposure
    ifd0.shortValue(50778, 21);                           // CalibrationIlluminant1: D65
    ifd0.rationals(50964, kSRational, forwardMatrix, 9);  // ForwardMatrix1
    ifd0.bytes(51043, timecode, 8);                       // TimeCodes
    ifd0.rationals(51044, kSRational, frameRate, 1);      // FrameRate
    ifd0.ascii(51081, g.reelName);                        // ReelName
    ifd0.srational(51109, 0, 1);                          // BaselineExposureOffset
    const uint8_t exifVersion[4] = {'0', '2', '3', '0'};
    exif.add(36864, kUndefined, 4, exifVersion, 4);       // ExifVersion
    exif.ascii(36867, dateTime);                          // DateTimeOriginal

    const size_t ifd0At = 8;
    const size_t exifAt = ifd0At + ifd0.tableSize();
    const size_t dataAt = exifAt + exif.tableSize();
    const size_t exifDataAt = dataAt + ifd0.dataSize();
    const size_t end = (exifDataAt + exif.dataSize() + 15) & ~size_t(15);
    ifd0.setLong(34665, uint32_t(exifAt));
    ifd0.setLong(273, uint32_t(end));
    std::vector<uint8_t> out(end, 0);
    const uint8_t tiff[8] = {'I', 'I', 42, 0, 8, 0, 0, 0};
    std::memcpy(out.data(), tiff, 8);
    ifd0.write(out, ifd0At, dataAt);
    exif.write(out, exifAt, exifDataAt);
    return out;
}

size_t packBitsBigEndian(const uint16_t* in, int width, int height, int bits, std::vector<uint8_t>& out) {
    const size_t rowBytes = (size_t(width) * bits + 7) / 8;
    out.resize(rowBytes * height);
    const uint32_t mask = bits >= 16 ? 0xFFFFu : ((1u << bits) - 1);
    for (int y = 0; y < height; ++y) {
        const uint16_t* src = in + size_t(y) * width;
        uint8_t* dst = out.data() + size_t(y) * rowBytes;
        uint64_t acc = 0;
        int have = 0;
        for (int x = 0; x < width; ++x) {
            acc = (acc << bits) | (src[x] & mask);
            have += bits;
            while (have >= 8) {
                have -= 8;
                *dst++ = uint8_t(acc >> have);
            }
        }
        if (have > 0) *dst++ = uint8_t(acc << (8 - have));  // the row's last byte, padded
    }
    return out.size();
}

Writer::Writer(const mlv::Clip& clip, const Options& options, raw::Fixer* fixer)
    : clip_(clip), options_(options), fixer_(fixer), output_(options.output) {
    // Pass-through copies the recorded values; a clip with a CURV block stores them encoded,
    // and a JPEG 2000 clip can't go into a DNG as it is.
    if (output_ == Output::PassThrough &&
        (!clip.lineariseLut.empty() || clip.compression() == mlv::Compression::Jpeg2000)) {
        output_ = Output::Lossless;
    }
    const int w = clip.width(), h = clip.height();
    if (output_ == Output::Uncompressed16) imageSize_ = uint64_t(w) * h * 2;
    if (output_ == Output::Uncompressed) imageSize_ = (uint64_t(w) * clip.bitsPerPixel() + 7) / 8 * h;
    // Every value that changes from frame to frame (date, timecode, strip size) has a fixed size,
    // so frame 0's header has the size of all of them.
    if (clip.frameCount() > 0) {
        std::vector<uint8_t> header;
        buildHeader(0, size_t(imageSize_), false, 16, header);
        headerSize_ = header.size();
    }
}

void Writer::buildHeader(uint32_t frameIndex, size_t imageBytes, bool compressed, int bitsPerSample,
                         std::vector<uint8_t>& out) const {
    const mlv::Clip& c = clip_;
    const mlv::RawInfo& ri = c.rawi.raw_info;
    const camera_id_t* camid = camidGet(c.idnt.cameraModel);

    Directory ifd0, exif;

    // --- Names ---
    std::string model = FixedText(c.idnt.cameraName, sizeof c.idnt.cameraName);
    if (model.empty()) model = "Canon";
    std::string make = model.substr(0, model.find(' '));
    const char* unique = camid->cameraName[UNIQ] ? camid->cameraName[UNIQ] : model.c_str();

    // --- Geometry: the recorded frame is the active area unless it holds the optical black ---
    int32_t active[4] = {ri.active_area.y1, ri.active_area.x1, ri.active_area.y2, ri.active_area.x2};
    int cropX = ri.jpeg.x, cropY = ri.jpeg.y;
    if (c.width() < ri.active_area.x2 || c.height() < ri.active_area.y2) {
        active[0] = 0;
        active[1] = 0;
        active[2] = c.height();
        active[3] = c.width();
        cropX = 0;
        cropY = 0;
    }
    const int cropW = active[3] - active[1], cropH = active[2] - active[0];

    // --- Aspect ratio and focal plane resolution ---
    int32_t focalX[2] = {camid->focal_resolution_x[0], camid->focal_resolution_x[1]};
    int32_t focalY[2] = {camid->focal_resolution_y[0], camid->focal_resolution_y[1]};
    int stretchNum = 1, stretchDen = 1;
    c.verticalStretch(&stretchNum, &stretchDen);
    if (c.hasRawc()) {
        int sx = c.rawc.binning_x + c.rawc.skipping_x, sy = c.rawc.binning_y + c.rawc.skipping_y;
        if (sx > 0) focalX[1] *= sx;
        if (sy > 0) focalY[1] *= sy;
    } else {
        double rawW = ri.active_area.x2 - ri.active_area.x1, rawH = ri.active_area.y2 - ri.active_area.y1;
        if (rawH > 0 && rawW / rawH > 2.0 && rawH <= 720) {
            focalX[1] *= 3;
            focalY[1] *= 5;
        } else if (rawW < 2000) {
            focalX[1] *= 3;
            focalY[1] *= 3;
        }
    }
    if (options_.stretchNum > 0 && options_.stretchDen > 0) {
        stretchNum = options_.stretchNum;
        stretchDen = options_.stretchDen;
    }
    const int32_t hNum = options_.hStretchNum > 0 ? options_.hStretchNum : 1;
    const int32_t hDen = options_.hStretchDen > 0 ? options_.hStretchDen : 1;
    const int32_t defaultScale[4] = {hNum, hDen, stretchNum, stretchDen};

    // --- Frame rate ---
    int32_t frameRate[2];
    frameRateRational(frameRate);
    const double fps = double(frameRate[0]) / frameRate[1];

    // --- White balance ---
    color::Vec3 mul = options_.customWhiteBalance ? options_.wbMultipliers : color::asShotMultipliers(c);
    double neutral[3] = {1 / mul[0], 1 / mul[1], 1 / mul[2]};
    int32_t asShot[6];
    ToRationals(neutral, 3, 1000000, asShot);

    int32_t baseline[2] = {ri.exposure_bias[0], ri.exposure_bias[1]};
    if (baseline[1] == 0) {
        baseline[0] = 0;
        baseline[1] = 1;
    }

    // --- Timecode ---
    const int64_t when = FrameTime(c, frameIndex);
    uint8_t timecode[8];
    TimecodeBytes(fps, timecodeFrame(frameIndex), timecode);

    const uint32_t cfa = c.cfaPattern();
    const uint8_t cfaBytes[4] = {uint8_t(cfa), uint8_t(cfa >> 8), uint8_t(cfa >> 16), uint8_t(cfa >> 24)};
    const uint8_t dngVersion[4] = {1, 4, 0, 0};
    const std::string dateTime = DateTimeText(when);

    ifd0.longValue(254, 0);                               // NewSubFileType: main image
    ifd0.longValue(256, uint32_t(c.width()));             // ImageWidth
    ifd0.longValue(257, uint32_t(c.height()));            // ImageLength
    ifd0.shortValue(258, uint16_t(bitsPerSample));        // BitsPerSample
    ifd0.shortValue(259, compressed ? 7 : 1);             // Compression: lossless JPEG or none
    ifd0.shortValue(262, 32803);                          // PhotometricInterpretation: CFA
    ifd0.shortValue(266, 1);                              // FillOrder
    ifd0.ascii(271, make);                                // Make
    ifd0.ascii(272, model);                               // Model
    ifd0.longValue(273, 0);                               // StripOffsets (set below)
    ifd0.shortValue(274, 1);                              // Orientation
    ifd0.shortValue(277, 1);                              // SamplesPerPixel
    ifd0.longValue(278, uint32_t(c.height()));            // RowsPerStrip
    ifd0.longValue(279, uint32_t(imageBytes));            // StripByteCounts
    ifd0.shortValue(284, 1);                              // PlanarConfiguration
    ifd0.ascii(305, kSoftware);                           // Software
    ifd0.ascii(306, dateTime);                            // DateTime
    ifd0.shorts(33421, {2, 2});                           // CFARepeatPatternDim
    ifd0.bytes(33422, cfaBytes, 4);                       // CFAPattern
    ifd0.longValue(34665, 0);                             // ExifIFD (set below)
    ifd0.bytes(50706, dngVersion, 4);                     // DNGVersion 1.4
    ifd0.ascii(50708, unique);                            // UniqueCameraModel
    ifd0.longValue(50714, uint32_t(c.blackLevel()));      // BlackLevel
    ifd0.longValue(50717, uint32_t(c.whiteLevel()));      // WhiteLevel
    ifd0.rationals(50718, kRational, defaultScale, 2);    // DefaultScale
    ifd0.shorts(50719, {uint16_t(cropX), uint16_t(cropY)});  // DefaultCropOrigin
    ifd0.shorts(50720, {uint16_t(cropW), uint16_t(cropH)});  // DefaultCropSize
    if (MatrixValid(camid->ColorMatrix1)) ifd0.rationals(50721, kSRational, camid->ColorMatrix1, 9);
    if (MatrixValid(camid->ColorMatrix2)) ifd0.rationals(50722, kSRational, camid->ColorMatrix2, 9);
    ifd0.rationals(50728, kRational, asShot, 3);          // AsShotNeutral
    ifd0.rationals(50730, kSRational, baseline, 1);       // BaselineExposure
    std::string serial = FixedText(c.idnt.cameraSerial, sizeof c.idnt.cameraSerial);
    if (!serial.empty()) ifd0.ascii(50735, serial);       // CameraSerialNumber
    ifd0.shortValue(50778, 17);                           // CalibrationIlluminant1: standard light A
    ifd0.shortValue(50779, 21);                           // CalibrationIlluminant2: D65
    ifd0.longs(50829, active, 4);                         // ActiveArea
    if (MatrixValid(camid->ForwardMatrix1)) ifd0.rationals(50964, kSRational, camid->ForwardMatrix1, 9);
    if (MatrixValid(camid->ForwardMatrix2)) ifd0.rationals(50965, kSRational, camid->ForwardMatrix2, 9);
    ifd0.bytes(51043, timecode, 8);                       // TimeCodes
    ifd0.rationals(51044, kSRational, frameRate, 1);      // FrameRate
    ifd0.ascii(51081, c.name());                          // ReelName
    ifd0.srational(51109, 0, 1);                          // BaselineExposureOffset

    // --- EXIF ---
    const uint64_t shutter = c.expo.shutterValue;
    if (shutter > 0 && shutter < 0x7FFFFFFF) exif.rational(33434, uint32_t(shutter), 1000000);  // ExposureTime
    if (c.lens.aperture) exif.rational(33437, c.lens.aperture, 100);  // FNumber
    if (c.expo.isoValue) {
        exif.shortValue(34855, uint16_t(std::min<uint32_t>(c.expo.isoValue, 65535)));  // ISOSpeedRatings
        exif.shortValue(34864, 3);                                                     // SensitivityType: ISO speed
    }
    const uint8_t exifVersion[4] = {'0', '2', '3', '0'};
    exif.add(36864, kUndefined, 4, exifVersion, 4);  // ExifVersion
    exif.ascii(36867, dateTime);                     // DateTimeOriginal
    if (c.lens.focalDist) {
        if (c.lens.focalDist == 65535) exif.rational(37382, 0xFFFFFFFFu, 1);  // SubjectDistance: infinity
        else exif.rational(37382, c.lens.focalDist, 1000);                    // mm -> m
    }
    if (c.lens.focalLength) exif.rational(37386, c.lens.focalLength, 1);  // FocalLength
    if (focalX[1] && focalY[1]) {
        exif.rationals(41486, kRational, focalX, 1);  // FocalPlaneXResolution
        exif.rationals(41487, kRational, focalY, 1);  // FocalPlaneYResolution
        exif.shortValue(41488, uint16_t(camid->focal_unit));
    }
    std::string lens = c.elnsName.empty() ? FixedText(c.lens.lensName, sizeof c.lens.lensName) : c.elnsName;
    if (!lens.empty()) exif.ascii(42036, lens);  // LensModel

    // --- Layout: header, IFD0, EXIF IFD, their values, then the image ---
    const size_t ifd0At = 8;
    const size_t exifAt = ifd0At + ifd0.tableSize();
    const size_t dataAt = exifAt + exif.tableSize();
    const size_t exifDataAt = dataAt + ifd0.dataSize();
    const size_t end = (exifDataAt + exif.dataSize() + 15) & ~size_t(15);
    ifd0.setLong(34665, uint32_t(exifAt));
    ifd0.setLong(273, uint32_t(end));

    out.assign(end, 0);
    const uint8_t tiff[8] = {'I', 'I', 42, 0, 8, 0, 0, 0};
    std::memcpy(out.data(), tiff, 8);
    ifd0.write(out, ifd0At, dataAt);
    exif.write(out, exifAt, exifDataAt);
}

bool Writer::encodeImage(uint32_t frameIndex, Scratch& s, const uint8_t** image, size_t* imageBytes,
                         bool* compressed, std::string* error) const {
    const mlv::Clip& c = clip_;
    const int w = c.width(), h = c.height(), bpp = c.bitsPerPixel();
    *compressed = false;

    if (output_ == Output::PassThrough) {
        if (!c.readFrameData(frameIndex, s.data, error)) return false;
        if (c.compression() == mlv::Compression::Lossless) {
            *image = s.data.data();
            *imageBytes = s.data.size();
            *compressed = true;
            return true;
        }
        // MLV's packing is a big-endian bit stream in little-endian words: swapping each
        // word's bytes gives DNG's byte order.
        const size_t bytes = size_t(w) * h * bpp / 8;
        if (s.data.size() < bytes) {
            if (error) *error = "Frame " + std::to_string(frameIndex) + " is too short";
            return false;
        }
        s.image.resize(bytes);
        for (size_t i = 0; i + 1 < bytes; i += 2) {
            s.image[i] = s.data[i + 1];
            s.image[i + 1] = s.data[i];
        }
        *image = s.image.data();
        *imageBytes = bytes;
        return true;
    }

    s.pixels.resize(size_t(w) * h);
    if (!c.decodeFrame(frameIndex, s.pixels.data(), s.data, error)) return false;
    if (fixer_) fixer_->apply(s.pixels.data(), frameIndex);
    if (output_ == Output::Lossless) {
        // As MLV-App: two rows per JPEG row (width x 2, height / 2), one component.
        uint8_t* encoded = nullptr;
        int encodedSize = 0;
        int ret = lj92_encode(s.pixels.data(), w * 2, h / 2, bpp, w * 2 * (h / 2), 0, nullptr, 0, &encoded,
                              &encodedSize);
        if (ret != LJ92_ERROR_NONE || !encoded) {
            std::free(encoded);
            if (error) *error = "Lossless encoder failed at frame " + std::to_string(frameIndex);
            return false;
        }
        s.image.assign(encoded, encoded + encodedSize);
        std::free(encoded);
        *compressed = true;
    } else {
        packBitsBigEndian(s.pixels.data(), w, h, bpp, s.image);
    }
    *image = s.image.data();
    *imageBytes = s.image.size();
    return true;
}

bool Writer::renderFrame(uint32_t frameIndex, std::vector<uint8_t>& out, Scratch& s, std::string* error) const {
    const mlv::Clip& c = clip_;
    if (frameIndex >= c.frameCount()) {
        if (error) *error = "No frame " + std::to_string(frameIndex);
        return false;
    }
    if (output_ == Output::Uncompressed16) {
        // Decoded and fixed in place, right after the header (which ends on 16 bytes): no copy.
        buildHeader(frameIndex, size_t(imageSize_), false, 16, s.header);
        out.resize(s.header.size() + size_t(imageSize_));
        std::memcpy(out.data(), s.header.data(), s.header.size());
        uint16_t* pixels = reinterpret_cast<uint16_t*>(out.data() + s.header.size());
        if (!c.decodeFrame(frameIndex, pixels, s.data, error)) return false;
        if (fixer_) fixer_->apply(pixels, frameIndex);
        return true;
    }
    const uint8_t* image = nullptr;
    size_t imageBytes = 0;
    bool compressed = false;
    if (!encodeImage(frameIndex, s, &image, &imageBytes, &compressed, error)) return false;
    buildHeader(frameIndex, imageBytes, compressed, c.bitsPerPixel(), s.header);
    out.resize(s.header.size() + imageBytes);
    std::memcpy(out.data(), s.header.data(), s.header.size());
    std::memcpy(out.data() + s.header.size(), image, imageBytes);
    return true;
}

void Writer::frameRateRational(int32_t out[2]) const {
    out[0] = int32_t(clip_.mlvi.sourceFpsNom);
    out[1] = int32_t(clip_.mlvi.sourceFpsDenom ? clip_.mlvi.sourceFpsDenom : 1);
    if (options_.fps > 0) {
        out[0] = int32_t(std::lround(options_.fps * 1000));
        out[1] = 1000;
    }
}

double Writer::frameRate() const {
    int32_t r[2];
    frameRateRational(r);
    return double(r[0]) / r[1];
}

int64_t Writer::timecodeFrame(uint32_t frameIndex) const {
    const mlv::Clip& c = clip_;
    const double fps = frameRate();
    int64_t tcFrame = c.frame(frameIndex).frameNumber;
    if (options_.timecode == Timecode::TimeOfDay && FrameTime(c, frameIndex) >= 0 && fps > 0) {
        const int64_t secondOfDay = c.rtci.tm_hour * 3600 + c.rtci.tm_min * 60 + c.rtci.tm_sec;
        tcFrame = int64_t(std::llround(secondOfDay * std::round(fps))) + c.frame(frameIndex).frameNumber;
    }
    return tcFrame;
}

bool Writer::renderHeader(uint32_t frameIndex, std::vector<uint8_t>& out) const {
    if (frameIndex >= clip_.frameCount() || imageSize_ == 0) return false;
    buildHeader(frameIndex, size_t(imageSize_), false, output_ == Output::Uncompressed16 ? 16 : clip_.bitsPerPixel(),
                out);
    return true;
}

bool Writer::writeFrame(uint32_t frameIndex, const std::string& path, Scratch& s, uint64_t* bytesWritten,
                        std::string* error) const {
    if (frameIndex >= clip_.frameCount()) {
        if (error) *error = "No frame " + std::to_string(frameIndex);
        return false;
    }
    const uint8_t* image = nullptr;
    size_t imageBytes = 0;
    bool compressed = false;
    int bitsPerSample = clip_.bitsPerPixel();
    if (output_ == Output::Uncompressed16) {
        s.pixels.resize(size_t(clip_.width()) * clip_.height());
        if (!clip_.decodeFrame(frameIndex, s.pixels.data(), s.data, error)) return false;
        if (fixer_) fixer_->apply(s.pixels.data(), frameIndex);
        image = reinterpret_cast<const uint8_t*>(s.pixels.data());
        imageBytes = size_t(imageSize_);
        bitsPerSample = 16;
    } else if (!encodeImage(frameIndex, s, &image, &imageBytes, &compressed, error)) {
        return false;
    }
    buildHeader(frameIndex, imageBytes, compressed, bitsPerSample, s.header);
    if (!util::writeFile(path, s.header.data(), s.header.size(), image, imageBytes, error)) return false;
    if (bytesWritten) *bytesWritten = s.header.size() + imageBytes;
    return true;
}

}  // namespace dng
