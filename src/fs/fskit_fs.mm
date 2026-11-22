// The drive on macOS: an FSKit module (a file system extension in MLV Drive.app) showing the tree
// (vfs::Tree) as a read-only volume. The engine runs here: the clips are opened, and DNGs made
// through the frame cache (read-ahead), in this process.
//
//   mount -F -t mlvdrive <folder> <mount point>
//
// The folder mounted is the MLV folder; if it is MLV Drive's settings folder (holding
// mlvdrive.ini), the settings' folders are shown instead (several, merged). The other settings
// (DNG format, raw fixes, timecode, test patterns, cache, read-ahead) come from the settings file,
// and -o key=value overrides them: format=16|packed, raw_fixes=1|0, timecode=time|count,
// test_patterns=1|0, cache_mb=N, read_ahead=N. FSKit passes the options with activate, so the
// clips are opened there.
//
// Live folders: the folders are watched (app::FolderWatcher: FSEvents, polling for network
// folders); a change rebuilds the tree from the one shown (unchanged clips taken over, their
// frames still cached), the volume shows the new one, and its directory verifier moves on so
// listings start again. `notifyutil -p org.mlvdrive.rescan` (`mlvdrive rescan`) rescans at once.
// FSKit (macOS 26.5) has no way to tell the kernel what changed, and the kernel keeps the names it
// has looked up and their attributes for good: listings are current, but a name looked up before
// stays as it was (a clip replaced under its name: the old footage there; a name asked for before
// it existed: not found) until the volume is mounted again. (Measured: nothing expired in 90 s;
// ESTALE for such items reached programs as an error, the kernel didn't look again.)
// -o watch=0 (or watch = false in the settings): no watching, only the notification.
//
// Items: the root is FSItemIDRootDirectory (2), a ".metadata_never_index" 3 (Spotlight would make
// every frame); a plain folder keeps its tree id (top bit set); a clip's folder, frames and WAV
// are its ids (ClipEntry::id: folder id, frame N id + 1 + N, WAV id + 1 + frame count) + 16.
// One FSItem per id, kept until FSKit reclaims it. A plain folder's item finds its folder by path
// in the tree shown now (gone: empty); a clip's items hold the clip, so a file open while its clip
// changes keeps reading the clip it was opened in.
//
// Reads are answered on a queue of their own (user-initiated QoS: the performance cores), never
// on FSKit's thread: a read within a DNG's header makes the header alone, anything further the
// whole DNG through the frame cache. No reply is ever 0 bytes before the end of a file.
#import <FSKit/FSKit.h>
#import <Foundation/Foundation.h>
#import <os/log.h>

#include "app/folder_watcher.h"
#include "app/settings.h"
#include "raw/rawfix.h"
#include "util/file.h"
#include "vfs/frame_cache.h"
#include "vfs/tree.h"

#include <notify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <map>
#include <unordered_map>
#include <vector>

extern "C" int NSExtensionMain(int argc, const char* argv[]);

