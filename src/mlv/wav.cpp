#include "mlv/wav.h"

#include "mlv/clip.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace mlv {

namespace {

class Bytes {
public:
    void raw(const void* p, size_t n) { data.insert(data.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
    void text(const char* s, size_t n) {  // fixed-size, zero padded
        size_t len = std::min(std::strlen(s), n);
        raw(s, len);
        data.insert(data.end(), n - len, 0);
    }
    void u16(uint16_t v) { raw(&v, 2); }
    void u32(uint32_t v) { raw(&v, 4); }
    void u64(uint64_t v) { raw(&v, 8); }
    std::vector<uint8_t> data;
};

std::string Trimmed(const char* s, size_t n) { return std::string(s, strnlen(s, n)); }

void FormatChunk(Bytes& b, const Clip& clip) {
    const uint16_t channels = clip.wavi.channels;
    const uint32_t rate = clip.wavi.samplingRate;
    const uint16_t bits = 16;
    b.text("fmt ", 4);
    b.u32(16);
    b.u16(1);  // PCM
    b.u16(channels);
    b.u32(rate);
    b.u32(rate * channels * bits / 8);
    b.u16(uint16_t(channels * bits / 8));
    b.u16(bits);
}

}  // namespace

WavLayout wavLayout(const Clip& clip, uint64_t timeReference) {
    WavLayout w;
    const double clipFps = clip.fps();
    if (!clip.hasAudio() || clipFps <= 0 || clip.frameCount() == 0) return w;

    const uint64_t sampleSize = uint64_t(clip.wavi.channels) * 2;
    const uint32_t rate = clip.wavi.samplingRate;
    // The audio is in real time: the frames last frameCount / the recorded rate.
    const uint64_t samples = uint64_t(std::llround(double(clip.frameCount()) * rate / clipFps));
    w.dataBytes = samples * sampleSize;

    char text[64];
    Bytes bext;
    bext.text("Exported MLV Audio", 256);  // description
    bext.text(Trimmed(clip.idnt.cameraName, 32).c_str(), 32);  // originator
    std::snprintf(text, sizeof text, "JPCAN%04u%.8s%02d%02d%02d", clip.idnt.cameraModel & 0xFFFF,
                  Trimmed(clip.idnt.cameraSerial, 32).c_str(), clip.rtci.tm_hour, clip.rtci.tm_min, clip.rtci.tm_sec);
    bext.text(text, 32);  // originator reference
    std::snprintf(text, sizeof text, "%04d:%02d:%02d", clip.rtci.tm_year + 1900, clip.rtci.tm_mon + 1, clip.rtci.tm_mday);
    bext.text(text, 10);
    std::snprintf(text, sizeof text, "%02d:%02d:%02d", clip.rtci.tm_hour, clip.rtci.tm_min, clip.rtci.tm_sec);
    bext.text(text, 8);
    bext.u64(timeReference);
    bext.u16(1);                                          // version
    bext.data.insert(bext.data.end(), 64 + 10 + 180, 0);  // UMID, loudness values, reserved
    bext.text("PCM ", 4);                                 // coding history

    const unsigned nom = clip.mlvi.sourceFpsNom, den = clip.mlvi.sourceFpsDenom ? clip.mlvi.sourceFpsDenom : 1;
    char ixml[1024] = {};  // a fixed size: the header's size doesn't depend on the text
    std::snprintf(ixml, sizeof ixml,
                  "<?xml version=\"1.0\" encoding=\"UTF-8\"?><BWFXML><IXML_VERSION>1.5</IXML_VERSION>"
                  "<PROJECT>MLV Drive</PROJECT><NOTE></NOTE><CIRCLED>FALSE</CIRCLED>"
                  "<BLACKMAGIC-KEYWORDS></BLACKMAGIC-KEYWORDS><TAPE>1</TAPE><SCENE>1</SCENE>"
                  "<BLACKMAGIC-SHOT>1</BLACKMAGIC-SHOT><TAKE>1</TAKE><BLACKMAGIC-ANGLE>ms</BLACKMAGIC-ANGLE>"
                  "<SPEED><MASTER_SPEED>%u/%u</MASTER_SPEED><CURRENT_SPEED>%u/%u</CURRENT_SPEED>"
                  "<TIMECODE_RATE>%u/%u</TIMECODE_RATE><TIMECODE_FLAG>NDF</TIMECODE_FLAG></SPEED></BWFXML>",
                  nom, den, nom, den, nom, den);

    Bytes b;
    b.text("RIFF", 4);
    b.u32(0);  // set below
    b.text("WAVE", 4);
    b.text("bext", 4);
    b.u32(uint32_t(bext.data.size()));
    b.raw(bext.data.data(), bext.data.size());
    b.text("iXML", 4);
    b.u32(sizeof ixml);
    b.raw(ixml, sizeof ixml);
    FormatChunk(b, clip);
    b.text("data", 4);
    b.u32(uint32_t(w.dataBytes));
    const uint32_t riff = uint32_t(b.data.size() + w.dataBytes - 8);
    std::memcpy(b.data.data() + 4, &riff, 4);
    w.header = std::move(b.data);
    return w;
}

uint64_t wavTimeReference(const Clip& clip, int64_t tcFrame, double fps) {
    if (fps <= 0 || tcFrame < 0) return 0;
    return uint64_t(double(tcFrame) * double(clip.wavi.samplingRate) / fps);
}

bool readWav(const Clip& clip, const WavLayout& layout, uint64_t offset, size_t n, uint8_t* out, std::string* error) {
    const uint64_t headerSize = layout.header.size();
    if (offset < headerSize) {
        const size_t take = size_t(std::min<uint64_t>(n, headerSize - offset));
        std::memcpy(out, layout.header.data() + offset, take);
        out += take;
        offset += take;
        n -= take;
    }
    if (n == 0) return true;
    // The audio (silence where the recording runs out: readSyncedAudioRange gives zeros there).
    return clip.readSyncedAudioRange(offset - headerSize, n, out, error);
}

}  // namespace mlv
