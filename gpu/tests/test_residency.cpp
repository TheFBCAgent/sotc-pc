// SPDX-License-Identifier: GPL-2.0-or-later
// Workstream R: the sparse-arena residency suballocator (VideoCore::ResidencyAllocator in
// third_party/shadps4/src/video_core/buffer_cache/residency_allocator.*). Device memory is faked
// with callbacks: no Vulkan device, no GPU.

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <vector>

#include "video_core/buffer_cache/residency_allocator.h"

using VideoCore::ResidencyAllocator;
using Slice = ResidencyAllocator::Slice;

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

static constexpr u64 BLOCK = 64 << 10;        // NVIDIA's sparse block
static constexpr u64 CHUNK = 4 * 1024 * 1024; // 64 blocks: small, so tests are short
static constexpr u64 CHUNK_BLOCKS = CHUNK / BLOCK;

/// Fake device memory: handles count up from 0x1000; can be told to fail.
struct FakeDevice {
    std::map<u64, u64> live; // handle -> bytes
    u64 next = 0x1000;
    u64 fail_above = ~0ull;  // allocations larger than this fail
    int fail_all = 0;
    std::vector<u64> requests;

    ResidencyAllocator::AllocateFn Alloc() {
        return [this](u64 bytes) -> u64 {
            requests.push_back(bytes);
            if (fail_all || bytes > fail_above) {
                return 0;
            }
            const u64 h = next++;
            live[h] = bytes;
            return h;
        };
    }
    ResidencyAllocator::FreeFn Free() {
        return [this](u64 h) {
            CHECK(live.erase(h) == 1);
        };
    }
};

static u64 Total(const std::vector<Slice>& s) {
    u64 n = 0;
    for (const auto& x : s) {
        n += x.blocks;
    }
    return n;
}

/// No two outstanding slices overlap, and each fits its chunk.
static bool Disjoint(const std::vector<Slice>& all, const FakeDevice& dev) {
    std::map<u64, std::vector<std::pair<u64, u64>>> by_mem;
    for (const auto& s : all) {
        const auto it = dev.live.find(s.memory);
        if (it == dev.live.end() || (s.offset + s.blocks) * BLOCK > it->second) {
            return false;
        }
        by_mem[s.memory].emplace_back(s.offset, s.offset + s.blocks);
    }
    for (auto& [m, v] : by_mem) {
        std::sort(v.begin(), v.end());
        for (size_t i = 1; i < v.size(); ++i) {
            if (v[i].first < v[i - 1].second) {
                return false;
            }
        }
    }
    return true;
}

static void TestBasic() {
    FakeDevice dev;
    ResidencyAllocator a{BLOCK, CHUNK, dev.Alloc(), dev.Free()};
    CHECK(a.ChunkBlocks() == CHUNK_BLOCKS);
    CHECK(a.GetStats().chunks == 0); // lazy: no chunk until the first request

    std::vector<Slice> out;
    CHECK(a.Allocate(0, out) && out.empty());
    CHECK(a.Allocate(4, out));
    CHECK(out.size() == 1 && out[0].offset == 0 && out[0].blocks == 4);
    CHECK(dev.requests.size() == 1 && dev.requests[0] == CHUNK);
    CHECK(a.Allocate(10, out));
    CHECK(out.size() == 2 && out[1].memory == out[0].memory && out[1].offset == 4);

    // Many small requests: one chunk serves 64 blocks.
    for (int i = 0; i < 50; ++i) {
        CHECK(a.Allocate(1, out));
    }
    auto st = a.GetStats();
    CHECK(st.chunks == 1 && st.allocations == 1);
    CHECK(st.used_bytes == 64 * BLOCK && st.chunk_bytes == CHUNK);
    CHECK(st.free_ranges == 0);
    CHECK(Disjoint(out, dev));
}

static void TestSplitAcrossChunks() {
    FakeDevice dev;
    ResidencyAllocator a{BLOCK, CHUNK, dev.Alloc(), dev.Free()};
    std::vector<Slice> out;
    CHECK(a.Allocate(60, out)); // chunk 0: 4 blocks left
    out.clear();
    CHECK(a.Allocate(10, out)); // 4 from the tail of chunk 0, 6 from a new chunk
    CHECK(out.size() == 2);
    CHECK(Total(out) == 10);
    CHECK(out[0].offset == 60 && out[0].blocks == 4);
    CHECK(out[1].memory != out[0].memory && out[1].offset == 0 && out[1].blocks == 6);
    auto st = a.GetStats();
    CHECK(st.chunks == 2 && st.used_bytes == 70 * BLOCK);
    // No tail waste: the second chunk's remainder is the only free range.
    CHECK(st.free_ranges == 1 && st.largest_free == CHUNK_BLOCKS - 6);
}

