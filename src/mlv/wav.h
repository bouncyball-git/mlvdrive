// Broadcast WAVE files of a clip's audio (with the bext and iXML chunks MLV-App writes, which
// editors such as DaVinci Resolve use to line the audio up with the DNG sequence).
//
// A file is a header (its size fixed per clip) and the synced audio (Clip::readSyncedAudio) for
// the clip's frames, made up with silence where the recording runs out: so the drive knows its
// size without reading any audio, and serves any range of it from the blocks it comes from.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mlv {

class Clip;

struct WavLayout {
    std::vector<uint8_t> header;  // RIFF, bext, iXML, fmt and the data chunk's header
    uint64_t dataBytes = 0;       // the 16-bit PCM after it
    uint64_t size() const { return header.size() + dataBytes; }
};

// The file for all of a clip's frames: the audio lasts as long as the frames at the recorded
// rate. `timeReference` is bext's: the audio's first sample in samples since midnight (time-of-day
// timecode) or since the recording started; to match the DNGs, wavTimeReference().
// An empty header if the clip has no audio.
WavLayout wavLayout(const Clip& clip, uint64_t timeReference);

// bext's time reference for audio starting at the frame whose DNG timecode is `tcFrame` (frames
// of the timecode, as the DNG writer counts them) at the DNGs' frame rate `fps`: tcFrame * rate /
// fps (MLV-App's formula; the samples are real time, so 23.976 timecode maps as it should).
uint64_t wavTimeReference(const Clip& clip, int64_t tcFrame, double fps);

// `n` bytes of the file from `offset` (the header, the audio, silence past the recording).
bool readWav(const Clip& clip, const WavLayout& layout, uint64_t offset, size_t n, uint8_t* out, std::string* error);

}  // namespace mlv