namespace {

constexpr uint64_t kRootId = FSItemIDRootDirectory;  // 2
constexpr uint64_t kNeverIndexId = 3;
constexpr uint64_t kClipOffset = 16;  // clip ids start at 1: kept clear of FSKit's and ours above
constexpr NSInteger kBlockSize = 4096;
constexpr NSInteger kIoSize = 1 << 20;  // measured: 1-4 MB alike, 256 KB a little slower
const char kNeverIndex[] = ".metadata_never_index";
const char kRescanNotification[] = "org.mlvdrive.rescan";

os_log_t Log() {
    static os_log_t log = os_log_create("org.mlvdrive.fs", "drive");
    return log;
}

NSError* posixError(int code) { return fs_errorForPOSIXError(code); }

// FILETIME (100 ns since 1601) -> timespec.
timespec toTimespec(uint64_t fileTime) {
    timespec t{};
    if (fileTime >= 116444736000000000ull) {
        const uint64_t unix100ns = fileTime - 116444736000000000ull;
        t.tv_sec = time_t(unix100ns / 10000000);
        t.tv_nsec = long(unix100ns % 10000000) * 100;
    }
    return t;
}

// What an item is.
struct Node {
    enum Kind { Folder, ClipFolder, Frame, Audio, NeverIndex } kind = Folder;
    uint64_t id = 0;
    uint64_t parentId = 0;
    std::wstring path;                            // Folder, ClipFolder: in the tree ("" the root, "\a\b")
    std::shared_ptr<const vfs::ClipEntry> clip;   // ClipFolder, Frame, Audio
    uint32_t frame = 0;
    bool isDirectory() const { return kind == Folder || kind == ClipFolder; }
};

uint64_t idOf(const vfs::Dir& d) { return d.clip ? d.clip->id + kClipOffset : d.id; }

Node childFolder(const vfs::Dir& d, const Node& parent) {
    Node n;
    n.kind = d.clip ? Node::ClipFolder : Node::Folder;
    n.id = idOf(d);
    n.parentId = parent.id;
    n.path = parent.path + L"\\" + d.name;
    n.clip = d.clip;
    return n;
}

Node clipFile(const Node& folder, vfs::Node::Kind kind, uint32_t frame) {
    const vfs::ClipEntry& c = *folder.clip;
    Node n;
    n.kind = kind == vfs::Node::Frame ? Node::Frame : Node::Audio;
    n.frame = frame;
    n.clip = folder.clip;
    n.parentId = folder.id;
    n.id = c.id + 1 + (n.kind == Node::Frame ? frame : c.source->frameCount()) + kClipOffset;
    return n;
}

// -o key=value[,key=value] over the settings (FSKit passes "-o" and the values as separate strings).
void applyOptions(FSTaskOptions* task, app::Settings* s) {
    for (NSString* str in task.taskOptions) {
        std::string text = str.UTF8String ? str.UTF8String : "";
        if (text.rfind("-o", 0) == 0) text = text.substr(2);
        size_t start = 0;
        while (start <= text.size()) {
            size_t end = text.find(',', start);
            if (end == std::string::npos) end = text.size();
            const std::string kv = text.substr(start, end - start);
            start = end + 1;
            const size_t eq = kv.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
            const bool on = v == "1" || v == "true" || v == "on" || v == "yes";
            if (k == "format") s->format = v == "packed" ? dng::Output::Uncompressed : dng::Output::Uncompressed16;
            else if (k == "raw_fixes") s->rawFixes = on;
            else if (k == "timecode") s->timecode = v == "count" ? dng::Timecode::FrameCount : dng::Timecode::TimeOfDay;
            else if (k == "test_patterns") s->testPatterns = on;
            else if (k == "cache_mb") s->cacheMb = unsigned(std::clamp<unsigned long>(std::strtoul(v.c_str(), nullptr, 10), 16, 1ul << 20));
            else if (k == "read_ahead") s->readAhead = int(std::min<unsigned long>(std::strtoul(v.c_str(), nullptr, 10), 256));
            else if (k == "watch") s->watch = on;
        }
    }
}

}  // namespace

// --- items ---

@interface MLVItem : FSItem {
@public
    Node node;
}
@end

@implementation MLVItem
@end

// --- the volume ---

@interface MLVVolume : FSVolume <FSVolumeOperations, FSVolumeReadWriteOperations, FSVolumeXattrOperations>
- (instancetype)initWithSettings:(const app::Settings&)settings source:(NSString*)source;
@end

@implementation MLVVolume {
    app::Settings _settings;
    std::mutex _treeMutex;
    std::shared_ptr<const vfs::Tree> _tree;  // the one shown now (a rescan replaces it)
    std::atomic<uint64_t> _generation;       // the directory verifier: moves on with each new tree
    std::mutex _rescanMutex;                 // one rescan at a time
    std::unique_ptr<vfs::FrameCache> _cache;
    std::unique_ptr<app::FolderWatcher> _watcher;
    int _notifyToken;
    std::mutex _itemsMutex;
    std::unordered_map<uint64_t, MLVItem*> _items;  // handed to FSKit and not reclaimed yet
    MLVItem* _root;
    dispatch_queue_t _reads;
    timespec _mounted;
}