static void TestOversize() {
    FakeDevice dev;
    ResidencyAllocator a{BLOCK, CHUNK, dev.Alloc(), dev.Free()};
    std::vector<Slice> out;
    CHECK(a.Allocate(200, out)); // > 64 blocks: one dedicated chunk of exactly 200 blocks
    CHECK(out.size() == 1 && out[0].blocks == 200 && out[0].offset == 0);
    CHECK(dev.requests.size() == 1 && dev.requests[0] == 200 * BLOCK);
    CHECK(a.GetStats().free_ranges == 0);

    // With a partly used regular chunk, the free tail is used first, then a dedicated chunk.
    FakeDevice dev2;
    ResidencyAllocator b{BLOCK, CHUNK, dev2.Alloc(), dev2.Free()};
    out.clear();
    CHECK(b.Allocate(10, out));
    out.clear();
    CHECK(b.Allocate(150, out));
    CHECK(out.size() == 2 && out[0].blocks == 54 && out[1].blocks == 96);
    CHECK(dev2.requests.size() == 2 && dev2.requests[1] == 96 * BLOCK);

    // A dedicated chunk is released when empty, regardless of `keep`.
    const Slice dedicated{dev.live.begin()->first, 0, 200};
    CHECK(a.Free(Slice{dedicated.memory, 0, 0})); // zero-length free is a no-op
    CHECK(a.Free(dedicated));
    CHECK(a.ReleaseEmptyChunks(8) == 1 && dev.live.empty());
}

static void TestFreeAndReuse() {
    FakeDevice dev;
    ResidencyAllocator a{BLOCK, CHUNK, dev.Alloc(), dev.Free()};
    std::vector<Slice> s;
    for (int i = 0; i < 8; ++i) {
        CHECK(a.Allocate(8, s)); // fills chunk 0 exactly
    }
    CHECK(a.GetStats().chunks == 1 && a.GetStats().free_ranges == 0);
    CHECK(a.Free(s[3]));
    CHECK(!a.Free(s[3])); // double free is rejected
    CHECK(!a.Free(Slice{0xdead, 0, 1}));            // unknown memory
    CHECK(!a.Free(Slice{s[0].memory, 60, 8}));      // past the chunk end
    CHECK(!a.Free(Slice{s[0].memory, 20, 8}));      // overlaps the free range [24, 32)
    std::vector<Slice> r;
    CHECK(a.Allocate(8, r));
    CHECK(r.size() == 1 && r[0] == s[3]); // reused, no new chunk
    CHECK(dev.requests.size() == 1);

    // Partial frees of a slice (a Backing is any sub-range) and coalescing back to one range.
    CHECK(a.Free(Slice{s[0].memory, 0, 3}));
    CHECK(a.Free(Slice{s[0].memory, 5, 3}));
    CHECK(a.Free(Slice{s[0].memory, 3, 2}));
    CHECK(a.GetStats().free_ranges == 1 && a.GetStats().largest_free == 8);
    for (int i = 1; i < 8; ++i) {
        CHECK(a.Free(i == 3 ? r[0] : s[i]));
    }
    auto st = a.GetStats();
    CHECK(st.used_bytes == 0 && st.free_ranges == 1 && st.largest_free == CHUNK_BLOCKS);

    // Empty chunks: kept with keep=1, freed with keep=0.
    CHECK(a.ReleaseEmptyChunks(1) == 0 && dev.live.size() == 1);
    CHECK(a.ReleaseEmptyChunks(0) == 1 && dev.live.empty());
    CHECK(a.GetStats().chunks == 0 && a.GetStats().free_ranges == 0);
    std::vector<Slice> again;
    CHECK(a.Allocate(2, again) && a.GetStats().chunks == 1); // the slot is reused
}

static void TestFragmentation() {
    FakeDevice dev;
    ResidencyAllocator a{BLOCK, CHUNK, dev.Alloc(), dev.Free()};
    std::vector<Slice> s;
    for (u64 i = 0; i < CHUNK_BLOCKS; ++i) {
        CHECK(a.Allocate(1, s));
    }
    // Free every other block: 32 one-block holes.
    for (u64 i = 0; i < CHUNK_BLOCKS; i += 2) {
        CHECK(a.Free(s[i]));
    }
    auto st = a.GetStats();
    CHECK(st.free_ranges == 32 && st.largest_free == 1);

    // 4 blocks: no hole fits, the holes are used (4 slices), no new chunk.
    std::vector<Slice> r;
    CHECK(a.Allocate(4, r));
    CHECK(r.size() == 4 && Total(r) == 4 && dev.requests.size() == 1);

    // 40 blocks with 28 one-block holes left: at most MAX_SLICES slices; the rest from a new
    // chunk.
    std::vector<Slice> big;
    CHECK(a.Allocate(40, big));
    CHECK(Total(big) == 40 && big.size() <= ResidencyAllocator::MAX_SLICES);
    CHECK(big.size() == ResidencyAllocator::MAX_SLICES);
    CHECK(big.back().blocks == 40 - (ResidencyAllocator::MAX_SLICES - 1));
    CHECK(dev.requests.size() == 2);

    // Best fit: a 3-block hole is preferred over a large free tail.
    std::vector<Slice> all = s;
    std::vector<Slice> live;
    for (u64 i = 1; i < CHUNK_BLOCKS; i += 2) {
        live.push_back(s[i]);
    }
    live.insert(live.end(), r.begin(), r.end());
    live.insert(live.end(), big.begin(), big.end());
    CHECK(Disjoint(live, dev));

    // Free three adjacent blocks of chunk 0 to make a 3-hole, then ask for 3.
    FakeDevice dev3;
    ResidencyAllocator c{BLOCK, CHUNK, dev3.Alloc(), dev3.Free()};
    std::vector<Slice> t;
    for (int i = 0; i < 10; ++i) {
        CHECK(c.Allocate(1, t)); // blocks 0..9; tail [10, 64) free
    }
    CHECK(c.Free(t[2]) && c.Free(t[3]) && c.Free(t[4]));
    std::vector<Slice> u;
    CHECK(c.Allocate(3, u));
    CHECK(u.size() == 1 && u[0].offset == 2 && u[0].blocks == 3);
}

