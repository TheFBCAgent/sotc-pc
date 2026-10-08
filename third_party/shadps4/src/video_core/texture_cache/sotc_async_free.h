// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc patch (docs/NEXT.md, session 14): free image memory off the GPU thread.
//
// Session 14's samples of the GPU command processor thread had ~2% of it in vkFreeMemory, reached
// from ~Image (vmaDestroyImage of a dedicated allocation), run from the scheduler's deferred
// operations: a few ms per free with VRAM over budget. With SOTC_ASYNC_IMAGE_FREE=1, UniqueImage
// hands the vmaDestroyImage to this worker instead. It is opt-in: no measurable fps gain in
// session 14, and one run with it on hit a blue screen of unknown cause. The GPU is already done
// with the image when its deferred operation runs, and VMA is thread-safe (no
// EXTERNALLY_SYNCHRONIZED).
//
// Memory now returns a little later, so code that frees images and then needs the memory back at
// once (TextureCache::SotcEmergencyCollect before its retry, Instance before vmaDestroyAllocator)
// calls Drain(). Header-only and Vulkan-free: test gpu/tests/test_async_free.cpp.

#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#include "common/unique_function.h"

namespace VideoCore::SotcAsyncFree {

class Worker {
public:
    using Job = Common::UniqueFunction<void>;

    Worker() = default;
    ~Worker() {
        Stop();
    }

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    /// Queues a job; the thread starts on the first one. After Stop() jobs run inline.
    void Push(Job&& job) {
        {
            std::unique_lock lk{mutex};
            if (!stopped) {
                if (!thread.joinable()) {
                    thread = std::thread([this] { Run(); });
                }
                queue.push_back(std::move(job));
                ++pushed;
                cv.notify_one();
                return;
            }
        }
        job();
    }

    /// Returns once every job pushed before the call has run.
    void Drain() {
        std::unique_lock lk{mutex};
        const uint64_t target = pushed;
        done_cv.wait(lk, [&] { return completed >= target; });
    }

    /// Runs what is queued, then joins the thread. Later pushes run inline.
    void Stop() {
        {
            std::unique_lock lk{mutex};
            if (stopped) {
                return;
            }
            stopped = true;
            cv.notify_one();
        }
        if (thread.joinable()) {
            thread.join();
        }
    }

    uint64_t Completed() {
        std::unique_lock lk{mutex};
        return completed;
    }

    /// Jobs queued and not yet finished.
    uint64_t Pending() {
        std::unique_lock lk{mutex};
        return pushed - completed;
    }

private:
    void Run() {
        std::unique_lock lk{mutex};
        for (;;) {
            cv.wait(lk, [&] { return stopped || !queue.empty(); });
            if (queue.empty()) {
                return; // stopped and drained
            }
            Job job = std::move(queue.front());
            queue.pop_front();
            lk.unlock();
            job();
            job = {};
            lk.lock();
            ++completed;
            done_cv.notify_all();
        }
    }

    std::mutex mutex;
    std::condition_variable cv;
    std::condition_variable done_cv;
    std::deque<Job> queue;
    std::thread thread;
    uint64_t pushed = 0;
    uint64_t completed = 0;
    bool stopped = false;
};

} // namespace VideoCore::SotcAsyncFree
