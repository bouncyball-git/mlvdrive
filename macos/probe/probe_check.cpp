// Reads every f_*.dng of a mounted probe volume and compares it with the pattern the module serves
// (probe_pattern.h): wrong bytes, short files or failed reads are counted and the first few shown.
//   probe_check <mount point> [size]     (size: the module's size=, default 4148416)
#include "probe_pattern.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: probe_check <mount point> [size]\n");
        return 2;
    }
    const size_t size = argc > 2 ? size_t(std::strtoull(argv[2], nullptr, 10)) : 4148416;
    const std::vector<uint8_t> pattern = probe::makePattern(size);
    std::vector<std::string> files;
    for (const auto& e : std::filesystem::directory_iterator(argv[1])) {
        const std::string name = e.path().filename().string();
        if (name.size() == 12 && name.rfind("f_", 0) == 0) files.push_back(e.path().string());
    }
    std::sort(files.begin(), files.end());
    std::vector<uint8_t> buffer(size + 4096);
    size_t bad = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (const std::string& path : files) {
        const uint64_t index = std::strtoull(path.c_str() + path.size() - 10, nullptr, 10);
        const int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            if (bad++ < 5) std::printf("%s: can't open\n", path.c_str());
            continue;
        }
        fcntl(fd, F_NOCACHE, 1);
        size_t total = 0;
        for (;;) {
            const ssize_t got = read(fd, buffer.data() + total, std::min<size_t>(1 << 20, buffer.size() - total));
            if (got <= 0) break;
            total += size_t(got);
        }
        close(fd);
        if (total != size || std::memcmp(buffer.data(), pattern.data() + probe::shift(index), size) != 0) {
            size_t at = 0;
            while (at < std::min(total, size) && buffer[at] == pattern[probe::shift(index) + at]) ++at;
            if (bad++ < 5) std::printf("%s: %zu bytes, first difference at %zu\n", path.c_str(), total, at);
        }
    }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%zu files checked, %zu bad, %.1f files/s\n", files.size(), bad, double(files.size()) / s);
    return bad || files.empty() ? 1 : 0;
}
