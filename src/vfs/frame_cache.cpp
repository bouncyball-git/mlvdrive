#include "vfs/frame_cache.h"

#include <algorithm>
#include <new>

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

// Out of memory is a failure like any other: thrown on a read-ahead worker it would end the
// program, and on a reader's thread it would leave the entry "being made" for good.
FrameData FrameCache::make(const ClipEntry& clip, uint32_t frame, std::string* error) {
    try {
        auto data = std::make_shared<std::vector<uint8_t>>();
        if (!clip.source->renderFrame(frame, *data, error)) return nullptr;
        return data;
    } catch (const std::bad_alloc&) {
        *error = "out of memory";
        return nullptr;
    }
}

FrameData FrameCache::get(const std::shared_ptr<const ClipEntry>& clipPtr, uint32_t frame, bool readAhead,
                          std::string* error) {
    const ClipEntry& clip = *clipPtr;
    const Key key{clip.id, frame};
    std::unique_lock<std::mutex> lock(mutex_);
    const bool forward = noteRead(clip, frame);
    // Only reads going forward start read-ahead: after a jump (or a first read) it waits for the
    // next frame in order, so scattered reads don't make frames no one reads.
    if (readAhead && forward) queueAhead(clipPtr, frame);
    bool waited = false;
    for (;;) {
        auto it = entries_.find(key);
        if (it == entries_.end()) break;  // not there (or making it failed): made below
        Entry& e = it->second;
        if (e.data) {
            lru_.splice(lru_.begin(), lru_, e.lru);
            e.run = clips_[clip.id].run;  // read again in this run: it belongs to it now
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
            if (!data || it->second.forgotten) {
                // Failed: a reader waiting for it tries itself (and gets the error). Forgotten: its
                // maker gets it, the cache doesn't keep it (a reader waiting makes it again).
                entries_.erase(it);
            } else {
                Entry& e = it->second;
                e.data = data;
                e.ahead = ahead;
                e.run = clips_[key.clip].run;
                e.seq = ++seq_;
                if (ahead) ++stats_.readAhead;
                lru_.push_front(key);
                e.lru = lru_.begin();
                bytes_ += data->size();
                evict(key);
            }
        }
    }
    made_.notify_all();
}

void FrameCache::evict(const Key& added) {
    while (bytes_ > maxBytes_ && !lru_.empty()) {
        auto victim = entries_.find(lru_.back());
        // The least recently used frame is of the run `added` belongs to: a straight read through
        // a clip longer than the cache. Then the newest frame read in that run goes instead, so
        // the run's start stays for the next pass.
        const uint64_t run = clips_[added.clip].run;
        if (victim->first.clip == added.clip && victim->second.run == run && !victim->second.ahead) {
            auto newest = entries_.end();
            for (auto it = entries_.lower_bound(Key{added.clip, 0});
                 it != entries_.end() && it->first.clip == added.clip; ++it) {
                const Entry& e = it->second;
                if (!e.data || e.ahead || e.run != run || it->first.frame == added.frame) continue;
                if (newest == entries_.end() || e.seq > newest->second.seq) newest = it;
            }
            if (newest != entries_.end()) victim = newest;
        }
        bytes_ -= victim->second.data->size();  // readers holding it keep their copy
        lru_.erase(victim->second.lru);
        entries_.erase(victim);
    }
}

bool FrameCache::noteRead(const ClipEntry& clip, uint32_t frame) {
    ClipState& s = clips_[clip.id];
    const int64_t f = frame;
    const int64_t window = int64_t(std::max(depth_, 1u)) + 1;
    if (s.last >= 0 && (f < s.last - window || f > s.last + window)) {
        // A jump: what was queued for the old place won't be read soon; a new run starts.
        ++s.run;
        auto end = std::remove_if(queue_.begin(), queue_.end(), [&](const Job& j) { return j.clip->id == clip.id; });
        stats_.dropped += uint64_t(queue_.end() - end);
        queue_.erase(end, queue_.end());
    }
    const bool forward = s.last >= 0 && f > s.last && f <= s.last + window;
    s.last = f;
    return forward;
}

void FrameCache::queueAhead(const std::shared_ptr<const ClipEntry>& clip, uint32_t frame) {
    if (!depth_) return;
    const uint32_t count = clip->source->frameCount();
    bool queued = false;
    for (unsigned i = 1; i <= depth_; ++i) {
        const uint64_t next = uint64_t(frame) + i;
        if (next >= count) break;
        const Key k{clip->id, uint32_t(next)};
        if (entries_.count(k)) continue;  // made or being made
        if (std::find_if(queue_.begin(), queue_.end(),
                         [&](const Job& q) { return q.clip->id == k.clip && q.frame == k.frame; }) != queue_.end())
            continue;
        queue_.push_back(Job{clip, k.frame});
        queued = true;
    }
    if (queued) work_.notify_all();
}

void FrameCache::worker() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        work_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (stopping_) return;
        const Job job = queue_.front();
        queue_.pop_front();
        const Key key{job.clip->id, job.frame};
        if (entries_.count(key)) continue;  // a reader got there first
        entries_.emplace(key, Entry{});
        lock.unlock();
        std::string why;
        FrameData data = make(*job.clip, job.frame, &why);
        finish(key, data, why, true);
        lock.lock();
    }
}

void FrameCache::forget(uint64_t clipId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto end = std::remove_if(queue_.begin(), queue_.end(), [&](const Job& j) { return j.clip->id == clipId; });
    queue_.erase(end, queue_.end());
    for (auto it = entries_.lower_bound(Key{clipId, 0}); it != entries_.end() && it->first.clip == clipId;) {
        if (!it->second.data) {  // being made: its maker finishes it, then lets it go
            it->second.forgotten = true;
            ++it;
            continue;
        }
        bytes_ -= it->second.data->size();
        lru_.erase(it->second.lru);
        it = entries_.erase(it);
    }
    clips_.erase(clipId);
}

FrameCache::Stats FrameCache::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats s = stats_;
    s.bytes = bytes_;
    return s;
}

}  // namespace vfs
