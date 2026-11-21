// winfsp.h first: it includes windows.h its own way (WIN32_NO_STATUS, then ntstatus.h).
#include <winfsp/winfsp.h>

#include "fs/winfsp_fs.h"

#include <sddl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace fsys {

namespace {

constexpr UINT32 kSectorSize = 4096;

uint64_t roundUp(uint64_t n) { return (n + kSectorSize - 1) / kSectorSize * kSectorSize; }

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string ntError(const char* what, NTSTATUS status) {
    char text[96];
    std::snprintf(text, sizeof text, "%s failed (NTSTATUS 0x%08lX)", what, (unsigned long)status);
    return text;
}

// What an open file or folder keeps until it is closed. A file's bytes are only kept until its
// handle is closed (Cleanup): WinFsp's Close comes much later, when Windows' cache lets the file
// go, and reads that come in between (from that cache) fetch the frame again.
struct FileContext {
    vfs::Node node;
    PVOID dirBuffer = nullptr;    // folders: WinFsp's directory buffer
    std::mutex mutex;             // files: Read can come on several threads at once
    std::vector<uint8_t> header;  // the DNG's header alone (reads of the start)
    vfs::FrameData file;          // the whole DNG (shared with the cache), once anything past the header is read
};

}  // namespace

struct Drive::Impl {
    const vfs::Tree& tree;
    vfs::FrameCache& cache;
    FSP_FILE_SYSTEM* fileSystem = nullptr;
    PSECURITY_DESCRIPTOR security = nullptr;  // the same for everything: read-only for everyone
    ULONG securitySize = 0;
    std::wstring mountPoint;

    Impl(const vfs::Tree& t, vfs::FrameCache& c) : tree(t), cache(c) {}

    static Impl* of(FSP_FILE_SYSTEM* fs) { return static_cast<Impl*>(fs->UserContext); }

    void fileInfo(const vfs::Node& n, FSP_FSCTL_FILE_INFO* info) const {
        std::memset(info, 0, sizeof *info);
        if (n.kind == vfs::Node::Directory) {
            info->FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
            info->IndexNumber = n.dir->id;
            info->CreationTime = info->LastAccessTime = info->LastWriteTime = info->ChangeTime = n.dir->time;
        } else {
            const vfs::ClipEntry& c = *n.dir->clip;
            info->FileAttributes = FILE_ATTRIBUTE_READONLY;
            info->FileSize = c.writer->fileSize();
            info->AllocationSize = roundUp(info->FileSize);
            info->IndexNumber = c.id + 1 + n.frame;
            // Frames apart by their time in the clip, so the files sort by time as well as by name.
            const uint64_t ts = c.clip->frame(n.frame).timestamp - c.clip->frame(0).timestamp;  // microseconds
            info->CreationTime = info->LastAccessTime = info->LastWriteTime = info->ChangeTime = c.time + ts * 10;
        }
    }

    NTSTATUS copySecurity(PSECURITY_DESCRIPTOR out, SIZE_T* size) const {
        if (!size) return STATUS_SUCCESS;
        if (*size < securitySize) {
            *size = securitySize;
            return STATUS_BUFFER_OVERFLOW;
        }
        *size = securitySize;
        if (out) std::memcpy(out, security, securitySize);
        return STATUS_SUCCESS;
    }

    // One directory entry into the context's buffer.
    static bool addEntry(FileContext* ctx, const std::wstring& name, const FSP_FSCTL_FILE_INFO& info, NTSTATUS* result) {
        union {
            UINT8 bytes[FIELD_OFFSET(FSP_FSCTL_DIR_INFO, FileNameBuf) + 256 * sizeof(WCHAR)];
            FSP_FSCTL_DIR_INFO d;
        } buf;
        const size_t len = std::min<size_t>(name.size(), 255);
        std::memset(&buf.d, 0, sizeof buf.d);
        buf.d.Size = UINT16(FIELD_OFFSET(FSP_FSCTL_DIR_INFO, FileNameBuf) + len * sizeof(WCHAR));
        buf.d.FileInfo = info;
        std::memcpy(buf.d.FileNameBuf, name.data(), len * sizeof(WCHAR));
        return FspFileSystemFillDirectoryBuffer(&ctx->dirBuffer, &buf.d, result) != FALSE;
    }