@synthesize xattrOperationsInhibited = _xattrOperationsInhibited;
@synthesize requestedMountOptions = _requestedMountOptions;

- (instancetype)initWithSettings:(const app::Settings&)settings source:(NSString*)source {
    self = [super initWithVolumeID:[[FSVolumeIdentifier alloc] init] volumeName:[FSFileName nameWithString:@"MLV Drive"]];
    if (!self) return nil;
    _settings = settings;
    _xattrOperationsInhibited = YES;  // none: answered without a round trip
    _requestedMountOptions = FSMountOptionsReadOnly;
    _reads = dispatch_queue_create("org.mlvdrive.fs.reads",
                                   dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_CONCURRENT,
                                                                           QOS_CLASS_USER_INITIATED, 0));
    clock_gettime(CLOCK_REALTIME, &_mounted);
    _generation = 1;
    _notifyToken = NOTIFY_TOKEN_INVALID;
    return self;
}

- (std::shared_ptr<const vfs::Tree>)current {
    std::lock_guard<std::mutex> lock(_treeMutex);
    return _tree;
}

// Looks at the folders again: a new tree from the one shown (unchanged clips taken over), the
// frames of clips changed or gone forgotten, then shown.
- (void)rescan {
    std::lock_guard<std::mutex> rescanLock(_rescanMutex);
    const std::shared_ptr<const vfs::Tree> before = [self current];
    if (!before || !_cache) return;
    auto next = std::make_shared<vfs::Tree>();
    std::string error;
    size_t skipped = 0;
    if (!next->build(_settings.folders, app::buildOptions(_settings),
                     [&](const std::string& m) {
                         ++skipped;
                         os_log_error(Log(), "%{public}s", m.c_str());
                     },
                     &error, before.get())) {
        os_log_error(Log(), "rescan: %{public}s", error.c_str());
        return;
    }
    std::map<std::wstring, uint64_t> was, now;
    for (const auto& c : before->clips()) was[c->sourceKey] = c->id;
    for (const auto& c : next->clips()) now[c->sourceKey] = c->id;
    size_t added = 0, removed = 0, changed = 0;
    for (const auto& [key, id] : was) {
        auto it = now.find(key);
        if (it == now.end()) ++removed;
        else if (it->second != id) ++changed;
        if (it == now.end() || it->second != id) _cache->forget(id);  // its frames won't be asked for again
    }
    for (const auto& [key, id] : now)
        if (!was.count(key)) ++added;
    {
        std::lock_guard<std::mutex> lock(_treeMutex);
        _tree = std::move(next);
    }
    if (added || removed || changed) ++_generation;
    os_log(Log(), "rescanned: %zu added, %zu removed, %zu changed, %zu skipped", added, removed, changed, skipped);
}

