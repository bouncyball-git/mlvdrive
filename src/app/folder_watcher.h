// Watching the MLV folders for clips added, removed or changed (macOS: FSEvents; Windows has its
// own in app/mounter.cpp).
//
// `changed` is called on the watcher's own thread once the folders have been quiet for 2 seconds
// after a change (a clip being copied shows once it is complete), at once after rescanNow(), and
// every 30 seconds while some folder may change unreported (not on a local disk: SMB / NFS
// notifications are unreliable). A folder that isn't there (an unplugged drive) is looked for
// every 5 seconds; coming or going counts as a change. Local folders aren't polled (that would keep
// sleeping disks awake).
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace app {

class FolderWatcher {
public:
    // `watch` false: no FSEvents and no polling, only rescanNow().
    FolderWatcher(std::vector<std::wstring> folders, bool watch, std::function<void()> changed);
    ~FolderWatcher();
    FolderWatcher(const FolderWatcher&) = delete;
    FolderWatcher& operator=(const FolderWatcher&) = delete;

    void rescanNow();
    bool polling() const { return polling_; }

private:
    struct Events;
    void noteChange();
    void startEvents();
    void stopEvents();
    bool foldersMoved();
    void loop();

    const std::vector<std::wstring> folders_;
    const bool watch_;
    const std::function<void()> changed_;
    std::vector<bool> exists_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false, dirty_ = false, now_ = false;
    std::atomic<bool> polling_{false};
    Events* events_ = nullptr;
    std::thread thread_;
};

}  // namespace app