static void TestFailure() {
    FakeDevice dev;
    ResidencyAllocator a{BLOCK, CHUNK, dev.Alloc(), dev.Free()};
    std::vector<Slice> out;
    CHECK(a.Allocate(60, out));
    // A full chunk can't be had, but the rest (6 blocks after the 4-block tail) can.
    dev.fail_above = 6 * BLOCK;
    std::vector<Slice> r;
    CHECK(a.Allocate(10, r));
    CHECK(r.size() == 2 && r[1].blocks == 6 && dev.live.size() == 2);
    CHECK(a.GetStats().failed == 1);

    // Total failure: the free 4 blocks are taken first, then rolled back; nothing leaks.
    CHECK(a.Free(r[0]));
    dev.fail_all = 1;
    const auto before = a.GetStats();
    std::vector<Slice> f{Slice{1, 2, 3}};
    CHECK(!a.Allocate(100, f));
    CHECK(f.size() == 1); // `out` unchanged
    const auto after = a.GetStats();
    CHECK(after.used_bytes == before.used_bytes && after.free_ranges == before.free_ranges);
    CHECK(after.chunks == before.chunks);
}

static void TestDestructorFrees() {
    FakeDevice dev;
    {
        ResidencyAllocator a{BLOCK, CHUNK, dev.Alloc(), dev.Free()};
        std::vector<Slice> out;
        CHECK(a.Allocate(100, out) && a.Allocate(5, out));
        CHECK(dev.live.size() == 2);
    }
    CHECK(dev.live.empty());
}

/// Random allocate/free against a shadow model.
static void TestRandom() {
    FakeDevice dev;
    ResidencyAllocator a{BLOCK, CHUNK, dev.Alloc(), dev.Free()};
    std::vector<std::vector<Slice>> held;
    u64 used = 0;
    u32 seed = 12345;
    auto rnd = [&] { return seed = seed * 1664525u + 1013904223u, seed >> 8; };
    for (int step = 0; step < 4000; ++step) {
        if (held.empty() || rnd() % 3 != 0) {
            const u64 n = 1 + rnd() % (rnd() % 8 == 0 ? 150 : 12);
            std::vector<Slice> s;
            CHECK(a.Allocate(n, s));
            CHECK(Total(s) == n && s.size() <= ResidencyAllocator::MAX_SLICES);
            used += n;
            held.push_back(std::move(s));
        } else {
            const size_t i = rnd() % held.size();
            for (const auto& x : held[i]) {
                CHECK(a.Free(x));
                used -= x.blocks;
            }
            held.erase(held.begin() + i);
        }
        if (step % 500 == 0) {
            a.ReleaseEmptyChunks(1);
        }
    }
    std::vector<Slice> all;
    for (const auto& h : held) {
        all.insert(all.end(), h.begin(), h.end());
    }
    CHECK(Disjoint(all, dev));
    const auto st = a.GetStats();
    CHECK(st.used_bytes == used * BLOCK);
    CHECK(st.chunks == dev.live.size());
    std::printf("random: %llu chunks, %llu MiB in chunks, %llu MiB used, %llu allocations, "
                "%llu free ranges\n",
                (unsigned long long)st.chunks, (unsigned long long)(st.chunk_bytes >> 20),
                (unsigned long long)(st.used_bytes >> 20), (unsigned long long)st.allocations,
                (unsigned long long)st.free_ranges);
}

int main() {
    TestBasic();
    TestSplitAcrossChunks();
    TestOversize();
    TestFreeAndReuse();
    TestFragmentation();
    TestFailure();
    TestDestructorFrees();
    TestRandom();
    if (g_failures) {
        std::fprintf(stderr, "test_residency: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("test_residency: all %d checks passed\n", g_checks);
    return 0;
}