    // --- FSP_FILE_SYSTEM_INTERFACE ---

    static NTSTATUS GetVolumeInfo(FSP_FILE_SYSTEM* fs, FSP_FSCTL_VOLUME_INFO* info) {
        Impl* self = of(fs);
        std::memset(info, 0, sizeof *info);
        info->TotalSize = std::max<uint64_t>(self->tree.totalBytes(), kSectorSize);
        info->FreeSize = 0;
        static const wchar_t kLabel[] = L"MLV Drive";
        info->VolumeLabelLength = UINT16((sizeof kLabel) - sizeof(wchar_t));
        std::memcpy(info->VolumeLabel, kLabel, info->VolumeLabelLength);
        return STATUS_SUCCESS;
    }

    static NTSTATUS GetSecurityByName(FSP_FILE_SYSTEM* fs, PWSTR name, PUINT32 attributes,
                                      PSECURITY_DESCRIPTOR security, SIZE_T* securitySize) {
        Impl* self = of(fs);
        vfs::Node n = self->tree.find(name);
        if (n.kind == vfs::Node::None) return STATUS_OBJECT_NAME_NOT_FOUND;
        if (attributes) *attributes = n.kind == vfs::Node::Directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_READONLY;
        return self->copySecurity(security, securitySize);
    }

    static NTSTATUS Open(FSP_FILE_SYSTEM* fs, PWSTR name, UINT32 createOptions, UINT32 /*grantedAccess*/,
                         PVOID* context, FSP_FSCTL_FILE_INFO* info) {
        Impl* self = of(fs);
        vfs::Node n = self->tree.find(name);
        if (n.kind == vfs::Node::None) return STATUS_OBJECT_NAME_NOT_FOUND;
        if (n.kind == vfs::Node::Frame && (createOptions & FILE_DIRECTORY_FILE)) return STATUS_NOT_A_DIRECTORY;
        if (n.kind == vfs::Node::Directory && (createOptions & FILE_NON_DIRECTORY_FILE)) return STATUS_FILE_IS_A_DIRECTORY;
        auto* ctx = new (std::nothrow) FileContext;
        if (!ctx) return STATUS_INSUFFICIENT_RESOURCES;
        ctx->node = n;
        *context = ctx;
        self->fileInfo(n, info);
        return STATUS_SUCCESS;
    }

