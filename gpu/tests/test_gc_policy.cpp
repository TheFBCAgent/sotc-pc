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
    // FreeStale only with free_stale (SOTC_GC_FREE_STALE=1); TestKeepGpuWritten has the default.
    const Params n = Configure(Level::Normal, true);
    const Params c = Configure(Level::Critical, true);
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

/// The default (session 7, black debris): no level, emergency included, frees a tiled GPU-written
/// image; SOTC_GC_FREE_STALE=1 allows it again. Everything else is decided the same either way.
static void TestKeepGpuWritten() {
    for (const Level level : {Level::Normal, Level::Pressured, Level::Critical, Level::Emergency}) {
        const Params keep = Configure(level);
        const Params stale = Configure(level, true);
        CHECK(!keep.free_stale && stale.free_stale);
        CHECK(Decide(keep, {~0ull, true, true}) == Action::Keep);
        CHECK(Decide(stale, {~0ull, true, true}) == Action::FreeStale);
        CHECK(Decide(keep, {keep.min_age, false, true}) == Action::Free);
        CHECK(Decide(keep, {~0ull, true, false}) == Decide(stale, {~0ull, true, false}));
    }
    CHECK(!Configure(Level::Idle, true).free_stale);
    Pass pass{Configure(Level::Emergency), EmergencyTarget(1)};
    CHECK(pass.Next({100, true, true, 64 * MiB}) == Action::Keep);
    CHECK(pass.freed == 0 && pass.freed_bytes == 0 && !pass.Done());
}

/// A fake image in an LRU, as TextureCache keeps them.
struct FakeImage : Common::LRUNode<> {
    bool safe_to_download;
    bool tiled;
    u64 bytes = 0;
    bool freed = false;
};

