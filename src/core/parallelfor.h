#pragma once

#include <QSemaphore>
#include <QThreadPool>

#include <algorithm>
#include <atomic>
#include <cstddef>

// Calls function(first, last) over consecutive chunks of [0, count), `grain` items each, on the
// global thread pool and on the calling thread, and returns when every chunk is done. Chunks are
// handed out in order as threads free up. A worker is started only where the pool has a thread
// free, and the calling thread takes chunks too, so a call made from inside the pool, or while
// the pool is busy, still finishes, if with fewer threads.
template <typename Function>
void parallelFor(size_t count, Function &&function, size_t grain = 64u * 1024u)
{
    grain = std::max<size_t>(grain, 1);
    const size_t chunkCount = count / grain + (count % grain != 0);
    QThreadPool *pool = QThreadPool::globalInstance();
    const int workerCount = int(std::min(
        chunkCount, size_t(std::max(1, pool->maxThreadCount()))));
    if (workerCount <= 1) {
        function(size_t(0), count);
        return;
    }

    std::atomic_size_t next { 0 };
    QSemaphore finished;
    auto work = [&] {
        for (;;) {
            const size_t first = next.fetch_add(grain, std::memory_order_relaxed);
            if (first >= count)
                return;
            function(first, first + std::min(grain, count - first));
        }
    };

    int started = 0;
    for (int i = 1; i < workerCount; ++i) {
        if (!pool->tryStart([&] { work(); finished.release(); }))
            break;
        ++started;
    }
    work();
    finished.acquire(started);
}
