// Files with UTF-8 paths on every platform, and reads at an offset that several threads can make
// on one open file at once (pread on Linux and macOS, ReadFile with an offset on Windows).
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

namespace util {

std::filesystem::path u8(const std::string& utf8);       // UTF-8 text -> path
std::string u8(const std::filesystem::path& path);       // path -> UTF-8 text

// fopen with a UTF-8 path (Windows' fopen takes the ANSI code page).
std::FILE* openFile(const std::string& utf8Path, const char* mode);

bool fileExists(const std::string& utf8Path);

class RandomAccessFile {
public:
    RandomAccessFile() = default;
    ~RandomAccessFile();
    RandomAccessFile(const RandomAccessFile&) = delete;
    RandomAccessFile& operator=(const RandomAccessFile&) = delete;

    bool open(const std::string& utf8Path);
    void close();
    bool isOpen() const;
    uint64_t size() const { return size_; }
    // Reads exactly `bytes` at `offset`; false on an error or a short read. Thread-safe.
    bool readAt(uint64_t offset, void* buffer, size_t bytes) const;

private:
#ifdef _WIN32
    void* handle_ = nullptr;  // HANDLE
#else
    int fd_ = -1;
#endif
    uint64_t size_ = 0;
};

// Writes a file in one go: `a` then `b` (either may be empty). Into a temporary name first,
// renamed when complete, so a cancelled or failed export leaves no half-written file.
bool writeFile(const std::string& utf8Path, const void* a, size_t aBytes, const void* b, size_t bBytes,
               std::string* error);

}  // namespace util
