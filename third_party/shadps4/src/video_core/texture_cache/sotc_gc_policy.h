// SPDX-FileCopyrightText: Copyright 2026 shadow-pc
// SPDX-License-Identifier: GPL-2.0-or-later
//
// shadow-pc patch (workstream T, docs/next/t.md): the texture cache's image GC policy, kept free
// of Vulkan so gpu/tests/test_gc_policy.cpp can test it. TextureCache::GarbageCollectImages
// feeds it the device-local usage and, per LRU image, the image's age and flags.
//
// What changed from shadPS4's GarbageCollectImages:
// - Thresholds are fractions of Instance::GetTotalMemoryBudget(). shadPS4's put the trigger at 0
//   on any card under 8 GB, and the "aggressive" pass waited for *older* images (160 ticks).
// - An image kept by the policy no longer uses up the pass's deletion budget. shadPS4 counted
//   every image it visited, so a few unfreeable images at the old end of the LRU stopped the GC
//   for good.
// - A tiled image the GPU wrote (SafeToDownload) is kept, as in shadPS4 (every render and depth
//   target is one). Freeing it without a download (FreeStale) is opt-in, SOTC_GC_FREE_STALE=1:
//   it lost textures the game renders once and samples later. Its re-creation uploaded the guest
//   bytes, which the GPU never wrote, and gameplay showed black debris, rocks and grass
//   (docs/NEXT.md, session 7).

#pragma once

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "common/types.h"

namespace VideoCore::SotcGc {

enum class Level : u8 {
    Idle,      ///< below the trigger: no GC
    Normal,    ///< free long-unused images
    Pressured, ///< shorter ages, more frees per tick, download GPU-written linear images
    Critical,  ///< near the budget: short ages, many frees
    /// A device allocation failed (EmergencyCollect, below). Never returned by Classify.
    Emergency,
};

struct Thresholds {
    u64 trigger;
    u64 pressure;
    u64 critical;
};

/// `budget` is the device-local budget (Instance::GetTotalMemoryBudget(), already minus the
/// system reserve).
constexpr Thresholds ComputeThresholds(u64 budget) {
    return {budget / 2, budget / 10 * 7, budget / 100 * 85};
}

constexpr Level Classify(u64 usage, const Thresholds& t) {
    if (usage >= t.critical) {
        return Level::Critical;
    }
    if (usage >= t.pressure) {
        return Level::Pressured;
    }
    if (usage >= t.trigger) {
        return Level::Normal;
    }
    return Level::Idle;
}

struct Params {
    Level level;
    /// Ticks (GC runs, one per sceGnmSubmitDone) unused before an image whose guest bytes are
    /// current can go. Re-creating it re-uploads exactly what it held.
    u64 min_age;
    /// Ticks unused before a tiled image the GPU wrote goes without a download (only with
    /// free_stale). Its GPU-written contents are lost: a re-creation uploads the guest bytes,
    /// which the GPU never updated.
    u64 min_age_gpu_tiled;
    /// Frees per tick (kept images don't count).
    u32 max_frees;
    /// Free GPU-written linear images after DownloadImageMemory (shadPS4: only under pressure).
    bool download_linear;
    /// Allow FreeStale at all (SOTC_GC_FREE_STALE=1). Off by default: see the top of this file.
    bool free_stale = false;
};

/// `free_stale` (SOTC_GC_FREE_STALE=1) allows FreeStale; the min_age_gpu_tiled ages apply only
/// then.
constexpr Params Configure(Level level, bool free_stale = false) {
    switch (level) {
    case Level::Normal:
        return {level, 120, 1800, 8, false, free_stale};
    case Level::Pressured:
        return {level, 30, 300, 32, true, free_stale};
    case Level::Critical:
        return {level, 8, 60, 128, true, free_stale};
    case Level::Emergency:
        // Anything not used in this tick (with free_stale, GPU-written targets too). No
        // downloads: they need GPU work and staging memory, and the pass runs in the middle of
        // an allocation.
        return {level, 1, 1, ~0u, false, free_stale};
    case Level::Idle:
    default:
        return {Level::Idle, ~0ull, ~0ull, 0, false, false};
    }
}

struct ImageState {
    u64 age;               ///< gc_tick - the image's LRU tick
    bool safe_to_download; ///< Image::SafeToDownload(): GPU-written, no newer CPU/GPU-buffer data
    bool tiled;            ///< ImageInfo::IsTiled()
    u64 bytes = 0;         ///< Image::SotcAllocatedBytes(), counted by Pass::freed_bytes
};

enum class Action : u8 {
    Keep,             ///< too young or not allowed at this level
    Free,             ///< guest bytes are current (or newer): free, re-creation is exact
    DownloadThenFree, ///< linear GPU-written image: DownloadImageMemory, then free
    FreeStale,        ///< tiled GPU-written image: free without a download
};

constexpr Action Decide(const Params& p, const ImageState& s) {
    if (p.level == Level::Idle) {
        return Action::Keep;
    }
    if (!s.safe_to_download) {
        return s.age >= p.min_age ? Action::Free : Action::Keep;
    }
    if (!s.tiled) {
        return p.download_linear && s.age >= p.min_age ? Action::DownloadThenFree : Action::Keep;
    }
    return p.free_stale && s.age >= p.min_age_gpu_tiled ? Action::FreeStale : Action::Keep;
}

/// One GC pass over the LRU, oldest first. Call Next() per image and stop when Done().
struct Pass {
    static constexpr u32 MAX_VISITS = 4096; ///< bounds the walk's CPU cost per tick

