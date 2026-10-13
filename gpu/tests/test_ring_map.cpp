// SPDX-License-Identifier: GPL-2.0-or-later
// Whole-draw memo (W3): VideoCore::SotcRingMap, guest address -> (ring id, offset) for memory a
// game's per-frame allocators hand out. Addresses below are made up.
#include <chrono>
#include <cstdio>
#include <map>
#include <random>

#include "video_core/buffer_cache/sotc_ring_map.h"

using VideoCore::SotcRingMap;
using Result = SotcRingMap::Result;

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

static constexpr u64 MiB = u64{1} << 20;

int main() {
    {
        SotcRingMap map;
        CHECK(map.Empty() && !map.Find(0x1000));
        CHECK(map.Register(0x1000, 0, 1) == Result::Empty);
        CHECK(map.Register(~u64{0} - 4, 16, 1) == Result::Empty); // wraps
        CHECK(map.Generation() == 0);

        // Two rings with a gap between them.
        const u64 a = 0x1040000000, b = 0x1050000000;
        CHECK(map.Register(b, 32 * MiB, 2) == Result::Added);
        CHECK(map.Register(a, 32 * MiB, 1) == Result::Added);
        CHECK(map.Count() == 2 && map.Generation() == 2);
        CHECK(map.begin()[0].base == a && map.begin()[1].base == b); // sorted

        const auto h = map.Find(a + 0x1230);
        CHECK(h && h->id == 1 && h->offset == 0x1230 && h->base == a && h->size == 32 * MiB);
        CHECK(map.Find(a) && map.Find(a)->offset == 0);
        CHECK(map.Find(a + 32 * MiB - 1) && map.Find(a + 32 * MiB - 1)->id == 1);
        CHECK(!map.Find(a + 32 * MiB)); // the gap
        CHECK(!map.Find(a - 1));
        CHECK(map.Find(b + 5) && map.Find(b + 5)->id == 2);
        CHECK(!map.Find(b + 32 * MiB));

        // FindRange: the whole range must lie in one ring.
        CHECK(map.FindRange(a + 0x100, 0xb00) && map.FindRange(a + 0x100, 0xb00)->offset == 0x100);
        CHECK(map.FindRange(a + 32 * MiB - 16, 16));
        CHECK(!map.FindRange(a + 32 * MiB - 16, 17));
        CHECK(!map.FindRange(a, 0));

        // Overlap with another id is rejected; touching it is fine.
        CHECK(map.Register(a + 31 * MiB, 2 * MiB, 3) == Result::Overlap);
        CHECK(map.Register(a + 32 * MiB, MiB, 3) == Result::Added); // touches ring 1 on the left
        CHECK(map.Register(b - MiB, MiB, 4) == Result::Added);      // touches ring 2 on the right
        CHECK(map.Find(a + 32 * MiB)->id == 3 && map.Find(b - 1)->id == 4);
        CHECK(map.Count() == 4);

        // Same id inside: present, no generation change.
        const u64 gen = map.Generation();
        CHECK(map.Register(a + MiB, MiB, 1) == Result::Present);
        CHECK(map.Generation() == gen);

        // Unregister and clear.
        CHECK(map.Unregister(3) == 1 && map.Unregister(4) == 1 && map.Unregister(9) == 0);
        CHECK(map.Count() == 2 && map.Generation() == gen + 2);
        map.Clear();
        CHECK(map.Empty() && !map.Find(a) && map.Generation() == gen + 3);
    }
    {
        // Learning one 1 MiB page at a time: touching pages of the same id merge into a window,
        // a page that fills a gap joins two windows, a page to the left moves the base.
        SotcRingMap map;
        const u64 base = 0x1100000000;
        CHECK(map.Register(base + 4 * MiB, MiB, 7) == Result::Added);
        CHECK(map.Register(base + 5 * MiB, MiB, 7) == Result::Merged);
        CHECK(map.Register(base + 8 * MiB, MiB, 7) == Result::Added);
        CHECK(map.Count() == 2);
        CHECK(map.Register(base + 6 * MiB, 2 * MiB, 7) == Result::Merged); // fills the gap
        CHECK(map.Count() == 1);
        CHECK(map.Find(base + 4 * MiB)->size == 5 * MiB);
        const u64 gen = map.Generation();
        CHECK(map.Find(base + 6 * MiB)->offset == 2 * MiB);
        CHECK(map.Register(base + 3 * MiB, MiB, 7) == Result::Merged); // left: base moves
        CHECK(map.Generation() == gen + 1);
        CHECK(map.Find(base + 6 * MiB)->offset == 3 * MiB);
        // Overlapping several same-id windows at once.
        CHECK(map.Register(base + 20 * MiB, MiB, 7) == Result::Added);
        CHECK(map.Register(base + 22 * MiB, MiB, 7) == Result::Added);
        CHECK(map.Register(base + 2 * MiB, 30 * MiB, 7) == Result::Merged);
        CHECK(map.Count() == 1 && map.Find(base + 31 * MiB)->offset == 29 * MiB);
        // A merge stops at a ring with another id that it only touches.
        CHECK(map.Register(base + 32 * MiB, MiB, 8) == Result::Added);
        CHECK(map.Register(base + 31 * MiB, MiB, 7) == Result::Present);
        CHECK(map.Count() == 2 && map.Find(base + 32 * MiB)->id == 8);
    }
    {
        // Full.
        SotcRingMap map;
        for (u64 i = 0; i < SotcRingMap::MaxRings; ++i) {
            CHECK(map.Register(0x2000000000 + i * 2 * MiB, MiB, static_cast<u32>(i)) ==
                  Result::Added);
        }
        CHECK(map.Register(0x3000000000, MiB, 99) == Result::Full);
        // Merging needs no new entry, so it still works.
        CHECK(map.Register(0x2000000000 + MiB, MiB / 2, 0) == Result::Merged);
    }
    {
        // Random check against a per-page model (16 KiB pages, page -> id).
        std::mt19937_64 rng(1234);
        SotcRingMap map;
        std::map<u64, u32> model; // page index -> id
        constexpr u64 Page = 16 << 10;
        constexpr u64 Pages = 512;
        const u64 base = 0x1000000000;
        for (int step = 0; step < 4000; ++step) {
            const u64 first = rng() % Pages;
            const u64 n = 1 + rng() % 8;
            const u32 id = static_cast<u32>(rng() % 4);
            if (rng() % 16 == 0) {
                map.Unregister(id);
                for (auto it = model.begin(); it != model.end();) {
                    it = it->second == id ? model.erase(it) : std::next(it);
                }
                continue;
            }
            bool clash = false;
            for (u64 p = first; p < first + n; ++p) {
                if (auto it = model.find(p); it != model.end() && it->second != id) {
                    clash = true;
                }
            }
            const Result r = map.Register(base + first * Page, n * Page, id);
            if (clash) {
                CHECK(r == Result::Overlap);
                continue;
            }
            if (r == Result::Full) {
                continue;
            }
            CHECK(r != Result::Overlap && r != Result::Empty);
            for (u64 p = first; p < first + n; ++p) {
                model[p] = id;
            }
            for (u64 p = 0; p < Pages; ++p) {
                const auto h = map.Find(base + p * Page + 8);
                const auto it = model.find(p);
                if (it == model.end()) {
                    CHECK(!h);
                } else {
                    CHECK(h && h->id == it->second && h->base + h->offset == base + p * Page + 8);
                }
            }
            if (g_failures > 20) {
                break;
            }
        }
        // Entries stay sorted and disjoint, same-id neighbours never touch.
        const SotcRingMap::Ring* prev = nullptr;
        for (const auto& r : map) {
            if (prev) {
                CHECK(prev->End() <= r.base);
                CHECK(!(prev->End() == r.base && prev->id == r.id));
            }
            prev = &r;
        }
    }
    {
        // Lookup cost with a full map (stage B calls it per binding).
        SotcRingMap map;
        for (u64 i = 0; i < SotcRingMap::MaxRings; ++i) {
            map.Register(0x2000000000 + i * 64 * MiB, 32 * MiB, static_cast<u32>(i));
        }
        std::mt19937_64 rng(5);
        constexpr int N = 1 << 22;
        u64 sum = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < N; ++i) {
            const u64 addr = 0x2000000000 + (rng() % (SotcRingMap::MaxRings * 64 * MiB));
            if (const auto h = map.Find(addr)) {
                sum += h->offset;
            }
        }
        const double ns =
            std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0)
                .count() /
            N;
        std::printf("Find with %zu rings: %.1f ns per lookup (incl. rng) [%llu]\n", map.Count(), ns,
                    static_cast<unsigned long long>(sum & 1));
    }
    if (g_failures == 0) {
        std::printf("test_ring_map: all passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
