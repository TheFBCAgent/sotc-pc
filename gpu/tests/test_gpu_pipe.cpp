// SPDX-License-Identifier: GPL-2.0-or-later
// Session 43: the two-stage GPU pipeline's Vulkan-free parts
// (video_core/renderer_vulkan/sotc_gpu_pipe.h): register deltas replayed into a second copy
// must equal the first, and the packet ring must hand every packet over intact and in order,
// across wraps, with a producer and a consumer thread.
#include <atomic>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

#include "video_core/renderer_vulkan/sotc_gpu_pipe.h"

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

using namespace Vulkan;

static void TestDirtyBlocks() {
    SotcDirtyBlocks<100> d; // 4 blocks, the last one partial
    std::vector<std::uint32_t> got;
    d.Take([&](std::uint32_t b) { got.push_back(b); });
    CHECK((got == std::vector<std::uint32_t>{0, 1, 2, 3})); // starts all dirty
    CHECK(!d.Any());
    d.Mark(31, 2); // straddles blocks 0 and 1
    d.Mark(99, 50); // clipped to the end
    d.Mark(100, 1); // out of range: ignored
    got.clear();
    d.Take([&](std::uint32_t b) { got.push_back(b); });
    CHECK((got == std::vector<std::uint32_t>{0, 1, 3}));
    d.Mark(64, 0);
    CHECK(!d.Any());
}

static void TestDeltaReplay() {
    constexpr std::size_t N = 0xD000; // AmdGpu::Regs::NumRegs
    std::vector<std::uint32_t> a(N), b(N, 0xdeadbeef);
    SotcDirtyBlocks<N> dirty;
    SotcPacketWriter w;
    std::vector<std::uint32_t> scratch;
    std::mt19937 rng{43};
    for (int packet = 0; packet < 2000; ++packet) {
        const int writes = rng() % 8;
        for (int i = 0; i < writes; ++i) {
            const std::uint32_t at = rng() % N;
            const std::uint32_t n = 1 + rng() % 40;
            for (std::uint32_t k = at; k < std::min<std::size_t>(N, at + n); ++k) {
                a[k] = rng();
            }
            dirty.Mark(at, n);
        }
        if (packet == 1000) {
            std::fill(a.begin(), a.end(), 7u); // ClearState
            dirty.MarkAll();
        }
        w.Clear();
        SotcEncodeRegDelta(dirty, a.data(), w, scratch);
        SotcPacketReader r{w.Bytes()};
        SotcApplyRegDelta<N>(r, b.data());
        CHECK(r.AtEnd());
        if (a != b) {
            std::printf("FAIL: delta replay differs after packet %d\n", packet);
            ++g_failures;
            return;
        }
    }
}

static std::vector<std::uint8_t> MakePacket(std::uint32_t seq, std::mt19937& rng) {
    const std::size_t size = 8 + rng() % (rng() % 16 == 0 ? 3000 : 200);
    std::vector<std::uint8_t> p(size);
    std::memcpy(p.data(), &seq, 4);
    for (std::size_t i = 4; i < size; ++i) {
        p[i] = static_cast<std::uint8_t>(seq * 31 + i);
    }
    return p;
}

static bool CheckPacket(std::span<const std::uint8_t> p, std::uint32_t seq) {
    std::uint32_t got;
    if (p.size() < 8) {
        return false;
    }
    std::memcpy(&got, p.data(), 4);
    if (got != seq) {
        return false;
    }
    for (std::size_t i = 4; i < p.size(); ++i) {
        if (p[i] != static_cast<std::uint8_t>(seq * 31 + i)) {
            return false;
        }
    }
    return true;
}

static void TestRingSync() {
    // Push then pop at once (SOTC_GPU_PIPE=sync): wraps many times. One thread, so the ring
    // must hold PublishEvery unpublished packets plus the next one.
    SotcPacketRing ring{SotcPacketRing::PublishEvery * 4096 * 2};
    std::mt19937 rng{1};
    CHECK(ring.Empty());
    CHECK(ring.Front().empty());
    for (std::uint32_t seq = 0; seq < 20000; ++seq) {
        const auto p = MakePacket(seq, rng);
        ring.Push(p);
        const auto f = ring.Front();
        CHECK(f.size() == p.size());
        CHECK(CheckPacket(f, seq));
        ring.Pop();
        CHECK(ring.Empty());
    }
    // Several queued, then drained.
    for (std::uint32_t seq = 0; seq < 10; ++seq) {
        ring.Push(MakePacket(seq, rng));
    }
    for (std::uint32_t seq = 0; seq < 10; ++seq) {
        CHECK(CheckPacket(ring.Front(), seq));
        ring.Pop();
    }
    CHECK(ring.Empty());
}