    // WinFsp only passes opens on when Create and Overwrite exist too; a read-only volume never
    // gets to them, but if it does: nothing can be written.
    static NTSTATUS Create(FSP_FILE_SYSTEM*, PWSTR, UINT32, UINT32, UINT32, PSECURITY_DESCRIPTOR, UINT64, PVOID*,
                           FSP_FSCTL_FILE_INFO*) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }
    static NTSTATUS Overwrite(FSP_FILE_SYSTEM*, PVOID, UINT32, BOOLEAN, UINT64, FSP_FSCTL_FILE_INFO*) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }

    static VOID Cleanup(FSP_FILE_SYSTEM* /*fs*/, PVOID context, PWSTR /*name*/, ULONG /*flags*/) {
        auto* ctx = static_cast<FileContext*>(context);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->file.reset();
        ctx->header = std::vector<uint8_t>();
    }

    static VOID Close(FSP_FILE_SYSTEM* /*fs*/, PVOID context) {
        auto* ctx = static_cast<FileContext*>(context);
        FspFileSystemDeleteDirectoryBuffer(&ctx->dirBuffer);
        delete ctx;
    }

    static NTSTATUS Read(FSP_FILE_SYSTEM* fs, PVOID context, PVOID buffer, UINT64 offset, ULONG length,
                         PULONG transferred) {
        Impl* self = of(fs);
        auto* ctx = static_cast<FileContext*>(context);
        *transferred = 0;
        if (ctx->node.kind != vfs::Node::Frame) return STATUS_INVALID_DEVICE_REQUEST;
        const vfs::ClipEntry& c = *ctx->node.dir->clip;
        const uint64_t size = c.writer->fileSize();
        if (offset >= size) return STATUS_END_OF_FILE;
        const uint64_t end = std::min<uint64_t>(size, offset + length);

        std::lock_guard<std::mutex> lock(ctx->mutex);
        const std::vector<uint8_t>* bytes = ctx->file.get();
        if (!bytes) {
            if (end <= c.writer->headerSize()) {
                if (ctx->header.empty() && !c.writer->renderHeader(ctx->node.frame, ctx->header))
                    return STATUS_UNEXPECTED_IO_ERROR;
                bytes = &ctx->header;
            } else {
                std::string error;
                ctx->file = self->cache.get(c, ctx->node.frame, true, &error);
                if (!ctx->file) {
                    std::fprintf(stderr, "%s frame %u: %s\n", narrow(c.name).c_str(), ctx->node.frame, error.c_str());
                    return STATUS_UNEXPECTED_IO_ERROR;
                }
                bytes = ctx->file.get();
                ctx->header = std::vector<uint8_t>();
            }
        }
        if (bytes->size() < end) return STATUS_UNEXPECTED_IO_ERROR;  // not the size listed: a bug
        std::memcpy(buffer, bytes->data() + offset, size_t(end - offset));
        *transferred = ULONG(end - offset);
        return STATUS_SUCCESS;
    }

    static NTSTATUS GetFileInfo(FSP_FILE_SYSTEM* fs, PVOID context, FSP_FSCTL_FILE_INFO* info) {
        of(fs)->fileInfo(static_cast<FileContext*>(context)->node, info);
        return STATUS_SUCCESS;
    }

    static NTSTATUS GetSecurity(FSP_FILE_SYSTEM* fs, PVOID /*context*/, PSECURITY_DESCRIPTOR security,
                                SIZE_T* securitySize) {
        return of(fs)->copySecurity(security, securitySize);
    }

    static NTSTATUS ReadDirectory(FSP_FILE_SYSTEM* fs, PVOID context, PWSTR /*pattern*/, PWSTR marker, PVOID buffer,
                                  ULONG length, PULONG transferred) {
        Impl* self = of(fs);
        auto* ctx = static_cast<FileContext*>(context);
        if (ctx->node.kind != vfs::Node::Directory) return STATUS_NOT_A_DIRECTORY;
        const vfs::Dir* dir = ctx->node.dir;

        NTSTATUS result = STATUS_SUCCESS;
        if (FspFileSystemAcquireDirectoryBuffer(&ctx->dirBuffer, marker == nullptr, &result)) {
            FSP_FSCTL_FILE_INFO info;
            bool ok = true;
            if (dir != &self->tree.root()) {
                self->fileInfo(ctx->node, &info);
                ok = addEntry(ctx, L".", info, &result) && addEntry(ctx, L"..", info, &result);
            }
            if (dir->clip) {
                vfs::Node f;
                f.kind = vfs::Node::Frame;
                f.dir = dir;
                const uint32_t frames = dir->clip->clip->frameCount();
                for (uint32_t i = 0; ok && i < frames; ++i) {
                    f.frame = i;
                    self->fileInfo(f, &info);
                    ok = addEntry(ctx, vfs::Tree::frameName(*dir->clip, i), info, &result);
                }
            } else {
                vfs::Node d;
                d.kind = vfs::Node::Directory;
                for (size_t i = 0; ok && i < dir->dirs.size(); ++i) {
                    d.dir = dir->dirs[i].get();
                    self->fileInfo(d, &info);
                    ok = addEntry(ctx, d.dir->name, info, &result);
                }
            }
            FspFileSystemReleaseDirectoryBuffer(&ctx->dirBuffer);
        }
        if (!NT_SUCCESS(result)) return result;
        FspFileSystemReadDirectoryBuffer(&ctx->dirBuffer, marker, buffer, length, transferred);
        return STATUS_SUCCESS;
    }
};

Drive::Drive(const vfs::Tree& tree, vfs::FrameCache& cache) : impl_(std::make_unique<Impl>(tree, cache)) {}

Drive::~Drive() {
    unmount();
}

bool Drive::loadWinFsp(std::string* error) {
    NTSTATUS status = FspLoad(nullptr);
    if (!NT_SUCCESS(status)) {
        if (error) *error = "WinFsp isn't installed (https://winfsp.dev/rel/)";
        return false;
    }
    return true;
}