// Finds and opens the clips (with the options given to mount).
- (NSError*)openWithOptions:(FSTaskOptions*)options {
    applyOptions(options, &_settings);
    raw::setPixelMapFolders(raw::defaultPixelMapFolders());
    const auto t0 = std::chrono::steady_clock::now();
    auto tree = std::make_shared<vfs::Tree>();
    std::string error;
    const bool built = tree->build(
        _settings.folders, app::buildOptions(_settings),
        [](const std::string& m) { os_log_error(Log(), "%{public}s", m.c_str()); }, &error);
    if (!built) {
        os_log_error(Log(), "can't build the drive: %{public}s", error.c_str());
        return posixError(EINVAL);
    }
    if (tree->clipCount() == 0) {
        os_log_error(Log(), "no MLV clips in the folders");
        return posixError(ENOENT);
    }
    const unsigned cpus = std::max(1u, std::thread::hardware_concurrency());
    const unsigned readAhead = _settings.readAhead < 0 ? cpus : unsigned(_settings.readAhead);
    const unsigned cacheMb = _settings.cacheMb ? _settings.cacheMb : app::cacheAutoMb();
    _cache = std::make_unique<vfs::FrameCache>(uint64_t(cacheMb) << 20, readAhead ? cpus : 0, readAhead);
    const size_t clips = tree->clipCount();
    const double gb = double(tree->totalBytes()) / 1e9;
    {
        std::lock_guard<std::mutex> lock(_treeMutex);
        _tree = std::move(tree);
    }
    Node root;
    root.kind = Node::Folder;
    root.id = kRootId;
    root.parentId = FSItemIDParentOfRoot;
    _root = [self itemFor:root];
    __weak MLVVolume* weakSelf = self;
    _watcher = std::make_unique<app::FolderWatcher>(_settings.folders, _settings.watch, [weakSelf] {
        [weakSelf rescan];
    });
    app::FolderWatcher* watcher = _watcher.get();
    notify_register_dispatch(kRescanNotification, &_notifyToken, _reads, ^(int) {
        watcher->rescanNow();
    });
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    os_log(Log(), "mounted: %zu clips, %.1f GB of DNG, opened in %.0f ms; cache %u MB, read-ahead %u%s", clips, gb,
           ms, cacheMb, readAhead,
           !_settings.watch ? "" : _watcher->polling() ? "; watching, network folders polled" : "; watching");
    return nil;
}

// The one item for a node (made the first time).
- (MLVItem*)itemFor:(const Node&)n {
    std::lock_guard<std::mutex> lock(_itemsMutex);
    auto it = _items.find(n.id);
    if (it != _items.end()) return it->second;
    MLVItem* item = [[MLVItem alloc] init];
    item->node = n;
    _items.emplace(n.id, item);
    return item;
}

- (FSItemAttributes*)attributesOf:(const Node&)n {
    FSItemAttributes* a = [[FSItemAttributes alloc] init];
    uint64_t time = 0;
    a.uid = getuid();
    a.gid = getgid();
    a.fileID = FSItemID(n.id);
    a.parentID = FSItemID(n.parentId);
    switch (n.kind) {
        case Node::Folder:
        case Node::ClipFolder:
            a.type = FSItemTypeDirectory;
            a.mode = S_IFDIR | 0555;
            a.linkCount = 2;
            if (n.kind == Node::ClipFolder) {
                time = n.clip->time;
            } else if (const auto tree = [self current]) {
                const vfs::Node d = tree->find(n.path.c_str());
                if (d.kind == vfs::Node::Directory) time = d.dir->time;
            }
            break;
        case Node::Frame:
            a.type = FSItemTypeFile;
            a.mode = S_IFREG | 0444;
            a.linkCount = 1;
            a.size = n.clip->source->fileSize();
            // Frames apart by their time in the clip, so the files sort by time as well as by name.
            time = n.clip->time + n.clip->source->frameOffset(n.frame);
            break;
        case Node::Audio:
            a.type = FSItemTypeFile;
            a.mode = S_IFREG | 0444;
            a.linkCount = 1;
            a.size = n.clip->wav.size();
            time = n.clip->time;
            break;
        case Node::NeverIndex:
            a.type = FSItemTypeFile;
            a.mode = S_IFREG | 0444;
            a.linkCount = 1;
            a.size = 0;
            break;
    }
    a.allocSize = (a.size + kBlockSize - 1) / kBlockSize * kBlockSize;
    const timespec t = time ? toTimespec(time) : _mounted;
    a.modifyTime = a.changeTime = a.accessTime = a.birthTime = t;
    a.flags = 0;
    a.supportsLimitedXAttrs = NO;
    a.inhibitKernelOffloadedIO = YES;
    return a;
}

