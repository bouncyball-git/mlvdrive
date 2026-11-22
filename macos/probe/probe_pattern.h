// The probe's file contents, shared by the FSKit module (which serves them) and probe_check (which
// reads them back from the mounted volume): file i's byte at offset o is pattern[shift(i) + o], a
// fixed pseudo-random buffer, so serving a range is one memcpy and every file differs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace probe {

constexpr size_t kShiftSpan = 65536;

inline size_t shift(uint64_t file) { return size_t((file * 4099) % kShiftSpan); }

// fileSize + kShiftSpan bytes: enough for any file's whole range.
inline std::vector<uint8_t> makePattern(size_t fileSize) {
    std::vector<uint8_t> p(fileSize + kShiftSpan);
    uint64_t x = 0x9E3779B97F4A7C15ull;
    for (size_t i = 0; i < p.size(); i += 8) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        for (size_t k = 0; k < 8 && i + k < p.size(); ++k) p[i + k] = uint8_t(x >> (8 * k));
    }
    return p;
}

}  // namespace probe
