// SPDX-License-Identifier: GPL-2.0-or-later
// Workstream T: the texture cache's image GC policy (VideoCore::SotcGc in
// third_party/shadps4/src/video_core/texture_cache/sotc_gc_policy.h), plus the LRU walk it relies
// on (common/lru_cache.h). No Vulkan device.

#include <cstdio>
#include <vector>

#include "common/lru_cache.h"
#include "video_core/texture_cache/sotc_gc_policy.h"

using namespace VideoCore::SotcGc;

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

static constexpr u64 MiB = 1ull << 20;

static void TestThresholds() {
    // The 6 GB RTX 2060: heap budget ~5,100 MiB, minus shadPS4's 1/8 system reserve.
    const u64 budget = 4466 * MiB;
    const Thresholds t = ComputeThresholds(budget);
    CHECK(t.trigger < t.pressure && t.pressure < t.critical && t.critical < budget);
    CHECK(t.trigger >= budget / 2 - 1 && t.trigger <= budget / 2);
    CHECK(Classify(0, t) == Level::Idle);
    CHECK(Classify(t.trigger - 1, t) == Level::Idle);
    CHECK(Classify(t.trigger, t) == Level::Normal);
    CHECK(Classify(t.pressure, t) == Level::Pressured);
    CHECK(Classify(t.critical, t) == Level::Critical);
    // The failing run: 6,576 MiB used.
    CHECK(Classify(6576 * MiB, t) == Level::Critical);
    // Relative: the same fractions on a bigger card.
    const Thresholds big = ComputeThresholds(4 * budget);
    CHECK(Classify(t.critical, big) == Level::Idle);
}

static void TestConfigure() {
    const Params idle = Configure(Level::Idle);
    const Params normal = Configure(Level::Normal);
    const Params pressured = Configure(Level::Pressured);
    const Params critical = Configure(Level::Critical);
    CHECK(idle.max_frees == 0);
    // More pressure: shorter ages, more frees.
    CHECK(normal.min_age > pressured.min_age && pressured.min_age > critical.min_age);
    CHECK(normal.min_age_gpu_tiled > pressured.min_age_gpu_tiled &&
          pressured.min_age_gpu_tiled > critical.min_age_gpu_tiled);
    CHECK(normal.max_frees < pressured.max_frees && pressured.max_frees < critical.max_frees);
    // GPU-written tiled images (contents lost on free) always wait longer than re-uploadable ones.
    for (const Params& p : {normal, pressured, critical}) {
        CHECK(p.min_age_gpu_tiled > p.min_age);
        CHECK(p.min_age >= 8); // never free what the current few frames use
    }
    CHECK(!normal.download_linear && pressured.download_linear && critical.download_linear);
}

static void TestDecide() {
    const Params n = Configure(Level::Normal);
    const Params c = Configure(Level::Critical);
    // Plain textures (guest bytes current).
    CHECK(Decide(n, {n.min_age - 1, false, true}) == Action::Keep);
    CHECK(Decide(n, {n.min_age, false, true}) == Action::Free);
    CHECK(Decide(n, {n.min_age, false, false}) == Action::Free);
    // Tiled render target written by the GPU: kept until its own, longer age.
    CHECK(Decide(n, {n.min_age, true, true}) == Action::Keep);
    CHECK(Decide(n, {n.min_age_gpu_tiled - 1, true, true}) == Action::Keep);
    CHECK(Decide(n, {n.min_age_gpu_tiled, true, true}) == Action::FreeStale);
    CHECK(Decide(c, {c.min_age_gpu_tiled, true, true}) == Action::FreeStale);
    // Linear GPU-written: downloaded first, and only under pressure (as shadPS4).
    CHECK(Decide(n, {100000, true, false}) == Action::Keep);
    CHECK(Decide(c, {c.min_age, true, false}) == Action::DownloadThenFree);
    // Idle frees nothing.
    CHECK(Decide(Configure(Level::Idle), {~0ull, false, false}) == Action::Keep);
}

/// A fake image in an LRU, as TextureCache keeps them.
struct FakeImage : Common::LRUNode<> {
    bool safe_to_download;
    bool tiled;
    bool freed = false;
};