// A directory's entries after "." and "..": its folders, or a clip's frames and WAV.
- (std::vector<Node>)childrenOf:(const Node&)dir {
    std::vector<Node> out;
    if (dir.kind == Node::Folder) {
        const auto tree = [self current];
        const vfs::Node d = tree ? tree->find(dir.path.c_str()) : vfs::Node{};
        if (d.kind != vfs::Node::Directory) return out;  // gone since
        out.reserve(d.dir->dirs.size());
        for (const auto& sub : d.dir->dirs) out.push_back(childFolder(*sub, dir));
    } else if (dir.kind == Node::ClipFolder) {
        const uint32_t frames = dir.clip->source->frameCount();
        out.reserve(frames + 1);
        for (uint32_t i = 0; i < frames; ++i) out.push_back(clipFile(dir, vfs::Node::Frame, i));
        if (dir.clip->hasWav()) out.push_back(clipFile(dir, vfs::Node::Audio, 0));
    }
    return out;
}

- (std::string)nameOf:(const Node&)n {
    switch (n.kind) {
        case Node::Folder:
        case Node::ClipFolder: return util::narrow(n.path.substr(n.path.rfind(L'\\') + 1));
        case Node::Frame: return util::narrow(vfs::Tree::frameName(*n.clip, n.frame));
        case Node::Audio: return util::narrow(vfs::Tree::wavName(*n.clip));
        case Node::NeverIndex: return kNeverIndex;
    }
    return {};
}

// --- FSVolumePathConfOperations ---

- (NSInteger)maximumLinkCount { return 1; }
- (NSInteger)maximumNameLength { return 255; }
- (BOOL)restrictsOwnershipChanges { return YES; }
- (BOOL)truncatesLongNames { return NO; }
- (NSInteger)maximumFileSizeInBits { return 64; }

// --- FSVolumeOperations ---

- (FSVolumeSupportedCapabilities*)supportedVolumeCapabilities {
    FSVolumeSupportedCapabilities* c = [[FSVolumeSupportedCapabilities alloc] init];
    c.supportsPersistentObjectIDs = YES;
    c.supports64BitObjectIDs = YES;
    c.supportsFastStatFS = YES;
    c.supports2TBFiles = YES;
    c.doesNotSupportSettingFilePermissions = YES;
    c.doesNotSupportImmutableFiles = YES;
    c.caseFormat = FSVolumeCaseFormatInsensitiveCasePreserving;  // as the tree's names
    return c;
}

- (FSStatFSResult*)volumeStatistics {
    FSStatFSResult* r = [[FSStatFSResult alloc] initWithFileSystemTypeName:@"mlvdrive"];
    r.blockSize = kBlockSize;
    r.ioSize = kIoSize;
    const auto tree = [self current];
    const uint64_t bytes = tree ? tree->totalBytes() : 0;
    r.totalBytes = r.usedBytes = (bytes + kBlockSize - 1) / kBlockSize * kBlockSize;
    r.freeBytes = r.availableBytes = 0;
    r.totalFiles = tree ? tree->clipCount() + 2 : 0;
    r.freeFiles = 0;
    return r;
}

- (void)mountWithOptions:(FSTaskOptions*)options replyHandler:(void (^)(NSError*))reply {
    reply(nil);
}

- (void)unmountWithReplyHandler:(void (^)(void))reply {
    reply();
}

- (void)synchronizeWithFlags:(FSSyncFlags)flags replyHandler:(void (^)(NSError*))reply {
    reply(nil);
}

- (void)activateWithOptions:(FSTaskOptions*)options replyHandler:(void (^)(FSItem*, NSError*))reply {
    if (NSError* error = [self openWithOptions:options]) {
        reply(nil, error);
        return;
    }
    reply(_root, nil);
}

- (void)deactivateWithOptions:(FSDeactivateOptions)options replyHandler:(void (^)(NSError*))reply {
    // FSKit has reclaimed every item but the root: no reads come any more.
    {
        std::lock_guard<std::mutex> lock(_itemsMutex);
        _items.clear();
    }
    _root = nil;
    if (_notifyToken != NOTIFY_TOKEN_INVALID) notify_cancel(_notifyToken);
    _notifyToken = NOTIFY_TOKEN_INVALID;
    dispatch_barrier_sync(_reads, ^{});  // a rescan request already queued has reached the watcher
    _watcher.reset();  // its thread stops (a rescan in progress finishes)
    _cache.reset();    // its workers stop
    {
        std::lock_guard<std::mutex> lock(_treeMutex);
        _tree.reset();  // the clips close
    }
    reply(nil);
}

