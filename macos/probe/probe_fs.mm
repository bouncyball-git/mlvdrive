// MLV Drive probe: a throwaway FSKit module for measuring how fast FSKit can serve MLV Drive's kind
// of volume before the real one is written (PLAN.md, milestone 8, step 2). A read-only volume of
// N files of a DNG's size (f_000000.dng ...), their bytes one memcpy from a pattern (probe_pattern.h),
// and probe_report.txt: the request counts so far, the options, and whether the extension could read
// its folder and a second one (the sandbox question).
//
// Mount options (-o key=value,...):
//   files=1000        files in the root
//   size=4148416      bytes a file (a 1080p 16-bit DNG)
//   iosize=1048576    the volume's preferred I/O size (statfs f_iosize)
//   reply=async       async: reads answered from a worker queue (FSKit's thread returns at once);
//                     sync: answered on FSKit's thread
//   make_us=0         CPU time spent the first time a file is read (a frame being made), busy, so
//                     the workers' QoS (which cores they get) shows
//   qos=initiated     the workers' QoS: interactive, initiated, utility, background
//   openclose=0       1: receive open / close calls (counted); 0: inhibited (two round trips fewer)
//   second=/path      a second folder the extension tries to read (reported)

#import <FSKit/FSKit.h>
#import <Foundation/Foundation.h>
#import <os/log.h>

#include "probe_pattern.h"

#include <mach/mach_time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern "C" int NSExtensionMain(int argc, const char* argv[]);

