// Finished DNG files, shared by every reader, limited in bytes, and read-ahead on worker threads
// so that a player reading frames in order finds them made.
//
// What goes when the cache is full: the least recently used frame, except within a forward run
// through a clip (frames read in order since the last jump), where the newest frame already read
// goes instead. A clip longer than the cache, read straight through again (a loop, a second
// pass), then finds its start still made instead of nothing (plain LRU drops each frame just
// before it is needed). Frames made ahead and not read yet are kept.
//
// Frames are keyed by the clip's id (never reused, also when the drive's tree is rebuilt), so a
// clip that changed or went away can't be mistaken for another; forget() lets its frames go.
//
// A frame asked for while it is being made (by read-ahead or another reader) is waited for, not
// made twice. Read-ahead: reading frame N just after N-1 (reads going forward) queues N+1 ...
// N+depth of its clip; a read far from the last one (a jump) drops what was queued for that clip
// and not started, and starts nothing until the next read in order.
#pragma once

#include "vfs/tree.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vfs {

using FrameData = std::shared_ptr<const std::vector<uint8_t>>;

class FrameCache {
public:
    // `maxBytes`: what the cache keeps (frames in use are kept as well, outside the count).
    // `threads`: read-ahead workers (0: none). `depth`: frames read ahead of the last one read.
    FrameCache(uint64_t maxBytes, unsigned threads, unsigned depth);
    ~FrameCache();
    FrameCache(const FrameCache&) = delete;
    FrameCache& operator=(const FrameCache&) = delete;

    // The whole DNG of a frame: from the cache, or made now. Null (with `error`) if it can't be.
    // `readAhead` queues the frames after it.
    FrameData get(const std::shared_ptr<const ClipEntry>& clip, uint32_t frame, bool readAhead, std::string* error);

    // Drops a clip's frames and what is queued for it (it changed, or left the drive).
    void forget(uint64_t clipId);

    struct Stats {
        uint64_t hits = 0;          // found made
        uint64_t waits = 0;         // found being made (waited for)
        uint64_t misses = 0;        // made for the reader
        uint64_t readAhead = 0;     // made ahead
        uint64_t readAheadUsed = 0; // of those, read
        uint64_t dropped = 0;       // queued ahead, then dropped by a jump
        uint64_t bytes = 0;         // in the cache now
    };
    Stats stats() const;

private:
    struct Key {
        uint64_t clip;  // ClipEntry::id
        uint32_t frame;
        bool operator<(const Key& o) const { return clip != o.clip ? clip < o.clip : frame < o.frame; }
    };
    struct Job {
        std::shared_ptr<const ClipEntry> clip;  // kept while queued
        uint32_t frame;
    };
    struct Entry {
        FrameData data;              // null while being made
        bool ahead = false;          // made by read-ahead and not read yet
        uint64_t run = 0;            // the clip's run it was made or last read in
        uint64_t seq = 0;            // when it was made (larger: later)
        std::list<Key>::iterator lru;  // valid once made
    };
    struct ClipState {
        int64_t last = -1;           // the last frame read
        uint64_t run = 1;            // the current forward run (a jump starts the next)
    };

    FrameData make(const ClipEntry& clip, uint32_t frame, std::string* error);
    void finish(const Key& key, FrameData data, const std::string& error, bool ahead);
    bool noteRead(const ClipEntry& clip, uint32_t frame);    // under mutex_: true if going forward
    void queueAhead(const std::shared_ptr<const ClipEntry>& clip, uint32_t frame);  // under mutex_
    void evict(const Key& added);                            // under mutex_
    void worker();

    const uint64_t maxBytes_;
    const unsigned depth_;
    mutable std::mutex mutex_;
    std::condition_variable made_;    // an entry was finished
    std::condition_variable work_;    // a job was queued, or stopping
    std::map<Key, Entry> entries_;
    std::list<Key> lru_;              // made entries, most recent first
    uint64_t bytes_ = 0;
    uint64_t seq_ = 0;
    std::deque<Job> queue_;           // read-ahead jobs
    std::map<uint64_t, ClipState> clips_;  // by ClipEntry::id
    Stats stats_;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
};

}  // namespace vfs