/// Runs one GC pass exactly as TextureCache::GarbageCollectImages does.
static Pass RunPass(Common::LRUCache<FakeImage>& lru, u64 gc_tick, Level level) {
    Pass pass{Configure(level)};
    if (pass.Done()) {
        return pass;
    }
    lru.ForEachItemBelow(pass.OldestTick(gc_tick), [&](FakeImage& im) {
        if (pass.Done()) {
            return true;
        }
        const Action a = pass.Next({gc_tick - im.tick, im.safe_to_download, im.tiled});
        if (a != Action::Keep) {
            im.freed = true;
            lru.Free(im);
        }
        return false;
    });
    return pass;
}

static void TestPassDoesNotStall() {
    // shadPS4's GC counted kept images against its deletion budget: 100 old render targets at the
    // head of the LRU stopped it from ever reaching the textures behind them.
    std::vector<FakeImage> images(300);
    Common::LRUCache<FakeImage> lru;
    for (size_t i = 0; i < 100; ++i) { // render targets, last used at tick 1000
        images[i].safe_to_download = true;
        images[i].tiled = true;
        lru.Insert(images[i], 1000);
    }
    for (size_t i = 100; i < 300; ++i) { // textures, last used at tick 1010
        images[i].safe_to_download = false;
        images[i].tiled = true;
        lru.Insert(images[i], 1010);
    }
    const u64 tick = 1010 + Configure(Level::Pressured).min_age;
    const Pass p = RunPass(lru, tick, Level::Pressured);
    CHECK(p.freed == Configure(Level::Pressured).max_frees);
    CHECK(p.kept == 100);
    CHECK(p.freed_stale == 0);
    for (size_t i = 0; i < 100; ++i) {
        CHECK(!images[i].freed);
    }
    // Repeated ticks free all the textures; the targets stay until they are old enough.
    u64 t = tick;
    for (int i = 0; i < 20; ++i) {
        RunPass(lru, ++t, Level::Pressured);
    }
    size_t textures_left = 0;
    for (size_t i = 100; i < 300; ++i) {
        textures_left += !images[i].freed;
    }
    CHECK(textures_left == 0);
    const Pass late = RunPass(lru, 1000 + Configure(Level::Pressured).min_age_gpu_tiled,
                              Level::Pressured);
    CHECK(late.freed_stale == Configure(Level::Pressured).max_frees);
}

static void TestPassBounds() {
    std::vector<FakeImage> images(10000);
    Common::LRUCache<FakeImage> lru;
    for (auto& im : images) { // all GPU-written and too young for FreeStale: all kept
        im.safe_to_download = true;
        im.tiled = true;
        lru.Insert(im, 0);
    }
    const Pass p = RunPass(lru, Configure(Level::Normal).min_age, Level::Normal);
    CHECK(p.visited == Pass::MAX_VISITS);
    CHECK(p.freed == 0);
    // Images younger than min_age are never visited.
    std::vector<FakeImage> recent(5);
    Common::LRUCache<FakeImage> recent_lru;
    for (auto& im : recent) {
        im.safe_to_download = false;
        im.tiled = false;
        recent_lru.Insert(im, 100);
    }
    CHECK(RunPass(recent_lru, 100 + Configure(Level::Critical).min_age - 1, Level::Critical)
              .visited == 0);
    CHECK(RunPass(recent_lru, 100 + Configure(Level::Critical).min_age, Level::Critical).freed ==
          5);
    // Idle: nothing.
    CHECK(RunPass(lru, 100000, Level::Idle).visited == 0);
    // OldestTick doesn't underflow early in the run.
    CHECK(Pass{Configure(Level::Normal)}.OldestTick(3) == 0);
}

static void TestLruStopsOnTrue() {
    // lru_cache.h used to ignore a `return true` from the callback.
    std::vector<FakeImage> images(10);
    Common::LRUCache<FakeImage> lru;
    for (auto& im : images) {
        lru.Insert(im, 0);
    }
    int calls = 0;
    lru.ForEachItemBelow(0, [&](FakeImage&) { return ++calls == 3; });
    CHECK(calls == 3);
    int all = 0;
    lru.ForEachItemBelow(0, [&](FakeImage&) { ++all; });
    CHECK(all == 10);
}

int main() {
    TestThresholds();
    TestConfigure();
    TestDecide();
    TestPassDoesNotStall();
    TestPassBounds();
    TestLruStopsOnTrue();
    std::printf("test_gc_policy: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