namespace {

os_log_t Log() {
    static os_log_t log = os_log_create("org.mlvdrive.probe", "fs");
    return log;
}

constexpr FSItemID kRootId = FSItemIDRootDirectory;  // 2
constexpr uint64_t kReportId = 3;
constexpr uint64_t kFirstFileId = 16;
constexpr size_t kReportSize = 65536;
constexpr FSDirectoryVerifier kVerifier = 1;

struct Options {
    bool given = false;  // some key=value was found
    uint32_t files = 1000;
    uint64_t size = 4148416;
    int64_t ioSize = 1 << 20;
    bool async = true;
    uint32_t makeUs = 0;
    std::string qos = "initiated";
    bool openClose = false;
    std::string second;
};

struct Stats {
    std::atomic<uint64_t> lookups{0}, lookupMisses{0}, getAttrs{0}, enumerates{0}, packed{0};
    std::atomic<uint64_t> reads{0}, readBytes{0}, minRead{UINT64_MAX}, maxRead{0}, readsPastEnd{0};
    std::atomic<uint64_t> opens{0}, closes{0}, reclaims{0}, xattrs{0}, made{0};
    std::atomic<int64_t> inFlight{0}, maxInFlight{0};
    std::atomic<uint32_t> fsThreads{0}, workerThreads{0};
};

void noteMax(std::atomic<int64_t>& m, int64_t v) {
    int64_t cur = m.load(std::memory_order_relaxed);
    while (v > cur && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}
void noteMin(std::atomic<uint64_t>& m, uint64_t v) {
    uint64_t cur = m.load(std::memory_order_relaxed);
    while (v < cur && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}
void noteMaxU(std::atomic<uint64_t>& m, uint64_t v) {
    uint64_t cur = m.load(std::memory_order_relaxed);
    while (v > cur && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

// Where mount options arrived (load, mount, activate) and as what: shown in the report.
std::mutex g_optionLogMutex;
std::string g_optionLog;

void logOptions(const char* stage, FSTaskOptions* task) {
    std::string line = std::string(stage) + ":";
    for (NSString* s in task.taskOptions) line += std::string(" [") + (s.UTF8String ? s.UTF8String : "") + "]";
    std::lock_guard<std::mutex> lock(g_optionLogMutex);
    g_optionLog += line + "\n           ";
}

std::string optionLog() {
    std::lock_guard<std::mutex> lock(g_optionLogMutex);
    return g_optionLog;
}

uint64_t parseNumber(const std::string& v, uint64_t fallback) {
    char* end = nullptr;
    const unsigned long long n = std::strtoull(v.c_str(), &end, 10);
    return end && *end == 0 && !v.empty() ? uint64_t(n) : fallback;
}

Options parseOptions(FSTaskOptions* task) {
    Options o;
    for (NSString* s in task.taskOptions) {
        // "-o" and "a=b,c=d" may come as separate strings or as one ("-oa=b"): take the key=value parts.
        std::string text = s.UTF8String ? s.UTF8String : "";
        if (text.rfind("-o", 0) == 0) text = text.substr(2);
        size_t start = 0;
        while (start <= text.size()) {
            size_t end = text.find(',', start);
            if (end == std::string::npos) end = text.size();
            const std::string kv = text.substr(start, end - start);
            const size_t eq = kv.find('=');
            if (eq != std::string::npos) {
                const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
                o.given = true;
                if (k == "files") o.files = uint32_t(std::clamp<uint64_t>(parseNumber(v, o.files), 1, 1000000));
                else if (k == "size") o.size = std::clamp<uint64_t>(parseNumber(v, o.size), 1, 1ull << 30);
                else if (k == "iosize") o.ioSize = int64_t(std::clamp<uint64_t>(parseNumber(v, uint64_t(o.ioSize)), 4096, 64ull << 20));
                else if (k == "reply") o.async = v != "sync";
                else if (k == "make_us") o.makeUs = uint32_t(std::min<uint64_t>(parseNumber(v, 0), 1000000));
                else if (k == "qos") o.qos = v;
                else if (k == "openclose") o.openClose = v == "1" || v == "true";
                else if (k == "second") o.second = v;
            }
            start = end + 1;
        }
    }
    o.ioSize -= o.ioSize % 4096;  // a multiple of the block size
    return o;
}

qos_class_t qosOf(const std::string& name) {
    if (name == "interactive") return QOS_CLASS_USER_INTERACTIVE;
    if (name == "utility") return QOS_CLASS_UTILITY;
    if (name == "background") return QOS_CLASS_BACKGROUND;
    if (name == "default") return QOS_CLASS_DEFAULT;
    return QOS_CLASS_USER_INITIATED;
}

const char* qosName(qos_class_t q) {
    switch (q) {
        case QOS_CLASS_USER_INTERACTIVE: return "interactive";
        case QOS_CLASS_USER_INITIATED: return "initiated";
        case QOS_CLASS_DEFAULT: return "default";
        case QOS_CLASS_UTILITY: return "utility";
        case QOS_CLASS_BACKGROUND: return "background";
        default: return "unspecified";
    }
}

// Whether the extension can list a folder: "readable, N entries" or the error.
std::string tryList(const std::string& path) {
    if (path.empty()) return "(none given)";
    DIR* d = opendir(path.c_str());
    if (!d) return std::string("NOT readable: ") + std::strerror(errno);
    size_t n = 0;
    while (readdir(d)) ++n;
    closedir(d);
    return "readable, " + std::to_string(n) + " entries";
}

void spinMicroseconds(uint32_t us) {
    if (!us) return;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    const uint64_t ticks = uint64_t(us) * 1000 * tb.denom / tb.numer;
    const uint64_t until = mach_absolute_time() + ticks;
    volatile uint64_t x = 0;
    while (mach_absolute_time() < until) x = x + 1;
}

}  // namespace

// --- items: one per node, made once at load and kept (reclaim drops nothing) ---

@interface ProbeItem : FSItem
@property (nonatomic, readonly) uint64_t itemId;
@property (nonatomic, readonly) FSFileName* name;
- (instancetype)initWithId:(uint64_t)itemId name:(FSFileName*)name;
@end

@implementation ProbeItem
- (instancetype)initWithId:(uint64_t)itemId name:(FSFileName*)name {
    if ((self = [super init])) {
        _itemId = itemId;
        _name = name;
    }
    return self;
}
@end

// --- the volume ---

@interface ProbeVolume : FSVolume <FSVolumeOperations, FSVolumeReadWriteOperations, FSVolumeOpenCloseOperations,
                                   FSVolumeXattrOperations>
- (instancetype)initWithOptions:(const Options&)options resourceInfo:(NSString*)info;
@end

@implementation ProbeVolume {
    Options _options;
    Stats _stats;
    std::vector<uint8_t> _pattern;
    std::unique_ptr<std::atomic<uint8_t>[]> _made;  // 0 not made, 1 being made, 2 made
    ProbeItem* _root;
    ProbeItem* _report;
    std::vector<ProbeItem*> _files;
    struct timespec _time;
    dispatch_queue_t _workers;
    std::atomic<unsigned> _fsQos;  // qos_class_t of FSKit's read thread
    NSString* _resourceInfo;
}

@synthesize xattrOperationsInhibited = _xattrOperationsInhibited;
@synthesize openCloseInhibited = _openCloseInhibited;
@synthesize requestedMountOptions = _requestedMountOptions;

- (instancetype)initWithOptions:(const Options&)options resourceInfo:(NSString*)info {
    self = [super initWithVolumeID:[[FSVolumeIdentifier alloc] init] volumeName:[FSFileName nameWithString:@"MLV Probe"]];
    if (!self) return nil;
    _resourceInfo = info;
    clock_gettime(CLOCK_REALTIME, &_time);
    _root = [[ProbeItem alloc] initWithId:kRootId name:[FSFileName nameWithString:@"/"]];
    _report = [[ProbeItem alloc] initWithId:kReportId name:[FSFileName nameWithCString:"probe_report.txt"]];
    [self configure:options];
    _xattrOperationsInhibited = YES;  // none: answered without a round trip
    _openCloseInhibited = !options.openClose;  // read once, after loadResource
    _requestedMountOptions = FSMountOptionsReadOnly;
    _fsQos.store(QOS_CLASS_UNSPECIFIED);
    return self;
}

// The files and workers for `options` (again when options come with mount / activate, before any
// item is handed out).
- (void)configure:(const Options&)options {
    _options = options;
    _pattern = probe::makePattern(size_t(options.size));
    _made.reset(new std::atomic<uint8_t>[options.files]);
    for (uint32_t i = 0; i < options.files; ++i) _made[i].store(0, std::memory_order_relaxed);
    _files.clear();
    _files.reserve(options.files);
    for (uint32_t i = 0; i < options.files; ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "f_%06u.dng", i);
        _files.push_back([[ProbeItem alloc] initWithId:kFirstFileId + i name:[FSFileName nameWithCString:name]]);
    }
    dispatch_queue_attr_t attr =
        dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_CONCURRENT, qosOf(options.qos), 0);
    _workers = dispatch_queue_create("org.mlvdrive.probe.workers", attr);
}

- (ProbeItem*)itemFor:(FSItem*)item {
    return (ProbeItem*)item;
}

- (uint64_t)sizeOf:(ProbeItem*)item {
    if (item.itemId == kReportId) return kReportSize;
    if (item.itemId >= kFirstFileId) return _options.size;
    return 0;
}

- (FSItemAttributes*)attributesOf:(ProbeItem*)item {
    FSItemAttributes* a = [[FSItemAttributes alloc] init];
    const bool dir = item.itemId == kRootId;
    a.type = dir ? FSItemTypeDirectory : FSItemTypeFile;
    a.mode = dir ? (S_IFDIR | 0555) : (S_IFREG | 0444);
    a.linkCount = dir ? 2 : 1;
    a.uid = getuid();
    a.gid = getgid();
    a.flags = 0;
    a.size = [self sizeOf:item];
    a.allocSize = (a.size + 4095) / 4096 * 4096;
    a.fileID = FSItemID(item.itemId);
    a.parentID = dir ? FSItemIDParentOfRoot : kRootId;
    a.modifyTime = a.changeTime = a.accessTime = a.birthTime = _time;
    a.supportsLimitedXAttrs = NO;
    a.inhibitKernelOffloadedIO = YES;
    return a;
}

- (std::string)reportText {
    const Stats& s = _stats;
    char buf[4096];
    const uint64_t reads = s.reads.load();
    std::snprintf(buf, sizeof buf,
                  "MLV Drive probe\n"
                  "options    files=%u size=%llu iosize=%lld reply=%s make_us=%u qos=%s openclose=%d\n"
                  "arrived    %s\n"
                  "resource   %s\n"
                  "second     %s: %s\n"
                  "requests   lookups %llu (misses %llu), getattr %llu, enumerate %llu (entries %llu), "
                  "xattr %llu, open %llu, close %llu, reclaim %llu\n"
                  "reads      %llu, %.1f MB, size min %llu max %llu mean %.0f, past end %llu, "
                  "most in flight %lld\n"
                  "files made %llu\n"
                  "threads    FSKit's %u (QoS %s), workers %u\n",
                  _options.files, (unsigned long long)_options.size, (long long)_options.ioSize,
                  _options.async ? "async" : "sync", _options.makeUs, _options.qos.c_str(), int(_options.openClose),
                  optionLog().c_str(), _resourceInfo.UTF8String, _options.second.empty() ? "-" : _options.second.c_str(),
                  tryList(_options.second).c_str(), (unsigned long long)s.lookups.load(),
                  (unsigned long long)s.lookupMisses.load(), (unsigned long long)s.getAttrs.load(),
                  (unsigned long long)s.enumerates.load(), (unsigned long long)s.packed.load(),
                  (unsigned long long)s.xattrs.load(), (unsigned long long)s.opens.load(),
                  (unsigned long long)s.closes.load(), (unsigned long long)s.reclaims.load(),
                  (unsigned long long)reads, double(s.readBytes.load()) / 1e6,
                  (unsigned long long)(reads ? s.minRead.load() : 0), (unsigned long long)s.maxRead.load(),
                  reads ? double(s.readBytes.load()) / double(reads) : 0.0, (unsigned long long)s.readsPastEnd.load(),
                  (long long)s.maxInFlight.load(), (unsigned long long)s.made.load(), s.fsThreads.load(),
                  qosName(qos_class_t(_fsQos.load())), s.workerThreads.load());
    std::string text = buf;
    text.resize(kReportSize - 1, ' ');
    text.push_back('\n');
    return text;
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
    c.caseFormat = FSVolumeCaseFormatInsensitiveCasePreserving;
    return c;
}

- (FSStatFSResult*)volumeStatistics {
    FSStatFSResult* r = [[FSStatFSResult alloc] initWithFileSystemTypeName:@"mlvprobe"];
    r.blockSize = 4096;
    r.ioSize = NSInteger(_options.ioSize);
    r.totalBytes = r.usedBytes = uint64_t(_options.files) * _options.size + kReportSize;
    r.freeBytes = r.availableBytes = 0;
    r.totalFiles = _options.files + 2;
    r.freeFiles = 0;
    return r;
}

- (void)mountWithOptions:(FSTaskOptions*)options replyHandler:(void (^)(NSError*))reply {
    logOptions("mount", options);
    const Options o = parseOptions(options);
    if (o.given) [self configure:o];
    reply(nil);
}

- (void)unmountWithReplyHandler:(void (^)(void))reply {
    reply();
}

- (void)synchronizeWithFlags:(FSSyncFlags)flags replyHandler:(void (^)(NSError*))reply {
    reply(nil);
}

- (void)activateWithOptions:(FSTaskOptions*)options replyHandler:(void (^)(FSItem*, NSError*))reply {
    logOptions("activate", options);
    const Options o = parseOptions(options);
    if (o.given) [self configure:o];
    reply(_root, nil);
}

- (void)deactivateWithOptions:(FSDeactivateOptions)options replyHandler:(void (^)(NSError*))reply {
    reply(nil);
}

- (void)getAttributes:(FSItemGetAttributesRequest*)desiredAttributes
               ofItem:(FSItem*)item
         replyHandler:(void (^)(FSItemAttributes*, NSError*))reply {
    _stats.getAttrs.fetch_add(1, std::memory_order_relaxed);
    reply([self attributesOf:[self itemFor:item]], nil);
}

- (void)setAttributes:(FSItemSetAttributesRequest*)newAttributes
               onItem:(FSItem*)item
         replyHandler:(void (^)(FSItemAttributes*, NSError*))reply {
    reply(nil, fs_errorForPOSIXError(EROFS));
}

- (void)lookupItemNamed:(FSFileName*)name
            inDirectory:(FSItem*)directory
           replyHandler:(void (^)(FSItem*, FSFileName*, NSError*))reply {
    _stats.lookups.fetch_add(1, std::memory_order_relaxed);
    NSData* data = name.data;
    const char* p = static_cast<const char*>(data.bytes);
    const size_t n = data.length;
    // f_NNNNNN.dng (any case), probe_report.txt
    if (n == 12 && (p[0] == 'f' || p[0] == 'F') && p[1] == '_' && strncasecmp(p + 8, ".dng", 4) == 0) {
        uint32_t index = 0;
        bool digits = true;
        for (int i = 2; i < 8; ++i) {
            if (p[i] < '0' || p[i] > '9') digits = false;
            index = index * 10 + uint32_t(p[i] - '0');
        }
        if (digits && index < _files.size()) {
            ProbeItem* item = _files[index];
            reply(item, item.name, nil);
            return;
        }
    } else if (n == 16 && strncasecmp(p, "probe_report.txt", 16) == 0) {
        reply(_report, _report.name, nil);
        return;
    }
    _stats.lookupMisses.fetch_add(1, std::memory_order_relaxed);
    reply(nil, nil, fs_errorForPOSIXError(ENOENT));
}

- (void)reclaimItem:(FSItem*)item replyHandler:(void (^)(NSError*))reply {
    _stats.reclaims.fetch_add(1, std::memory_order_relaxed);
    reply(nil);  // the items live as long as the volume
}

- (void)readSymbolicLink:(FSItem*)item replyHandler:(void (^)(FSFileName*, NSError*))reply {
    reply(nil, fs_errorForPOSIXError(EINVAL));
}

- (void)createItemNamed:(FSFileName*)name
                   type:(FSItemType)type
            inDirectory:(FSItem*)directory
             attributes:(FSItemSetAttributesRequest*)newAttributes
           replyHandler:(void (^)(FSItem*, FSFileName*, NSError*))reply {
    reply(nil, nil, fs_errorForPOSIXError(EROFS));
}

- (void)createSymbolicLinkNamed:(FSFileName*)name
                    inDirectory:(FSItem*)directory
                     attributes:(FSItemSetAttributesRequest*)newAttributes
                   linkContents:(FSFileName*)contents
                   replyHandler:(void (^)(FSItem*, FSFileName*, NSError*))reply {
    reply(nil, nil, fs_errorForPOSIXError(EROFS));
}

- (void)createLinkToItem:(FSItem*)item
                   named:(FSFileName*)name
             inDirectory:(FSItem*)directory
            replyHandler:(void (^)(FSFileName*, NSError*))reply {
    reply(nil, fs_errorForPOSIXError(EROFS));
}

- (void)removeItem:(FSItem*)item
             named:(FSFileName*)name
     fromDirectory:(FSItem*)directory
      replyHandler:(void (^)(NSError*))reply {
    reply(fs_errorForPOSIXError(EROFS));
}

- (void)renameItem:(FSItem*)item
       inDirectory:(FSItem*)sourceDirectory
             named:(FSFileName*)sourceName
         toNewName:(FSFileName*)destinationName
       inDirectory:(FSItem*)destinationDirectory
          overItem:(FSItem*)overItem
      replyHandler:(void (^)(FSFileName*, NSError*))reply {
    reply(nil, fs_errorForPOSIXError(EROFS));
}

// Entries by cookie: without attributes 0 ".", 1 "..", then 2 the report and 3... the files; with
// attributes the same numbers, "." and ".." left out (FSKit's rule).
- (void)enumerateDirectory:(FSItem*)directory
          startingAtCookie:(FSDirectoryCookie)cookie
                  verifier:(FSDirectoryVerifier)verifier
       providingAttributes:(FSItemGetAttributesRequest*)attributes
               usingPacker:(FSDirectoryEntryPacker*)packer
              replyHandler:(void (^)(FSDirectoryVerifier, NSError*))reply {
    _stats.enumerates.fetch_add(1, std::memory_order_relaxed);
    if (cookie != FSDirectoryCookieInitial && verifier != kVerifier) {
        reply(kVerifier, [NSError errorWithDomain:FSKitErrorDomain code:FSErrorInvalidDirectoryCookie userInfo:nil]);
        return;
    }
    const uint64_t end = 3 + _files.size();
    uint64_t i = cookie;
    if (attributes && i < 2) i = 2;
    static FSFileName* dot = [FSFileName nameWithCString:"."];
    static FSFileName* dotdot = [FSFileName nameWithCString:".."];
    uint64_t packed = 0;
    for (; i < end; ++i) {
        BOOL ok;
        if (i < 2) {
            ok = [packer packEntryWithName:i == 0 ? dot : dotdot
                                  itemType:FSItemTypeDirectory
                                  itemID:i == 0 ? kRootId : FSItemIDParentOfRoot
                                nextCookie:i + 1
                                attributes:nil];
        } else {
            ProbeItem* item = i == 2 ? _report : _files[size_t(i - 3)];
            ok = [packer packEntryWithName:item.name
                                  itemType:FSItemTypeFile
                                    itemID:FSItemID(item.itemId)
                                nextCookie:i + 1
                                attributes:attributes ? [self attributesOf:item] : nil];
        }
        if (!ok) break;
        ++packed;
    }
    _stats.packed.fetch_add(packed, std::memory_order_relaxed);
    reply(kVerifier, nil);
}

// --- FSVolumeXattrOperations (inhibited: these aren't called) ---

- (void)getXattrNamed:(FSFileName*)name ofItem:(FSItem*)item replyHandler:(void (^)(NSData*, NSError*))reply {
    _stats.xattrs.fetch_add(1, std::memory_order_relaxed);
    reply(nil, fs_errorForPOSIXError(ENOATTR));
}

- (void)setXattrNamed:(FSFileName*)name
               toData:(NSData*)value
               onItem:(FSItem*)item
               policy:(FSSetXattrPolicy)policy
         replyHandler:(void (^)(NSError*))reply {
    reply(fs_errorForPOSIXError(EROFS));
}

- (void)listXattrsOfItem:(FSItem*)item replyHandler:(void (^)(NSArray<FSFileName*>*, NSError*))reply {
    _stats.xattrs.fetch_add(1, std::memory_order_relaxed);
    reply(@[], nil);
}

// --- FSVolumeOpenCloseOperations (inhibited unless openclose=1) ---

- (void)openItem:(FSItem*)item withModes:(FSVolumeOpenModes)modes replyHandler:(void (^)(NSError*))reply {
    _stats.opens.fetch_add(1, std::memory_order_relaxed);
    reply((modes & FSVolumeOpenModesWrite) ? fs_errorForPOSIXError(EROFS) : nil);
}

- (void)closeItem:(FSItem*)item keepingModes:(FSVolumeOpenModes)modes replyHandler:(void (^)(NSError*))reply {
    _stats.closes.fetch_add(1, std::memory_order_relaxed);
    reply(nil);
}

// --- FSVolumeReadWriteOperations ---

// Copies [offset, offset + length) of `item` into `buffer`; the bytes copied (0 only at or past
// the end).
- (size_t)fill:(FSMutableFileDataBuffer*)buffer item:(ProbeItem*)item offset:(off_t)offset length:(size_t)length {
    const uint64_t size = [self sizeOf:item];
    if (offset < 0 || uint64_t(offset) >= size) {
        _stats.readsPastEnd.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    const size_t n = size_t(std::min<uint64_t>(std::min<uint64_t>(length, buffer.length), size - uint64_t(offset)));
    uint8_t* out = static_cast<uint8_t*>(buffer.mutableBytes);
    if (item.itemId == kReportId) {
        const std::string text = [self reportText];
        std::memcpy(out, text.data() + offset, n);
    } else {
        const uint64_t file = item.itemId - kFirstFileId;
        std::memcpy(out, _pattern.data() + probe::shift(file) + size_t(offset), n);
    }
    _stats.readBytes.fetch_add(n, std::memory_order_relaxed);
    noteMin(_stats.minRead, n);
    noteMaxU(_stats.maxRead, n);
    return n;
}

// The first read of a file "makes" it (make_us of CPU); others wait for that.
- (void)makeFile:(ProbeItem*)item {
    if (item.itemId < kFirstFileId || !_options.makeUs) return;
    std::atomic<uint8_t>& state = _made[item.itemId - kFirstFileId];
    uint8_t expected = 0;
    if (state.compare_exchange_strong(expected, 1)) {
        spinMicroseconds(_options.makeUs);
        _stats.made.fetch_add(1, std::memory_order_relaxed);
        state.store(2, std::memory_order_release);
    } else {
        while (state.load(std::memory_order_acquire) != 2) sched_yield();
    }
}

- (void)readFromFile:(FSItem*)fsItem
              offset:(off_t)offset
              length:(size_t)length
          intoBuffer:(FSMutableFileDataBuffer*)buffer
        replyHandler:(void (^)(size_t, NSError*))reply {
    static thread_local bool seenFs = false;
    if (!seenFs) {
        seenFs = true;
        _stats.fsThreads.fetch_add(1, std::memory_order_relaxed);
        _fsQos.store(unsigned(qos_class_self()));
    }
    _stats.reads.fetch_add(1, std::memory_order_relaxed);
    noteMax(_stats.maxInFlight, _stats.inFlight.fetch_add(1, std::memory_order_relaxed) + 1);
    ProbeItem* item = [self itemFor:fsItem];
    if (!_options.async) {
        [self makeFile:item];
        const size_t n = [self fill:buffer item:item offset:offset length:length];
        _stats.inFlight.fetch_sub(1, std::memory_order_relaxed);
        reply(n, nil);
        return;
    }
    dispatch_async(_workers, ^{
        static thread_local bool seenWorker = false;
        if (!seenWorker) {
            seenWorker = true;
            self->_stats.workerThreads.fetch_add(1, std::memory_order_relaxed);
        }
        [self makeFile:item];
        const size_t n = [self fill:buffer item:item offset:offset length:length];
        self->_stats.inFlight.fetch_sub(1, std::memory_order_relaxed);
        reply(n, nil);
    });
}

- (void)writeContents:(NSData*)contents
               toFile:(FSItem*)item
             atOffset:(off_t)offset
         replyHandler:(void (^)(size_t, NSError*))reply {
    reply(0, fs_errorForPOSIXError(EROFS));
}

@end

// --- the file system: one volume per mount ---

@interface ProbeFileSystem : FSUnaryFileSystem <FSUnaryFileSystemOperations>
@end

@implementation ProbeFileSystem

- (void)probeResource:(FSResource*)resource replyHandler:(void (^)(FSProbeResult*, NSError*))reply {
    NSUUID* uuid = [[NSUUID alloc] initWithUUIDString:@"6D4C5644-5052-4F42-4500-000000000001"];
    reply([FSProbeResult usableProbeResultWithName:@"MLV Probe"
                                       containerID:[[FSContainerIdentifier alloc] initWithUUID:uuid]],
          nil);
}

- (void)loadResource:(FSResource*)resource
             options:(FSTaskOptions*)options
        replyHandler:(void (^)(FSVolume*, NSError*))reply {
    logOptions("load", options);
    const Options o = parseOptions(options);
    NSString* info = @"(no path)";
    NSURL* url = nil;
    if ([resource isKindOfClass:[FSPathURLResource class]]) url = ((FSPathURLResource*)resource).url;
    else if ([resource isKindOfClass:[FSGenericURLResource class]]) url = ((FSGenericURLResource*)resource).url;
    if (url) {
        const BOOL scoped = [url startAccessingSecurityScopedResource];
        info = [NSString stringWithFormat:@"%@ %@ (security-scoped: %s): %s", NSStringFromClass([resource class]),
                                          url.path, scoped ? "yes" : "no", tryList(url.path.UTF8String ?: "").c_str()];
    }
    os_log(Log(), "load: %{public}@", info);
    self.containerStatus = [FSContainerStatus ready];
    reply([[ProbeVolume alloc] initWithOptions:o resourceInfo:info], nil);
}

- (void)unloadResource:(FSResource*)resource options:(FSTaskOptions*)options replyHandler:(void (^)(NSError*))reply {
    reply(nil);
}

- (void)didFinishLoading {
    os_log(Log(), "module loaded");
}

@end

int main(int argc, const char* argv[]) {
    return NSExtensionMain(argc, argv);
}
