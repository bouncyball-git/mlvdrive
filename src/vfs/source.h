// What a clip folder's DNG files are made from: an MLV clip, or the test patterns. Every method
// may be called from several threads at once.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vfs {

class FrameSource {
public:
    virtual ~FrameSource() = default;
    virtual uint32_t frameCount() const = 0;
    virtual uint64_t headerSize() const = 0;  // the same for every frame
    virtual uint64_t fileSize() const = 0;    // every frame's file: known before any is made
    // When a frame was taken, in 100 ns units after the first (for the files' dates).
    virtual uint64_t frameOffset(uint32_t frame) const = 0;
    // A frame's header alone (headerSize() bytes), or its whole DNG file.
    virtual bool renderHeader(uint32_t frame, std::vector<uint8_t>& out) const = 0;
    virtual bool renderFrame(uint32_t frame, std::vector<uint8_t>& out, std::string* error) const = 0;
};

}  // namespace vfs