- (void)getAttributes:(FSItemGetAttributesRequest*)desiredAttributes
               ofItem:(FSItem*)item
         replyHandler:(void (^)(FSItemAttributes*, NSError*))reply {
    reply([self attributesOf:((MLVItem*)item)->node], nil);
}

- (void)setAttributes:(FSItemSetAttributesRequest*)newAttributes
               onItem:(FSItem*)item
         replyHandler:(void (^)(FSItemAttributes*, NSError*))reply {
    reply(nil, posixError(EROFS));
}

- (void)lookupItemNamed:(FSFileName*)name
            inDirectory:(FSItem*)directory
           replyHandler:(void (^)(FSItem*, FSFileName*, NSError*))reply {
    const Node& dir = ((MLVItem*)directory)->node;
    NSData* data = name.data;
    const std::string utf8(static_cast<const char*>(data.bytes), data.length);
    if (dir.kind == Node::Folder) {
        if (dir.id == kRootId && utf8 == kNeverIndex) {
            Node n;
            n.kind = Node::NeverIndex;
            n.id = kNeverIndexId;
            n.parentId = kRootId;
            reply([self itemFor:n], name, nil);
            return;
        }
        // In the tree shown now (case-insensitive, as its names).
        const auto tree = [self current];
        const vfs::Node d = tree ? tree->find((dir.path + L"\\" + util::widen(utf8)).c_str()) : vfs::Node{};
        if (d.kind == vfs::Node::Directory) {
            const Node n = childFolder(*d.dir, dir);
            reply([self itemFor:n], [FSFileName nameWithCString:[self nameOf:n].c_str()], nil);
            return;
        }
    } else if (dir.kind == Node::ClipFolder) {
        const std::wstring w = util::widen(utf8);
        uint32_t frame = 0;
        const vfs::Node::Kind k = vfs::Tree::clipFile(*dir.clip, w.data(), w.size(), &frame);
        if (k != vfs::Node::None) {
            const Node n = clipFile(dir, k, frame);
            reply([self itemFor:n], [FSFileName nameWithCString:[self nameOf:n].c_str()], nil);
            return;
        }
    } else {
        reply(nil, nil, posixError(ENOTDIR));
        return;
    }
    reply(nil, nil, posixError(ENOENT));
}

- (void)reclaimItem:(FSItem*)item replyHandler:(void (^)(NSError*))reply {
    const uint64_t id = ((MLVItem*)item)->node.id;
    if (id != kRootId) {
        std::lock_guard<std::mutex> lock(_itemsMutex);
        _items.erase(id);
    }
    reply(nil);
}

- (void)readSymbolicLink:(FSItem*)item replyHandler:(void (^)(FSFileName*, NSError*))reply {
    reply(nil, posixError(EINVAL));
}

- (void)createItemNamed:(FSFileName*)name
                   type:(FSItemType)type
            inDirectory:(FSItem*)directory
             attributes:(FSItemSetAttributesRequest*)newAttributes
           replyHandler:(void (^)(FSItem*, FSFileName*, NSError*))reply {
    reply(nil, nil, posixError(EROFS));
}

- (void)createSymbolicLinkNamed:(FSFileName*)name
                    inDirectory:(FSItem*)directory
                     attributes:(FSItemSetAttributesRequest*)newAttributes
                   linkContents:(FSFileName*)contents
                   replyHandler:(void (^)(FSItem*, FSFileName*, NSError*))reply {
    reply(nil, nil, posixError(EROFS));
}

- (void)createLinkToItem:(FSItem*)item
                   named:(FSFileName*)name
             inDirectory:(FSItem*)directory
            replyHandler:(void (^)(FSFileName*, NSError*))reply {
    reply(nil, posixError(EROFS));
}