static void TestRingThreads(std::size_t depth) {
    SotcPacketRing ring{16384, depth};
    constexpr std::uint32_t Count = 200000;
    std::thread consumer([&] {
        for (std::uint32_t seq = 0; seq < Count; ++seq) {
            const auto f = ring.WaitFront();
            if (!CheckPacket(f, seq)) {
                std::printf("FAIL: packet %u corrupt or out of order (depth %zu)\n", seq, depth);
                ++g_failures;
                return;
            }
            ring.Pop();
        }
    });
    std::mt19937 rng{2};
    for (std::uint32_t seq = 0; seq < Count; ++seq) {
        ring.Push(MakePacket(seq, rng));
    }
    consumer.join();
    CHECK(ring.Empty());
}

// Session 46: a consumer asleep in WaitFront wakes for packets and for Kick'ed side work (stage
// B's command queue), in any interleaving; every packet and every command is seen once.
static void TestRingKick() {
    SotcPacketRing ring{16384, 64};
    constexpr std::uint32_t Count = 50000;
    constexpr std::uint32_t Commands = 50000;
    std::atomic<std::uint32_t> queued{0};
    std::uint32_t done = 0;
    std::thread consumer([&] {
        std::uint32_t seq = 0;
        while (seq < Count || done < Commands) {
            while (done < queued.load(std::memory_order_acquire)) {
                ++done;
            }
            const auto f = ring.WaitFront(
                [&] { return queued.load(std::memory_order_seq_cst) != done; });
            if (f.empty()) {
                continue; // woken for a command
            }
            if (!CheckPacket(f, seq)) {
                std::printf("FAIL: packet %u corrupt or out of order (kick)\n", seq);
                ++g_failures;
                return;
            }
            ring.Pop();
            ++seq;
        }
    });
    std::thread kicker([&] {
        for (std::uint32_t i = 0; i < Commands; ++i) {
            queued.fetch_add(1, std::memory_order_seq_cst);
            ring.Kick();
            if (i % 64 == 0) {
                std::this_thread::yield(); // let the consumer fall asleep between kicks
            }
        }
    });
    std::mt19937 rng{3};
    for (std::uint32_t seq = 0; seq < Count; ++seq) {
        ring.Push(MakePacket(seq, rng));
    }
    kicker.join();
    consumer.join();
    CHECK(done == Commands);
    CHECK(ring.Empty());
}

static void TestWriterReader() {
    SotcPacketWriter w;
    w.Put<std::uint32_t>(5);
    const std::uint32_t arr[3] = {1, 2, 3};
    w.PutSpan(std::span<const std::uint32_t>{arr});
    w.Put<std::uint64_t>(0x123456789ULL);
    w.PutSpan(std::span<const std::uint32_t>{});
    CHECK(w.Bytes().size() % 8 == 0);
    SotcPacketReader r{w.Bytes()};
    CHECK(r.Get<std::uint32_t>() == 5);
    const auto s = r.GetSpan<std::uint32_t>();
    CHECK(s.size() == 3 && s[0] == 1 && s[2] == 3);
    CHECK(r.Get<std::uint64_t>() == 0x123456789ULL);
    CHECK(r.GetSpan<std::uint32_t>().empty());
    CHECK(r.AtEnd());
}

