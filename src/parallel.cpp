#include "parallel.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace par {

namespace {

struct Job {
    const std::function<void(int, int)>* fn;
    int n = 0;
    int chunks = 0;
    int next = 0;  // the next chunk to hand out (under the pool's mutex)
    int done = 0;  // chunks finished (under the pool's mutex)
};

class Pool {
public:
    void run(int n, const std::function<void(int, int)>& fn, int minChunk) {
        const int threads = start() + 1;  // the caller works too
        int chunks = std::min((n + minChunk - 1) / minChunk, threads * 4);
        if (chunks <= 1) {
            fn(0, n);
            return;
        }
        Job job;
        job.fn = &fn;
        job.n = n;
        job.chunks = chunks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(&job);
        }
        work_.notify_all();

        std::unique_lock<std::mutex> lock(mutex_);
        while (job.next < job.chunks) {
            const int c = claim(&job);
            lock.unlock();
            execute(job, c);
            lock.lock();
            if (++job.done == job.chunks) finished_.notify_all();
        }
        finished_.wait(lock, [&] { return job.done == job.chunks; });
    }

    void stop() {
        std::vector<std::thread> threads;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
            threads.swap(threads_);
        }
        work_.notify_all();
        for (auto& t : threads) t.join();
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = false;
    }

private:
    // Starts the threads if they aren't running; returns how many there are.
    int start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (threads_.empty()) {
            const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
            for (unsigned i = 0; i + 1 < hw; ++i) threads_.emplace_back([this] { loop(); });
        }
        return int(threads_.size());
    }

    // Takes the next chunk of `job` (mutex held); a job leaves the queue with its last chunk.
    int claim(Job* job) {
        const int c = job->next++;
        if (job->next == job->chunks) queue_.erase(std::find(queue_.begin(), queue_.end(), job));
        return c;
    }

    static void execute(const Job& job, int c) {
        const int begin = int(int64_t(job.n) * c / job.chunks);
        const int end = int(int64_t(job.n) * (c + 1) / job.chunks);
        if (begin < end) (*job.fn)(begin, end);
    }

    void loop() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            work_.wait(lock, [&] { return quit_ || !queue_.empty(); });
            if (quit_) return;
            Job* job = queue_.front();
            const int c = claim(job);
            lock.unlock();
            execute(*job, c);
            lock.lock();
            if (++job->done == job->chunks) finished_.notify_all();
        }
    }

    std::mutex mutex_;
    std::condition_variable work_, finished_;
    std::deque<Job*> queue_;
    std::vector<std::thread> threads_;
    bool quit_ = false;
};

Pool& pool() {
    static Pool* p = new Pool;  // never destroyed: stopped by shutdown(), not by static destructors
    return *p;
}

}  // namespace

void forRange(int n, const std::function<void(int, int)>& fn, int minChunk) {
    if (n <= 0) return;
    pool().run(n, fn, std::max(1, minChunk));
}

void shutdown() { pool().stop(); }

}  // namespace par