bool Drive::mount(const std::wstring& mountPoint, bool debug, std::string* error) {
    Impl& m = *impl_;
    if (m.fileSystem) {
        if (error) *error = "already mounted";
        return false;
    }
    // Owner and group Administrators; full access for SYSTEM and Administrators, read and
    // execute (open, list) for everyone. The volume is read-only whatever the ACL says.
    if (!m.security &&
        !ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFX;;;WD)",
                                                              SDDL_REVISION_1, &m.security, &m.securitySize)) {
        if (error) *error = "can't make the security descriptor";
        return false;
    }

    FSP_FSCTL_VOLUME_PARAMS params;
    std::memset(&params, 0, sizeof params);
    params.Version = sizeof params;
    params.SectorSize = kSectorSize;
    params.SectorsPerAllocationUnit = 1;
    params.MaxComponentLength = 255;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    params.VolumeCreationTime = uint64_t(now.dwHighDateTime) << 32 | now.dwLowDateTime;
    params.VolumeSerialNumber = UINT32(params.VolumeCreationTime / (10000 * 1000));
    // The tree never changes while mounted: names, sizes and dates can be cached for good.
    params.FileInfoTimeout = INFINITE;
    params.CaseSensitiveSearch = 0;
    params.CasePreservedNames = 1;
    params.UnicodeOnDisk = 1;
    params.PersistentAcls = 1;
    params.ReadOnlyVolume = 1;
    params.PostCleanupWhenModifiedOnly = 0;  // Cleanup for every handle: frees the frame it holds
    params.UmFileContextIsUserContext2 = 1;
    wcscpy_s(params.FileSystemName, L"MLVDrive");

    static FSP_FILE_SYSTEM_INTERFACE iface = [] {
        FSP_FILE_SYSTEM_INTERFACE i;
        std::memset(&i, 0, sizeof i);
        i.GetVolumeInfo = Impl::GetVolumeInfo;
        i.GetSecurityByName = Impl::GetSecurityByName;
        i.Create = Impl::Create;
        i.Open = Impl::Open;
        i.Overwrite = Impl::Overwrite;
        i.Cleanup = Impl::Cleanup;
        i.Close = Impl::Close;
        i.Read = Impl::Read;
        i.GetFileInfo = Impl::GetFileInfo;
        i.GetSecurity = Impl::GetSecurity;
        i.ReadDirectory = Impl::ReadDirectory;
        return i;
    }();

    NTSTATUS status = FspFileSystemCreate(const_cast<PWSTR>(L"" FSP_FSCTL_DISK_DEVICE_NAME), &params, &iface,
                                          &m.fileSystem);
    if (!NT_SUCCESS(status)) {
        m.fileSystem = nullptr;
        if (error) *error = ntError("creating the file system", status);
        return false;
    }
    m.fileSystem->UserContext = &m;
    if (debug) {
        FspDebugLogSetHandle(GetStdHandle(STD_ERROR_HANDLE));
        FspFileSystemSetDebugLog(m.fileSystem, UINT32(-1));
    }

    status = FspFileSystemSetMountPoint(m.fileSystem, mountPoint.empty() ? nullptr : const_cast<PWSTR>(mountPoint.c_str()));
    if (!NT_SUCCESS(status)) {
        FspFileSystemDelete(m.fileSystem);
        m.fileSystem = nullptr;
        if (error) {
            *error = ntError("mounting", status);
            if (!mountPoint.empty()) *error += " at " + narrow(mountPoint) + ": in use?";
        }
        return false;
    }
    m.mountPoint = FspFileSystemMountPoint(m.fileSystem);

    status = FspFileSystemStartDispatcher(m.fileSystem, 0);
    if (!NT_SUCCESS(status)) {
        FspFileSystemRemoveMountPoint(m.fileSystem);
        FspFileSystemDelete(m.fileSystem);
        m.fileSystem = nullptr;
        if (error) *error = ntError("starting the dispatcher", status);
        return false;
    }
    return true;
}

void Drive::unmount() {
    Impl& m = *impl_;
    if (m.fileSystem) {
        FspFileSystemStopDispatcher(m.fileSystem);
        FspFileSystemRemoveMountPoint(m.fileSystem);
        FspFileSystemDelete(m.fileSystem);
        m.fileSystem = nullptr;
    }
    if (m.security) {
        LocalFree(m.security);
        m.security = nullptr;
    }
}

std::wstring Drive::mountPoint() const {
    return impl_->mountPoint;
}

}  // namespace fsys