- (void)removeItem:(FSItem*)item
             named:(FSFileName*)name
     fromDirectory:(FSItem*)directory
      replyHandler:(void (^)(NSError*))reply {
    reply(posixError(EROFS));
}

- (void)renameItem:(FSItem*)item
       inDirectory:(FSItem*)sourceDirectory
             named:(FSFileName*)sourceName
         toNewName:(FSFileName*)destinationName
       inDirectory:(FSItem*)destinationDirectory
          overItem:(FSItem*)overItem
      replyHandler:(void (^)(FSFileName*, NSError*))reply {
    reply(nil, posixError(EROFS));
}

// Cookies: 0 ".", 1 "..", 2 + i the i-th child; with attributes "." and ".." are left out
// (FSKit's rule).
- (void)enumerateDirectory:(FSItem*)directory
          startingAtCookie:(FSDirectoryCookie)cookie
                  verifier:(FSDirectoryVerifier)verifier
       providingAttributes:(FSItemGetAttributesRequest*)attributes
               usingPacker:(FSDirectoryEntryPacker*)packer
              replyHandler:(void (^)(FSDirectoryVerifier, NSError*))reply {
    const Node& dir = ((MLVItem*)directory)->node;
    const FSDirectoryVerifier current = _generation.load();
    if (cookie != FSDirectoryCookieInitial && verifier != current) {  // the tree changed since: start again
        reply(current, [NSError errorWithDomain:FSKitErrorDomain code:FSErrorInvalidDirectoryCookie userInfo:nil]);
        return;
    }
    const std::vector<Node> children = [self childrenOf:dir];
    uint64_t i = cookie;
    if (attributes && i < 2) i = 2;
    for (; i < 2 + children.size(); ++i) {
        BOOL ok;
        if (i < 2) {
            ok = [packer packEntryWithName:[FSFileName nameWithCString:i == 0 ? "." : ".."]
                                  itemType:FSItemTypeDirectory
                                    itemID:FSItemID(i == 0 ? dir.id : dir.parentId)
                                nextCookie:i + 1
                                attributes:nil];
        } else {
            const Node& n = children[size_t(i - 2)];
            ok = [packer packEntryWithName:[FSFileName nameWithCString:[self nameOf:n].c_str()]
                                  itemType:n.isDirectory() ? FSItemTypeDirectory : FSItemTypeFile
                                    itemID:FSItemID(n.id)
                                nextCookie:i + 1
                                attributes:attributes ? [self attributesOf:n] : nil];
        }
        if (!ok) break;
    }
    reply(current, nil);
}

// --- FSVolumeXattrOperations (inhibited: not called) ---

- (void)getXattrNamed:(FSFileName*)name ofItem:(FSItem*)item replyHandler:(void (^)(NSData*, NSError*))reply {
    reply(nil, posixError(ENOATTR));
}

- (void)setXattrNamed:(FSFileName*)name
               toData:(NSData*)value
               onItem:(FSItem*)item
               policy:(FSSetXattrPolicy)policy
         replyHandler:(void (^)(NSError*))reply {
    reply(posixError(EROFS));
}

- (void)listXattrsOfItem:(FSItem*)item replyHandler:(void (^)(NSArray<FSFileName*>*, NSError*))reply {
    reply(@[], nil);
}

// --- FSVolumeReadWriteOperations ---

