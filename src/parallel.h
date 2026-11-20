// A small thread pool for the per-pixel passes. Any number of threads may call forRange() at once
// (the filesystem generates several frames in parallel); the calling thread works on its own job
// too.
#pragma once

#include <functional>

namespace par {

// Calls fn(begin, end) on sub-ranges covering [0, n), in parallel, and returns when all are done.
// `minChunk` is the smallest sub-range worth handing to another thread.
void forRange(int n, const std::function<void(int, int)>& fn, int minChunk = 8);

// Stops the pool's threads (at unmount or exit: not from DllMain or static destructors,
// where joining threads can deadlock). The pool starts again on the next forRange().
void shutdown();

}  // namespace par
