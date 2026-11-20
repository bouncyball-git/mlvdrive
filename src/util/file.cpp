#include "util/file.h"

#include <cerrno>
#include <cstring>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace util {

std::filesystem::path u8(const std::string& utf8) {
#if defined(__cpp_char8_t)
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
#else
    return std::filesystem::u8path(utf8);
#endif
}

std::string u8(const std::filesystem::path& path) {
    auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

#ifdef _WIN32
static std::wstring Wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    std::wstring wide(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), wide.data(), n);
    return wide;
}
#endif

std::FILE* openFile(const std::string& utf8Path, const char* mode) {
#ifdef _WIN32
    std::wstring wmode(mode, mode + std::strlen(mode));
    return _wfopen(Wide(utf8Path).c_str(), wmode.c_str());
#else
    return std::fopen(utf8Path.c_str(), mode);
#endif
}

bool fileExists(const std::string& utf8Path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(u8(utf8Path), ec);
}

RandomAccessFile::~RandomAccessFile() { close(); }

bool RandomAccessFile::isOpen() const {
#ifdef _WIN32
    return handle_ != nullptr;
#else
    return fd_ >= 0;
#endif
}

bool RandomAccessFile::open(const std::string& utf8Path) {
    close();
#ifdef _WIN32
    HANDLE h = CreateFileW(Wide(utf8Path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size)) {
        CloseHandle(h);
        return false;
    }
    handle_ = h;
    size_ = uint64_t(size.QuadPart);
#else
    int fd = ::open(utf8Path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0) {
        ::close(fd);
        return false;
    }
    fd_ = fd;
    size_ = uint64_t(st.st_size);
#endif
    return true;
}

void RandomAccessFile::close() {
#ifdef _WIN32
    if (handle_) CloseHandle(handle_);
    handle_ = nullptr;
#else
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
#endif
    size_ = 0;
}

bool RandomAccessFile::readAt(uint64_t offset, void* buffer, size_t bytes) const {
    auto* out = static_cast<uint8_t*>(buffer);
    while (bytes > 0) {
#ifdef _WIN32
        if (!handle_) return false;
        DWORD chunk = DWORD(bytes > (1u << 30) ? (1u << 30) : bytes);
        OVERLAPPED ov{};
        ov.Offset = DWORD(offset & 0xFFFFFFFFu);
        ov.OffsetHigh = DWORD(offset >> 32);
        DWORD got = 0;
        // A synchronous handle with an OVERLAPPED offset: a positional read, safe from any thread.
        if (!ReadFile(handle_, out, chunk, &got, &ov) || got == 0) return false;
#else
        if (fd_ < 0) return false;
        size_t chunk = bytes > (size_t(1) << 30) ? (size_t(1) << 30) : bytes;
        ssize_t got = ::pread(fd_, out, chunk, off_t(offset));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
#endif
        out += got;
        offset += uint64_t(got);
        bytes -= size_t(got);
    }
    return true;
}

bool writeFile(const std::string& utf8Path, const void* a, size_t aBytes, const void* b, size_t bBytes,
               std::string* error) {
    const std::string temporary = utf8Path + ".part";
    std::FILE* f = openFile(temporary, "wb");
    if (!f) {
        if (error) *error = "Cannot create " + utf8Path + ": " + std::strerror(errno);
        return false;
    }
    bool ok = (aBytes == 0 || std::fwrite(a, 1, aBytes, f) == aBytes) &&
              (bBytes == 0 || std::fwrite(b, 1, bBytes, f) == bBytes);
    ok = (std::fclose(f) == 0) && ok;
    std::error_code ec;
    if (ok) {
        std::filesystem::rename(u8(temporary), u8(utf8Path), ec);  // replaces an existing file
        ok = !ec;
    }
    if (!ok) {
        if (error) *error = "Cannot write " + utf8Path + (ec ? ": " + ec.message() : std::string(" (disk full?)"));
        std::filesystem::remove(u8(temporary), ec);
    }
    return ok;
}

}  // namespace util