- (void)readFromFile:(FSItem*)fsItem
              offset:(off_t)offset
              length:(size_t)length
          intoBuffer:(FSMutableFileDataBuffer*)buffer
        replyHandler:(void (^)(size_t, NSError*))reply {
    MLVItem* item = (MLVItem*)fsItem;
    vfs::FrameCache* cache = _cache.get();
    dispatch_async(_reads, ^{
        const Node& n = item->node;
        uint8_t* out = static_cast<uint8_t*>(buffer.mutableBytes);
        const uint64_t size = n.kind == Node::Frame   ? n.clip->source->fileSize()
                              : n.kind == Node::Audio ? n.clip->wav.size()
                                                      : 0;
        if (offset < 0 || uint64_t(offset) >= size) {
            reply(0, nil);  // at or past the end
            return;
        }
        const uint64_t begin = uint64_t(offset);
        const size_t count = size_t(std::min<uint64_t>({uint64_t(length), uint64_t(buffer.length), size - begin}));
        std::string error;
        if (n.kind == Node::Audio) {
            // Straight from the clip's audio blocks.
            if (!mlv::readWav(*n.clip->clip, n.clip->wav, begin, count, out, &error)) {
                os_log_error(Log(), "%{public}s audio: %{public}s", util::narrow(n.clip->name).c_str(), error.c_str());
                reply(0, posixError(EIO));
                return;
            }
            reply(count, nil);
            return;
        }
        const vfs::FrameSource& source = *n.clip->source;
        if (begin + count <= source.headerSize()) {
            // The header alone (Finder, Quick Look and the like read the start of many files).
            std::vector<uint8_t> header;
            if (!source.renderHeader(n.frame, header) || header.size() < begin + count) {
                reply(0, posixError(EIO));
                return;
            }
            std::memcpy(out, header.data() + begin, count);
            reply(count, nil);
            return;
        }
        const vfs::FrameData file = cache->get(n.clip, n.frame, true, &error);
        if (!file || file->size() < begin + count) {  // (not the size listed: a bug)
            os_log_error(Log(), "%{public}s frame %u: %{public}s", util::narrow(n.clip->name).c_str(), n.frame,
                         error.c_str());
            reply(0, posixError(EIO));
            return;
        }
        std::memcpy(out, file->data() + begin, count);
        reply(count, nil);
    });
}

- (void)writeContents:(NSData*)contents
               toFile:(FSItem*)item
             atOffset:(off_t)offset
         replyHandler:(void (^)(size_t, NSError*))reply {
    reply(0, posixError(EROFS));
}

@end

// --- the file system: one volume per mount ---

@interface MLVDriveFileSystem : FSUnaryFileSystem <FSUnaryFileSystemOperations>
@end

@implementation MLVDriveFileSystem

- (void)probeResource:(FSResource*)resource replyHandler:(void (^)(FSProbeResult*, NSError*))reply {
    NSUUID* uuid = [[NSUUID alloc] initWithUUIDString:@"6D4C5644-5249-5645-0000-000000000001"];
    reply([FSProbeResult usableProbeResultWithName:@"MLV Drive"
                                       containerID:[[FSContainerIdentifier alloc] initWithUUID:uuid]],
          nil);
}

- (void)loadResource:(FSResource*)resource
             options:(FSTaskOptions*)options
        replyHandler:(void (^)(FSVolume*, NSError*))reply {
    if (![resource isKindOfClass:[FSPathURLResource class]]) {
        reply(nil, posixError(EINVAL));
        return;
    }
    NSURL* url = ((FSPathURLResource*)resource).url;
    [url startAccessingSecurityScopedResource];  // (when it is one)
    const std::string folder = url.path.UTF8String ?: "";
    std::vector<std::string> warnings;
    const std::wstring settingsFile = app::settingsPath();
    app::Settings settings = app::loadSettings(settingsFile, &warnings);
    for (const auto& w : warnings) os_log_error(Log(), "settings: %{public}s", w.c_str());
    // The settings folder: the settings' folders; any other: that folder alone.
    std::error_code ec;
    const bool isSettingsFolder =
        std::filesystem::equivalent(util::u8(folder), util::u8(util::narrow(settingsFile)).parent_path(), ec);
    if (!isSettingsFolder) settings.folders = {util::widen(folder)};
    os_log(Log(), "loading %{public}s (%zu folder(s))", folder.c_str(), settings.folders.size());
    self.containerStatus = [FSContainerStatus ready];
    reply([[MLVVolume alloc] initWithSettings:settings source:url.path], nil);
}

- (void)unloadResource:(FSResource*)resource options:(FSTaskOptions*)options replyHandler:(void (^)(NSError*))reply {
    reply(nil);
}

@end

int main(int argc, const char* argv[]) {
    return NSExtensionMain(argc, argv);
}