    Params params;
    u32 visited = 0;
    u32 freed = 0;      ///< all frees (Free, DownloadThenFree, FreeStale)
    u32 freed_stale = 0;
    u32 downloaded = 0;
    u32 kept = 0;
    u64 freed_bytes = 0;
    u64 target_bytes = 0; ///< stop once this much is freed (0: no target)

    explicit constexpr Pass(const Params& p, u64 target = 0) : params{p}, target_bytes{target} {}

    /// The LRU tick at or below which images are old enough to look at (min_age is the
    /// smallest age any action needs).
    constexpr u64 OldestTick(u64 gc_tick) const {
        return gc_tick - std::min(params.min_age, gc_tick);
    }

    constexpr bool Done() const {
        return params.level == Level::Idle || freed >= params.max_frees ||
               visited >= MAX_VISITS || (target_bytes != 0 && freed_bytes >= target_bytes);
    }

    constexpr Action Next(const ImageState& s) {
        ++visited;
        const Action a = Decide(params, s);
        if (a != Action::Keep) {
            freed_bytes += s.bytes;
        }
        switch (a) {
        case Action::Keep:
            ++kept;
            break;
        case Action::FreeStale:
            ++freed_stale;
            ++freed;
            break;
        case Action::DownloadThenFree:
            ++downloaded;
            ++freed;
            break;
        case Action::Free:
            ++freed;
            break;
        }
        return a;
    }
};

// ---- Emergency pass (docs/NEXT.md, session 5): vkAllocateMemory for the arena (EnsureResident)
// or vmaCreateImage failed with the heap full. TextureCache::SotcEmergencyCollect runs one pass
// with Configure(Level::Emergency) over the whole LRU, oldest first, skips images the current
// operation holds, waits for the GPU so the frees reach the driver, and the caller retries once.

/// Free at least this much, so one failure buys room for the next few allocations.
constexpr u64 EMERGENCY_MIN_BYTES = 256ull << 20;

constexpr u64 EmergencyTarget(u64 request) {
    return std::max(request * 2, EMERGENCY_MIN_BYTES);
}

/// Age buckets for the [vram] image breakdown.
constexpr u32 AgeBucket(u64 age) {
    return age < 60 ? 0 : age < 600 ? 1 : 2;
}

/// Images used within this many ticks count as "this frame's working set" in the breakdown
/// (the critical level's min_age: anything older can already go).
constexpr u64 RECENT_AGE = 8;

// ---- Diagnostics (docs/NEXT.md, session 5): is the image working set real, duplicated or
// thrashing? None of this changes what the GC does.

/// One live image's guest range, for AnalyzeOverlap.
struct GuestRange {
    u64 addr;
    u64 size;       ///< ImageInfo::guest_size
    u64 host_bytes; ///< Image::SotcAllocatedBytes()
    bool cpu_dirty; ///< CpuDirty or MaybeCpuDirty: the guest bytes changed since the upload
};

struct OverlapStats {
    u64 guest_sum = 0;    ///< sum of all guest sizes
    u64 guest_unique = 0; ///< bytes of the union of all guest ranges
    u32 overlapping = 0;  ///< images whose range intersects another live image's
    u64 overlapping_host = 0;
    u32 overlapping_dirty = 0;
    u32 groups = 0; ///< connected groups of 2+ overlapping images
};

/// A half-open address range [start, end).
struct Span {
    u64 start;
    u64 end;
};

/// Sorts `ranges` by address. Every member of a group of 2+ chained ranges overlaps at least
/// one other member: a range joins the group only if it starts below the group's highest end.
/// `unions`, if given, receives the union of all ranges, sorted and disjoint.
inline OverlapStats AnalyzeOverlap(std::vector<GuestRange>& ranges,
                                   std::vector<Span>* unions = nullptr) {
    std::sort(ranges.begin(), ranges.end(),
              [](const GuestRange& a, const GuestRange& b) { return a.addr < b.addr; });
    OverlapStats st;
    size_t i = 0;
    while (i < ranges.size()) {
        u64 end = ranges[i].addr + ranges[i].size;
        size_t j = i + 1;
        while (j < ranges.size() && ranges[j].addr < end) {
            end = std::max(end, ranges[j].addr + ranges[j].size);
            ++j;
        }
        st.guest_unique += end - ranges[i].addr;
        if (unions) {
            unions->push_back({ranges[i].addr, end});
        }
        if (j - i > 1) {
            ++st.groups;
            for (size_t k = i; k < j; ++k) {
                ++st.overlapping;
                st.overlapping_host += ranges[k].host_bytes;
                st.overlapping_dirty += ranges[k].cpu_dirty;
            }
        }
        for (size_t k = i; k < j; ++k) {
            st.guest_sum += ranges[k].size;
        }
        i = j;
    }
    return st;
}

/// Bytes in both `a` and `b`, each sorted and disjoint. Used for how much of the arena's
/// resident memory backs guest memory that live images also hold (resident twice).
inline u64 IntersectBytes(const std::vector<Span>& a, const std::vector<Span>& b) {
    u64 bytes = 0;
    size_t i = 0;
    size_t j = 0;
    while (i < a.size() && j < b.size()) {
        const u64 lo = std::max(a[i].start, b[j].start);
        const u64 hi = std::min(a[i].end, b[j].end);
        if (lo < hi) {
            bytes += hi - lo;
        }
        if (a[i].end < b[j].end) {
            ++i;
        } else {
            ++j;
        }
    }
    return bytes;
}

/// Counts images re-created at a guest address the GC freed shortly before: the GC freeing
/// something the game still uses (thrash).
class RefaultTracker {
public:
    static constexpr u64 WINDOW = 60; ///< ticks

    void OnGcFree(u64 addr, u64 tick) {
        freed[addr] = tick;
    }

    /// Returns true (and forgets the address) if the GC freed an image there within WINDOW.
    bool OnCreate(u64 addr, u64 tick) {
        const auto it = freed.find(addr);
        if (it == freed.end()) {
            return false;
        }
        const bool refault = tick - it->second <= WINDOW;
        freed.erase(it);
        return refault;
    }

    /// Drops addresses freed more than WINDOW ticks ago.
    void Prune(u64 tick) {
        std::erase_if(freed, [&](const auto& kv) { return tick - kv.second > WINDOW; });
    }

    size_t Size() const {
        return freed.size();
    }

private:
    std::unordered_map<u64, u64> freed; ///< guest address -> GC tick it was freed at
};

} // namespace VideoCore::SotcGc
