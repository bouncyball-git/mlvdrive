#include "app/folder_watcher.h"

#include "util/file.h"

#include <CoreServices/CoreServices.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include <chrono>

namespace app {

namespace {

// Not on a local disk: changes may go unreported.
bool isRemote(const std::wstring& folder) {
    struct statfs s;
    if (::statfs(util::narrow(folder).c_str(), &s) != 0) return false;
    return (s.f_flags & MNT_LOCAL) == 0;
}

bool folderExists(const std::wstring& folder) {
    struct stat s;
    return ::stat(util::narrow(folder).c_str(), &s) == 0 && S_ISDIR(s.st_mode);
}

}  // namespace

struct FolderWatcher::Events {
    FSEventStreamRef stream = nullptr;
    dispatch_queue_t queue = nullptr;
};

FolderWatcher::FolderWatcher(std::vector<std::wstring> folders, bool watch, std::function<void()> changed)
    : folders_(std::move(folders)), watch_(watch), changed_(std::move(changed)) {
    events_ = new Events;
    if (watch_) {
        for (const auto& f : folders_) {
            if (isRemote(f)) polling_ = true;
            exists_.push_back(folderExists(f));
        }
        startEvents();
    }
    thread_ = std::thread([this] { loop(); });
}

FolderWatcher::~FolderWatcher() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        wake_.notify_all();
    }
    thread_.join();
    stopEvents();
    if (events_->queue) dispatch_release(events_->queue);
    delete events_;
}

void FolderWatcher::rescanNow() {
    std::lock_guard<std::mutex> lock(mutex_);
    now_ = true;
    wake_.notify_all();
}

void FolderWatcher::noteChange() {
    std::lock_guard<std::mutex> lock(mutex_);
    dirty_ = true;
    wake_.notify_all();
}

void FolderWatcher::startEvents() {
    CFMutableArrayRef paths = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
    for (const auto& f : folders_) {
        if (CFStringRef p = CFStringCreateWithCString(nullptr, util::narrow(f).c_str(), kCFStringEncodingUTF8)) {
            CFArrayAppendValue(paths, p);
            CFRelease(p);
        }
    }
    FSEventStreamContext context{0, this, nullptr, nullptr, nullptr};
    auto callback = [](ConstFSEventStreamRef, void* self, size_t, void*, const FSEventStreamEventFlags*,
                       const FSEventStreamEventId*) { static_cast<FolderWatcher*>(self)->noteChange(); };
    events_->stream = FSEventStreamCreate(nullptr, callback, &context, paths, kFSEventStreamEventIdSinceNow, 0.5,
                                          kFSEventStreamCreateFlagNoDefer | kFSEventStreamCreateFlagWatchRoot);
    CFRelease(paths);
    if (!events_->stream) {
        polling_ = true;  // can't be watched: looked at every 30 seconds
        return;
    }
    if (!events_->queue) events_->queue = dispatch_queue_create("org.mlvdrive.watch", DISPATCH_QUEUE_SERIAL);
    FSEventStreamSetDispatchQueue(events_->stream, events_->queue);
    if (!FSEventStreamStart(events_->stream)) {
        FSEventStreamInvalidate(events_->stream);
        FSEventStreamRelease(events_->stream);
        events_->stream = nullptr;
        polling_ = true;
    }
}

void FolderWatcher::stopEvents() {
    if (!events_->stream) return;
    FSEventStreamStop(events_->stream);
    FSEventStreamInvalidate(events_->stream);
    FSEventStreamRelease(events_->stream);
    events_->stream = nullptr;
}

bool FolderWatcher::foldersMoved() {
    bool moved = false;
    for (size_t i = 0; i < folders_.size(); ++i) {
        const bool now = folderExists(folders_[i]);
        if (now != exists_[i]) moved = true;
        exists_[i] = now;
    }
    return moved;
}

void FolderWatcher::loop() {
    using namespace std::chrono;
    auto due = steady_clock::now() + seconds(30);
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        wake_.wait_for(lock, seconds(5), [this] { return stop_ || dirty_ || now_; });
        if (stop_) return;
        bool go = false;
        if (now_) {
            go = true;
        } else if (dirty_) {
            // Until the folders have been quiet for 2 seconds (a copy in progress keeps them busy).
            while (dirty_ && !now_) {
                dirty_ = false;
                wake_.wait_for(lock, seconds(2), [this] { return stop_ || dirty_ || now_; });
                if (stop_) return;
            }
            go = true;
        } else if (watch_) {
            lock.unlock();
            const bool moved = foldersMoved();
            if (moved) {  // FSEvents again, for folders that came back
                stopEvents();
                startEvents();
            }
            lock.lock();
            if (moved) {
                dirty_ = true;
                continue;
            }
            go = polling_ && steady_clock::now() >= due;
        }
        if (!go) continue;
        dirty_ = now_ = false;
        due = steady_clock::now() + seconds(30);
        lock.unlock();
        changed_();
        lock.lock();
    }
}

}  // namespace app
