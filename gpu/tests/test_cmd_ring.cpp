// SPDX-License-Identifier: GPL-2.0-or-later
// Session 62: the command ring of draw-prep shape D
// (video_core/renderer_vulkan/sotc_cmd_ring.h). Records written in place must come out intact
// and in order across wraps, Reserve must refuse when full (and accept again after Release),
// and only published records are visible; then the same with a producer and a consumer thread.
// Session 64: stage C's waits (Sleep, PublishAndWake, WaitDrained, WaitForSpace): after a
// drain the consumer has finished every record, and no wake-up is lost.
#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "video_core/renderer_vulkan/sotc_cmd_ring.h"

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

using namespace Vulkan;

static void Fill(void* p, std::size_t n, std::uint32_t seq) {
    auto* b = static_cast<std::uint8_t*>(p);
    for (std::size_t i = 0; i < n; ++i) {
        b[i] = static_cast<std::uint8_t>(seq * 31 + i);
    }
}

static bool Same(const void* p, std::size_t n, std::uint32_t seq) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    for (std::size_t i = 0; i < n; ++i) {
        if (b[i] != static_cast<std::uint8_t>(seq * 31 + i)) {
            return false;
        }
    }
    return true;
}

static void TestBasics() {
    SotcCmdRing ring{256};
    CHECK(ring.Capacity() == 256);
    CHECK(ring.Empty());
    void* p = ring.Reserve(7, 12);
    CHECK(p != nullptr);
    Fill(p, 12, 1);
    CHECK(ring.Empty()); // not published yet
    CHECK(ring.Front().kind == SotcCmdRing::WrapKind);
    ring.Publish();
    const auto r = ring.Front();
    CHECK(r.kind == 7);
    CHECK(r.payload_bytes == 16);
    CHECK(Same(r.payload, 12, 1));
    ring.Pop();
    ring.Release();
    CHECK(ring.Empty());
    // A zero-size record.
    CHECK(ring.Reserve(3, 0) != nullptr);
    ring.Publish();
    CHECK(ring.Front().kind == 3);
    CHECK(ring.Front().payload_bytes == 0);
    ring.Pop();
    ring.Release();
    // Too big for the ring at all.
    CHECK(ring.Reserve(1, ring.MaxPayload() + 8) == nullptr);
    CHECK(ring.Reserve(1, ring.MaxPayload()) != nullptr);
}

static void TestFullAndWrap() {
    SotcCmdRing ring{256};
    std::uint32_t written = 0, read = 0;
    std::mt19937 rng{62};
    for (int round = 0; round < 2000; ++round) {
        // Fill until refused.
        for (;;) {
            const std::size_t n = rng() % 60;
            void* p = ring.Reserve(written % 1000, n);
            if (!p) {
                break;
            }
            // Write the size in the first bytes so the reader can check the rest.
            std::uint8_t* b = static_cast<std::uint8_t*>(p);
            if (n >= 1) {
                b[0] = static_cast<std::uint8_t>(n);
                Fill(b + 1, n - 1, written);
            }
            ++written;
            if (rng() % 3 == 0) {
                break;
            }
        }
        ring.Publish();
        // Drain some.
        const int take = rng() % 8;
        for (int i = 0; i < take; ++i) {
            const auto r = ring.Front();
            if (r.kind == SotcCmdRing::WrapKind) {
                break;
            }
            CHECK(r.kind == read % 1000);
            const auto* b = static_cast<const std::uint8_t*>(r.payload);
            if (r.payload_bytes > 0) {
                const std::size_t n = b[0];
                CHECK(SotcCmdRing::Align(n) == r.payload_bytes);
                CHECK(Same(b + 1, n - 1, read));
            }
            ring.Pop();
            ++read;
        }
        ring.Release();
    }
    CHECK(written > 1000);
    CHECK(read > 1000);
}

