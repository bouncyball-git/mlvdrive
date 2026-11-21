#include "vfs/frame_cache.h"

#include <algorithm>

namespace vfs {

FrameCache::FrameCache(uint64_t maxBytes, unsigned threads, unsigned depth)
    : maxBytes_(maxBytes), depth_(threads ? depth : 0) {
    for (unsigned i = 0; i < threads && depth_; ++i) workers_.emplace_back([this] { worker(); });
}

FrameCache::~FrameCache() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        queue_.clear();
    }
    work_.notify_all();
    for (auto& t : workers_) t.join();
}

FrameData FrameCache::make(const ClipEntry& clip, uint32_t frame, std::string* error) {
    auto data = std::make_shared<std::vector<uint8_t>>();
    if (!clip.source->renderFrame(frame, *data, error)) return nullptr;
    return data;
}

FrameData FrameCache::get(const ClipEntry& clip, uint32_t frame, bool readAhead, std::string* error) {
    const Key key{&clip, frame};
    std::unique_lock<std::mutex> lock(mutex_);
    if (readAhead) queueAhead(clip, frame);
    bool waited = false;
    for (;;) {
        auto it = entries_.find(key);
        if (it == entries_.end()) break;  // not there (or making it failed): made below
        Entry& e = it->second;
        if (e.data) {
            lru_.splice(lru_.begin(), lru_, e.lru);
            if (e.ahead) {
                e.ahead = false;
                ++stats_.readAheadUsed;
            }
            if (!waited) ++stats_.hits;
            return e.data;
        }
        if (!waited) {
            waited = true;
            ++stats_.waits;
        }
        made_.wait(lock);
    }

    entries_.emplace(key, Entry{});  // being made: others wait for it
    ++stats_.misses;
    lock.unlock();
    std::string why;
    FrameData data = make(clip, frame, &why);
    finish(key, data, why, false);
    if (!data && error) *error = why;
    return data;
}

void FrameCache::finish(const Key& key, FrameData data, const std::string& /*error*/, bool ahead) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(key);
        if (it != entries_.end()) {
            if (!data) {
                entries_.erase(it);  // a reader waiting for it tries itself (and gets the error)
            } else {
                Entry& e = it->second;
                e.data = data;
                e.ahead = ahead;
                if (ahead) ++stats_.readAhead;
                lru_.push_front(key);
                e.lru = lru_.begin();
                bytes_ += data->size();
                evict();
            }
        }
    }
    made_.notify_all();
}

void FrameCache::evict() {
    while (bytes_ > maxBytes_ && !lru_.empty()) {
        auto it = entries_.find(lru_.back());
        bytes_ -= it->second.data->size();  // readers holding it keep their copy
        entries_.erase(it);
        lru_.pop_back();
    }
}

void FrameCache::queueAhead(const ClipEntry& clip, uint32_t frame) {
    if (!depth_) return;
    ClipState& s = clips_[&clip];
    const int64_t f = frame;
    if (s.last >= 0 && (f < s.last - int64_t(depth_) || f > s.last + int64_t(depth_))) {
        // A jump: what was queued for the old place won't be read soon.
        auto end = std::remove_if(queue_.begin(), queue_.end(), [&](const Key& k) { return k.clip == &clip; });
        stats_.dropped += uint64_t(queue_.end() - end);
        queue_.erase(end, queue_.end());
    }
    // Only reads going forward start read-ahead: after a jump (or a first read) it waits for the
    // next frame in order, so scattered reads don't make frames no one reads.
    const bool forward = s.last >= 0 && f > s.last && f <= s.last + int64_t(depth_) + 1;
    s.last = f;
    if (!forward) return;
    const uint32_t count = clip.source->frameCount();
    bool queued = false;
    for (unsigned i = 1; i <= depth_; ++i) {
        const uint64_t next = uint64_t(frame) + i;
        if (next >= count) break;
        const Key k{&clip, uint32_t(next)};
        if (entries_.count(k)) continue;  // made or being made
        if (std::find_if(queue_.begin(), queue_.end(), [&](const Key& q) { return q.clip == k.clip && q.frame == k.frame; }) !=
            queue_.end())
            continue;
        queue_.push_back(k);
        queued = true;
    }
    if (queued) work_.notify_all();
}

void FrameCache::worker() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        work_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (stopping_) return;
        const Key key = queue_.front();
        queue_.pop_front();
        if (entries_.count(key)) continue;  // a reader got there first
        entries_.emplace(key, Entry{});
        lock.unlock();
        std::string why;
        FrameData data = make(*key.clip, key.frame, &why);
        finish(key, data, why, true);
        lock.lock();
    }
}

FrameCache::Stats FrameCache::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats s = stats_;
    s.bytes = bytes_;
    return s;
}

}  // namespace vfs
