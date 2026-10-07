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
// - A tiled image the GPU wrote (SafeToDownload) can go without a download once it is old
//   enough; the age shrinks with memory pressure. shadPS4 never freed such images, and every
//   render and depth target is one.

#pragma once

#include <algorithm>

#include "common/types.h"

namespace VideoCore::SotcGc {

enum class Level : u8 {
    Idle,      ///< below the trigger: no GC
    Normal,    ///< free long-unused images
    Pressured, ///< shorter ages, more frees per tick, download GPU-written linear images
    Critical,  ///< near the budget: short ages, many frees
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
    /// Ticks unused before a tiled image the GPU wrote goes without a download. Its GPU-written
    /// contents are lost: a re-creation uploads the guest bytes, which the GPU never updated.
    u64 min_age_gpu_tiled;
    /// Frees per tick (kept images don't count).
    u32 max_frees;
    /// Free GPU-written linear images after DownloadImageMemory (shadPS4: only under pressure).
    bool download_linear;
};

constexpr Params Configure(Level level) {
    switch (level) {
    case Level::Normal:
        return {level, 120, 1800, 8, false};
    case Level::Pressured:
        return {level, 30, 300, 32, true};
    case Level::Critical:
        return {level, 8, 60, 128, true};
    case Level::Idle:
    default:
        return {Level::Idle, ~0ull, ~0ull, 0, false};
    }
}

struct ImageState {
    u64 age;               ///< gc_tick - the image's LRU tick
    bool safe_to_download; ///< Image::SafeToDownload(): GPU-written, no newer CPU/GPU-buffer data
    bool tiled;            ///< ImageInfo::IsTiled()
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
    return s.age >= p.min_age_gpu_tiled ? Action::FreeStale : Action::Keep;
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

    explicit constexpr Pass(const Params& p) : params{p} {}

    /// The LRU tick at or below which images are old enough to look at (min_age is the
    /// smallest age any action needs).
    constexpr u64 OldestTick(u64 gc_tick) const {
        return gc_tick - std::min(params.min_age, gc_tick);
    }

    constexpr bool Done() const {
        return params.level == Level::Idle || freed >= params.max_frees || visited >= MAX_VISITS;
    }

    constexpr Action Next(const ImageState& s) {
        ++visited;
        const Action a = Decide(params, s);
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

/// Age buckets for the [vram] image breakdown.
constexpr u32 AgeBucket(u64 age) {
    return age < 60 ? 0 : age < 600 ? 1 : 2;
}

} // namespace VideoCore::SotcGc