static void TestThreads() {
    SotcCmdRing ring{4096};
    constexpr std::uint32_t N = 200000;
    std::atomic<bool> bad{false};
    std::thread consumer{[&] {
        std::uint32_t seq = 0;
        while (seq < N) {
            const auto r = ring.Front();
            if (r.kind == SotcCmdRing::WrapKind) {
                ring.Release();
                std::this_thread::yield();
                continue;
            }
            std::uint32_t got;
            std::memcpy(&got, r.payload, 4);
            const std::size_t n = r.kind;
            if (got != seq || !Same(static_cast<const std::uint8_t*>(r.payload) + 4, n, seq)) {
                bad = true;
            }
            ring.Pop();
            if ((seq & 15) == 0) {
                ring.Release();
            }
            ++seq;
        }
        ring.Release();
    }};
    std::mt19937 rng{7};
    for (std::uint32_t seq = 0; seq < N; ++seq) {
        const std::uint32_t n = rng() % 200;
        void* p;
        while (!(p = ring.Reserve(n, 4 + n))) {
            ring.Publish();
            std::this_thread::yield();
        }
        std::memcpy(p, &seq, 4);
        Fill(static_cast<std::uint8_t*>(p) + 4, n, seq);
        if (rng() % 4 == 0) {
            ring.Publish();
        }
    }
    ring.Publish();
    consumer.join();
    CHECK(!bad);
    CHECK(ring.Empty());
}

static void TestThreadWaits() {
    using namespace std::chrono_literals;
    SotcCmdRing ring{4096};
    constexpr std::uint32_t N = 300000;
    constexpr std::uint32_t StopKind = 999;
    std::atomic<std::uint64_t> done{0};
    std::atomic<bool> bad{false};
    std::thread consumer{[&] {
        std::uint32_t seq = 0, n = 0;
        for (;;) {
            const auto r = ring.Front();
            if (r.kind == SotcCmdRing::WrapKind) {
                // Short spins so the sleeping path runs often.
                ring.Sleep(std::chrono::nanoseconds{(seq & 1) ? 0 : 2000});
                continue;
            }
            if (r.kind == StopKind) {
                ring.Pop();
                ring.Release();
                return;
            }
            std::uint32_t got;
            std::memcpy(&got, r.payload, 4);
            if (got != seq || !Same(static_cast<const std::uint8_t*>(r.payload) + 4, r.kind, seq)) {
                bad = true;
            }
            ++seq;
            done.store(seq, std::memory_order_relaxed);
            ring.Pop();
            if ((++n & 63) == 0) {
                ring.Release();
            }
        }
    }};
    std::mt19937 rng{64};
    std::uint32_t drains = 0, stale = 0;
    for (std::uint32_t seq = 0; seq < N; ++seq) {
        const std::uint32_t n = rng() % 200;
        void* p;
        while (!(p = ring.Reserve(n, 4 + n))) {
            ring.WaitForSpace(std::chrono::nanoseconds{rng() % 2 ? 0 : 1000});
        }
        std::memcpy(p, &seq, 4);
        Fill(static_cast<std::uint8_t*>(p) + 4, n, seq);
        const std::uint32_t action = rng() % 64;
        if (action < 8) {
            ring.PublishAndWake();
        } else if (action == 8) {
            ring.WaitDrained(std::chrono::nanoseconds{rng() % 2 ? 0 : 5000});
            ++drains;
            if (done.load(std::memory_order_relaxed) != seq + 1 || !ring.Drained()) {
                ++stale;
            }
        } else if (action == 9) {
            // Let the consumer fall asleep (a busy wait: sleep_for rounds up to ~1-15 ms on
            // Windows).
            const auto until = std::chrono::steady_clock::now() + 50us;
            while (std::chrono::steady_clock::now() < until) {
            }
        }
    }
    ring.WaitDrained(0ns);
    CHECK(done.load() == N);
    while (!ring.Reserve(StopKind, 0)) {
        ring.WaitForSpace(0ns);
    }
    ring.PublishAndWake();
    consumer.join();
    CHECK(!bad);
    CHECK(stale == 0);
    CHECK(drains > 1000);
}

int main() {
    TestBasics();
    TestFullAndWrap();
    TestThreads();
    TestThreadWaits();
    if (g_failures == 0) {
        std::printf("test_cmd_ring: OK\n");
    }
    return g_failures == 0 ? 0 : 1;
}