// Session 45: pending fence writes decide WaitRegMem from the newest write to a label.
static void TestPendingFences() {
    SotcPendingFences f;
    std::uint32_t v = 0;
    CHECK(f.Find(0x1000, &v) == SotcPendingFences::Lookup::None);
    f.Add(1, 0x1000, 4, 7, true);
    f.Add(2, 0x2000, 8, 0x1111111122222222ULL, true);
    f.Add(3, 0x1000, 4, 9, true);
    CHECK(f.Find(0x1000, &v) == SotcPendingFences::Lookup::Value && v == 9); // newest wins
    CHECK(f.Find(0x2000, &v) == SotcPendingFences::Lookup::Value && v == 0x22222222);
    CHECK(f.Find(0x2004, &v) == SotcPendingFences::Lookup::Value && v == 0x11111111);
    CHECK(f.Find(0x2006, &v) == SotcPendingFences::Lookup::Unknown); // partly covered
    CHECK(f.Find(0x0ffe, &v) == SotcPendingFences::Lookup::Unknown);
    CHECK(f.Find(0x1004, &v) == SotcPendingFences::Lookup::None);
    f.Add(4, 0x3000, 8, 0, false); // a clock
    CHECK(f.Find(0x3004, &v) == SotcPendingFences::Lookup::Unknown);
    f.Retire(2);
    CHECK(f.Find(0x2000, &v) == SotcPendingFences::Lookup::None);
    CHECK(f.Find(0x1000, &v) == SotcPendingFences::Lookup::Value && v == 9);
    f.Retire(4);
    CHECK(f.Empty());
}

static void TestPendingRanges() {
    SotcPendingRanges p;
    const std::uint8_t* at = nullptr;
    CHECK(p.Find(0x1000, 16, 0, &at) == SotcPendingRanges::Read::None);
    p.AddWrite(1, 0x10000, 0x100);
    p.AddOverlay(2, 0x20000, {1, 2, 3, 4});
    p.AddWrite(3, 0x20008, 4); // a GPU write over the overlay's third dword
    CHECK(p.Size() == 3);
    CHECK(p.Find(0x100f0, 0x20, 0, &at) == SotcPendingRanges::Read::Wait);
    CHECK(p.Find(0x10100, 4, 0, &at) == SotcPendingRanges::Read::None);
    CHECK(p.Find(0x20000, 8, 0, &at) == SotcPendingRanges::Read::Overlay);
    CHECK(reinterpret_cast<const std::uint32_t*>(at)[1] == 2);
    CHECK(p.Find(0x20004, 8, 0, &at) == SotcPendingRanges::Read::Wait); // newest is the write
    CHECK(p.Find(0x20004, 8, 3, &at) == SotcPendingRanges::Read::None); // retired view
    CHECK(p.Find(0x1fffc, 8, 0, &at) == SotcPendingRanges::Read::Wait); // partial overlay
    // Merge: the overlay's bytes over guest memory, refused under a GPU write.
    std::uint32_t buf[4] = {9, 9, 9, 9};
    CHECK(p.Merge(0x1fff8, reinterpret_cast<std::uint8_t*>(buf), 16, 0));
    CHECK(buf[0] == 9 && buf[1] == 9 && buf[2] == 1 && buf[3] == 2);
    CHECK(!p.Merge(0x20000, reinterpret_cast<std::uint8_t*>(buf), 16, 0));
    // A newer overlay wins in Merge and Find.
    p.AddOverlay(4, 0x20000, {7});
    std::uint32_t one = 0;
    CHECK(p.Merge(0x20000, reinterpret_cast<std::uint8_t*>(&one), 4, 3));
    CHECK(one == 7);
    // Refreshing a GPU write keeps one entry; Retire frees by packet.
    p.AddWrite(5, 0x10000, 0x100);
    CHECK(p.Size() == 4);
    p.Retire(4);
    CHECK(p.Size() == 1);
    CHECK(p.Find(0x10000, 4, 0, &at) == SotcPendingRanges::Read::Wait);
    CHECK(p.Find(0x20000, 4, 0, &at) == SotcPendingRanges::Read::None);
    p.AddWrite(6, 0x40000000, std::uint64_t{64} << 20); // big: no block counters
    CHECK(p.Big() == 1);
    CHECK(p.Find(0x40000000 + (std::uint64_t{32} << 20), 4, 0, &at) ==
          SotcPendingRanges::Read::Wait);
    p.Retire(6);
    CHECK(p.Empty() && p.Big() == 0);
    CHECK(p.Find(0x10000, 4, 0, &at) == SotcPendingRanges::Read::None);
}

int main() {
    TestPendingRanges();
    TestPendingFences();
    TestDirtyBlocks();
    TestDeltaReplay();
    TestWriterReader();
    TestRingSync();
    TestRingThreads(~std::size_t{0});
    TestRingThreads(4);
    TestRingThreads(1);
    TestRingKick();
    if (g_failures == 0) {
        std::printf("test_gpu_pipe: OK\n");
        return 0;
    }
    std::printf("test_gpu_pipe: %d failure(s)\n", g_failures);
    return 1;
}
