// SPDX-License-Identifier: GPL-2.0-or-later
// Session 14: the image-free worker (VideoCore::SotcAsyncFree in
// third_party/shadps4/src/video_core/texture_cache/sotc_async_free.h). No Vulkan device.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "video_core/texture_cache/sotc_async_free.h"

using VideoCore::SotcAsyncFree::Worker;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

// Jobs run off the caller's thread, in push order, and Drain waits for all of them.
static void TestOrderAndThread() {
    Worker worker;
    const auto caller = std::this_thread::get_id();
    std::vector<int> order;
    std::atomic<int> off_thread{0};
    for (int i = 0; i < 100; ++i) {
        worker.Push([&, i] {
            order.push_back(i); // only the worker thread touches it until Drain returns
            off_thread += std::this_thread::get_id() != caller;
        });
    }
    worker.Drain();
    CHECK(order.size() == 100);
    bool in_order = true;
    for (int i = 0; i < static_cast<int>(order.size()); ++i) {
        in_order &= order[i] == i;
    }
    CHECK(in_order);
    CHECK(off_thread == 100);
    CHECK(worker.Completed() == 100);
    CHECK(worker.Pending() == 0);
}

// The point of the worker: a slow job (vkFreeMemory) doesn't hold up the pushing thread, and
// Drain (the emergency GC before its retry) still waits for it.
static void TestPushDoesNotBlock() {
    Worker worker;
    std::atomic<bool> release{false};
    std::atomic<bool> ran{false};
    worker.Push([&] {
        while (!release) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ran = true;
    });
    const auto t0 = std::chrono::steady_clock::now();
    worker.Push([] {});
    const auto pushed = std::chrono::steady_clock::now() - t0;
    CHECK(pushed < std::chrono::milliseconds(50));
    CHECK(!ran);
    CHECK(worker.Pending() == 2);
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        release = true;
    });
    worker.Drain();
    CHECK(ran);
    CHECK(worker.Pending() == 0);
    releaser.join();
}

// Drain with nothing queued returns at once, also before the thread exists.
static void TestDrainEmpty() {
    Worker worker;
    worker.Drain();
    CHECK(worker.Completed() == 0);
}

// Stop (Instance before vmaDestroyAllocator) runs what is queued; later pushes run inline.
static void TestStop() {
    Worker worker;
    std::atomic<int> runs{0};
    for (int i = 0; i < 10; ++i) {
        worker.Push([&] { ++runs; });
    }
    worker.Stop();
    CHECK(runs == 10);
    const auto caller = std::this_thread::get_id();
    bool inline_run = false;
    worker.Push([&] { inline_run = std::this_thread::get_id() == caller; });
    CHECK(inline_run);
    worker.Stop(); // twice is fine
    worker.Drain();
}

// Jobs own move-only captures (UniqueFunction), released after they run.
static void TestMoveOnlyCapture() {
    Worker worker;
    auto owned = std::make_unique<int>(7);
    int seen = 0;
    worker.Push([p = std::move(owned), &seen] { seen = *p; });
    worker.Drain();
    CHECK(seen == 7);
}

// Several pushing threads (the GPU thread and the presenter both destroy images).
static void TestManyProducers() {
    Worker worker;
    std::atomic<int> runs{0};
    std::vector<std::thread> producers;
    for (int t = 0; t < 4; ++t) {
        producers.emplace_back([&] {
            for (int i = 0; i < 1000; ++i) {
                worker.Push([&] { ++runs; });
            }
        });
    }
    for (auto& p : producers) {
        p.join();
    }
    worker.Drain();
    CHECK(runs == 4000);
}

int main() {
    TestOrderAndThread();
    TestPushDoesNotBlock();
    TestDrainEmpty();
    TestStop();
    TestMoveOnlyCapture();
    TestManyProducers();
    std::printf("test_async_free: %d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