/// Runs one GC pass exactly as TextureCache::GarbageCollectImages does (SotcEmergencyCollect
/// with a byte target and Level::Emergency).
/// `free_stale` defaults to true here: most cases below test FreeStale's ages and budgets.
static Pass RunPass(Common::LRUCache<FakeImage>& lru, u64 gc_tick, Level level, u64 target = 0,
                    bool free_stale = true) {
    Pass pass{Configure(level, free_stale), target};
    if (pass.Done()) {
        return pass;
    }
    lru.ForEachItemBelow(pass.OldestTick(gc_tick), [&](FakeImage& im) {
        if (pass.Done()) {
            return true;
        }
        const Action a = pass.Next({gc_tick - im.tick, im.safe_to_download, im.tiled, im.bytes});
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

static void TestEmergency() {
    const Params e = Configure(Level::Emergency, true);
    // Only images used in this tick (age 0) are safe; everything else can go (with free_stale,
    // GPU-written tiled targets included). No downloads in the middle of an allocation.
    CHECK(e.min_age == 1 && e.min_age_gpu_tiled == 1 && !e.download_linear);
    CHECK(Decide(e, {0, false, true}) == Action::Keep);
    CHECK(Decide(e, {0, true, true}) == Action::Keep);
    CHECK(Decide(e, {1, false, true}) == Action::Free);
    CHECK(Decide(e, {1, true, true}) == Action::FreeStale);
    CHECK(Decide(e, {1000, true, false}) == Action::Keep); // linear GPU-written: needs a download
    // Classify never returns it.
    const Thresholds t = ComputeThresholds(4466 * MiB);
    CHECK(Classify(~0ull, t) == Level::Critical);
    // Target: twice the failed request, at least EMERGENCY_MIN_BYTES.
    CHECK(EmergencyTarget(0) == EMERGENCY_MIN_BYTES);
    CHECK(EmergencyTarget(1 * MiB) == EMERGENCY_MIN_BYTES);
    CHECK(EmergencyTarget(512 * MiB) == 1024 * MiB);

    // The crash: ~1000 live images, all used in the last few ticks (the critical GC already
    // freed anything older than 8). 400 used this tick, 600 over the last 7 ticks, 4 MiB each.
    std::vector<FakeImage> images(1000);
    Common::LRUCache<FakeImage> lru;
    const u64 tick = 5000;
    for (size_t i = 0; i < 600; ++i) {
        images[i].safe_to_download = i % 3 == 0; // some render targets
        images[i].tiled = true;
        images[i].bytes = 4 * MiB;
        lru.Insert(images[i], tick - 7 + i / 100); // ticks 4993..4998, oldest first
    }
    for (size_t i = 600; i < 1000; ++i) {
        images[i].safe_to_download = false;
        images[i].tiled = true;
        images[i].bytes = 4 * MiB;
        lru.Insert(images[i], tick);
    }
    // The critical pass can't free anything (all younger than 8 ticks)...
    CHECK(RunPass(lru, tick, Level::Critical).freed == 0);
    // ...the emergency pass frees exactly up to the target, the oldest first, and past the
    // critical max_frees.
    const u64 target = EmergencyTarget(300 * MiB); // 600 MiB = 150 images
    const Pass p = RunPass(lru, tick, Level::Emergency, target);
    CHECK(p.freed == 150);
    CHECK(p.freed_bytes == target);
    CHECK(p.freed_stale == 50);
    for (size_t i = 0; i < 1000; ++i) {
        CHECK(images[i].freed == (i < 150));
    }
    // Without a target it takes everything older than this tick and stops there.
    const Pass all = RunPass(lru, tick, Level::Emergency);
    CHECK(all.freed == 450);
    for (size_t i = 600; i < 1000; ++i) {
        CHECK(!images[i].freed);
    }
    CHECK(RunPass(lru, tick, Level::Emergency).visited == 0);
}

static void TestIntersect() {
    using V = std::vector<Span>;
    CHECK(IntersectBytes({}, {}) == 0);
    CHECK(IntersectBytes(V{{0, 100}}, {}) == 0);
    // Resident arena ranges vs. image ranges.
    const V arena = {{0x1000, 0x5000}, {0x8000, 0x9000}, {0x10000, 0x20000}};
    const V images = {{0x0, 0x2000}, {0x4000, 0x8800}, {0x12000, 0x13000}, {0x1f000, 0x30000}};
    CHECK(IntersectBytes(arena, images) == 0x1000 + 0x1000 + 0x800 + 0x1000 + 0x1000);
    CHECK(IntersectBytes(images, arena) == IntersectBytes(arena, images));
    CHECK(IntersectBytes(arena, arena) == 0x4000 + 0x1000 + 0x10000);
    CHECK(IntersectBytes(V{{0, 0x1000}}, V{{0x1000, 0x2000}}) == 0); // touching, half-open

    // AnalyzeOverlap's union output feeds it.
    std::vector<GuestRange> r = {{0x4000, 0x1000, 0, false},
                                 {0x1000, 0x2000, 0, false},
                                 {0x2000, 0x2000, 0, false}};
    V u;
    AnalyzeOverlap(r, &u);
    CHECK(u.size() == 2);
    CHECK(u[0].start == 0x1000 && u[0].end == 0x4000);
    CHECK(u[1].start == 0x4000 && u[1].end == 0x5000);
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

static void TestAnalyzeOverlap() {
    std::vector<GuestRange> none;
    const OverlapStats empty = AnalyzeOverlap(none);
    CHECK(empty.guest_sum == 0 && empty.guest_unique == 0 && empty.overlapping == 0);

    // Unsorted input. [0x1000,+0x1000) and [0x1800,+0x1000) overlap, [0x2400,+0x100) chains onto
    // the second; [0x2800,+0x800) only touches the end of the group (no overlap); [0x8000,+0x1000)
    // is alone. A stale copy at the same address as the lone one overlaps it exactly.
    std::vector<GuestRange> r = {
        {0x8000, 0x1000, 10, false}, {0x1800, 0x1000, 20, true}, {0x2800, 0x800, 30, false},
        {0x1000, 0x1000, 40, false}, {0x2400, 0x100, 50, true},  {0x8000, 0x1000, 60, true},
    };
    const OverlapStats st = AnalyzeOverlap(r);
    CHECK(st.guest_sum == 0x1000 + 0x1000 + 0x800 + 0x1000 + 0x100 + 0x1000);
    CHECK(st.guest_unique == 0x1800 + 0x800 + 0x1000);
    CHECK(st.groups == 2);
    CHECK(st.overlapping == 5);
    CHECK(st.overlapping_host == 40 + 20 + 50 + 10 + 60);
    CHECK(st.overlapping_dirty == 3);
    CHECK(r.front().addr == 0x1000); // sorted in place

    // A big image containing small ones (a reused pool slot): every member overlaps.
    std::vector<GuestRange> pool = {
        {0x10000, 0x10000, 1, false}, {0x11000, 0x100, 1, false}, {0x1f000, 0x1000, 1, false}};
    const OverlapStats p = AnalyzeOverlap(pool);
    CHECK(p.groups == 1 && p.overlapping == 3 && p.guest_unique == 0x10000);
}

static void TestRefaultTracker() {
    RefaultTracker t;
    CHECK(!t.OnCreate(0x1000, 5)); // never freed
    t.OnGcFree(0x1000, 100);
    t.OnGcFree(0x2000, 100);
    CHECK(t.Size() == 2);
    CHECK(t.OnCreate(0x1000, 100 + RefaultTracker::WINDOW)); // re-created in the window
    CHECK(!t.OnCreate(0x1000, 101));                          // counted once
    CHECK(!t.OnCreate(0x2000, 101 + RefaultTracker::WINDOW)); // too late
    t.OnGcFree(0x3000, 10);
    t.OnGcFree(0x4000, 50);
    t.Prune(50 + RefaultTracker::WINDOW);
    CHECK(t.Size() == 1);
    CHECK(t.OnCreate(0x4000, 60));
}

int main() {
    TestThresholds();
    TestConfigure();
    TestDecide();
    TestKeepGpuWritten();
    TestPassDoesNotStall();
    TestPassBounds();
    TestEmergency();
    TestIntersect();
    TestLruStopsOnTrue();
    TestAnalyzeOverlap();
    TestRefaultTracker();
    std::printf("test_gc_policy: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
