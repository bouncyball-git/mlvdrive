// MLV (Magic Lantern Video) on-disk structures, as written by the camera.
//
// Layout from MLV-App's src/mlv/mlv.h and raw.h (Magic Lantern Team, GPL-2.0-or-later), made
// portable: plain #pragma pack (MSVC, GCC and Clang) and fixed-size fields only (the camera is a
// 32-bit machine, so raw_info's pointer is a 32-bit value on disk). Every multi-byte value is
// little-endian, as are all the platforms this program runs on.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mlv {

constexpr uint16_t kVideoFlagJpeg2k = 0x10;
constexpr uint16_t kVideoFlagLj92 = 0x20;
constexpr uint16_t kVideoFlagDelta = 0x40;
constexpr uint16_t kVideoFlagLzma = 0x80;
constexpr uint16_t kVideoFlagMcraw = 0x100;

#pragma pack(push, 1)

struct BlockHeader {
    char type[4];
    uint32_t size;  // the whole block, this header included
    uint64_t timestamp;
};

struct FileHeader {  // MLVI
    char magic[4];
    uint32_t blockSize;
    char versionString[8];
    uint64_t fileGuid;
    uint16_t fileNum;
    uint16_t fileCount;
    uint32_t fileFlags;     // 1 = out-of-order data, 2 = dropped frames, 4 = single image, 8 = stopped by an error
    uint16_t videoClass;    // 1 (raw) | the kVideoFlag values
    uint16_t audioClass;    // 0 = none, 1 = WAV
    uint32_t videoFrameCount;
    uint32_t audioFrameCount;
    uint32_t sourceFpsNom;
    uint32_t sourceFpsDenom;
};

struct VideoFrameHeader {  // VIDF
    char type[4];
    uint32_t blockSize;
    uint64_t timestamp;     // microseconds from the start of recording
    uint32_t frameNumber;
    uint16_t cropPosX;      // sensor position of the frame (8x2 blocks)
    uint16_t cropPosY;
    uint16_t panPosX;       // the same, 1x1 resolution
    uint16_t panPosY;
    uint32_t frameSpace;    // padding before the frame data
};

struct AudioFrameHeader {  // AUDF
    char type[4];
    uint32_t blockSize;
    uint64_t timestamp;
    uint32_t frameNumber;
    uint32_t frameSpace;
};

struct RawInfo {
    uint32_t api_version;
    uint32_t buffer;        // a pointer on the camera; meaningless here
    int32_t height, width, pitch;
    int32_t frame_size;
    int32_t bits_per_pixel;
    int32_t black_level;
    int32_t white_level;
    struct { int32_t x, y, width, height; } jpeg;  // also "crop": origin[2], size[2]
    struct { int32_t y1, x1, y2, x2; } active_area;
    int32_t exposure_bias[2];
    int32_t cfa_pattern;
    int32_t calibration_illuminant1;
    int32_t color_matrix1[18];
    int32_t dynamic_range;  // EV x100
};

struct RawInfoBlock {  // RAWI
    char type[4];
    uint32_t blockSize;
    uint64_t timestamp;
    uint16_t xRes;          // the recorded frame's size
    uint16_t yRes;
    RawInfo raw_info;
};

struct RawCaptureBlock {  // RAWC
    char type[4];
    uint32_t blockSize;
    uint64_t timestamp;
    uint16_t sensor_res_x;
    uint16_t sensor_res_y;
    uint16_t sensor_crop;   // crop factor x100
    uint16_t reserved;
    uint8_t binning_x;
    uint8_t skipping_x;
    uint8_t binning_y;
    uint8_t skipping_y;
    int16_t offset_x;
    int16_t offset_y;
};

struct WaveInfoBlock {  // WAVI
    char type[4];
    uint32_t blockSize;
    uint64_t timestamp;
    uint16_t format;        // 1 = PCM
    uint16_t channels;
    uint32_t samplingRate;
    uint32_t bytesPerSecond;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
};

struct ExposureBlock {  // EXPO
    char type[4];
    uint32_t blockSize;
    uint64_t timestamp;
    uint32_t isoMode;
    uint32_t isoValue;
    uint32_t isoAnalog;
    uint32_t digitalGain;   // 1024 = 1 EV
    uint64_t shutterValue;  // microseconds
};

struct LensBlock {  // LENS
    char type[4];
    uint32_t blockSize;
    uint64_t timestamp;
    uint16_t focalLength;   // mm
    uint16_t focalDist;     // mm, 65535 = infinity
    uint16_t aperture;      // f-number x100
    uint8_t stabilizerMode;
    uint8_t autofocusMode;
    uint32_t flags;
    uint32_t lensID;
    char lensName[32];
    char lensSerial[32];
};

struct ExtendedLensBlock {  // ELNS, followed by the lens name
    char type[4];
    uint32_t blockSize;
    uint64_t timestamp;
    uint16_t focalLengthMin;
    uint16_t focalLengthMax;
    uint16_t apertureMin;
    uint16_t apertureMax;
    uint32_t version;
    uint8_t extenderInfo;
    uint8_t capabilities;
    uint8_t chipped;
};

struct RealTimeClockBlock {  // RTCI
    char type[4];
    uint32_t blockSize;
    uint64_t timestamp;
    uint16_t tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst, tm_gmtoff;
    char tm_zone[8];
};

struct IdentityBlock {  // IDNT
    char type[4];
    uint32_t blockSize;
    uint64_t timestamp;
    char cameraName[32];
    uint32_t cameraModel;
    char cameraSerial[32];
};

struct WhiteBalanceBlock {  // WBAL
    char type[4];
    uint32_t blockSize;
    uint64_t timestamp;
    uint32_t wb_mode;       // see WbMode
    uint32_t kelvin;
    uint32_t wbgain_r;      // custom mode; 1024 = 1.0
    uint32_t wbgain_g;
    uint32_t wbgain_b;
    uint32_t wbs_gm;
    uint32_t wbs_ba;
};

#pragma pack(pop)

enum WbMode : uint32_t {
    kWbAuto = 0, kWbSunny = 1, kWbCloudy = 2, kWbTungsten = 3, kWbFluorescent = 4,
    kWbFlash = 5, kWbCustom = 6, kWbShade = 8, kWbKelvin = 9,
};

static_assert(sizeof(BlockHeader) == 16, "MLV block header");
static_assert(sizeof(FileHeader) == 52, "MLVI");
static_assert(sizeof(VideoFrameHeader) == 32, "VIDF");
static_assert(sizeof(AudioFrameHeader) == 24, "AUDF");
static_assert(sizeof(RawInfo) == 160, "raw_info");
static_assert(sizeof(RawInfoBlock) == 180, "RAWI");
static_assert(sizeof(RawCaptureBlock) == 32, "RAWC");
static_assert(sizeof(WaveInfoBlock) == 32, "WAVI");
static_assert(sizeof(ExposureBlock) == 40, "EXPO");
static_assert(sizeof(LensBlock) == 96, "LENS");
static_assert(sizeof(IdentityBlock) == 84, "IDNT");
static_assert(sizeof(WhiteBalanceBlock) == 44, "WBAL");

}  // namespace mlv
