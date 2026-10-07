// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc debug tools (workstream X), implemented in debug_dump.cpp:
// - SOTC_DUMP_RT=<dir>: render-target dump of one frame on the next screenshot tick.
// - SOTC_GPU_STATS=1: per-second counters (draws, dispatches, waits, copies, faults).
// - SOTC_GPU_DUMP=1: shadPS4's dump_shaders setting.
// Hooks live in sotcgpu.cpp, shim_core.cpp and two listed patches (vk_rasterizer.cpp,
// vk_scheduler.cpp). Each hook is one relaxed load and a branch while the variables are unset.
#pragma once

#include <array>
#include <atomic>
#include "common/types.h"
#include "video_core/texture_cache/types.h"

namespace VideoCore {
class TextureCache;
}
namespace Vulkan {
class Rasterizer;
}

namespace SotcDebug {

enum Flags : u32 {
    StatsOn = 1u << 0,  ///< SOTC_GPU_STATS
    RecordRt = 1u << 1, ///< a SOTC_DUMP_RT frame is being recorded
};
extern std::atomic<u32> g_flags;

enum class Counter : u32 {
    Draw,
    DrawIndirect,
    Dispatch,
    SchedulerWait,
    CopyBytes,
    ReadFault,
    WriteFault,
    Count,
};
extern std::array<std::atomic<u64>, size_t(Counter::Count)> g_counters;

[[nodiscard]] inline bool StatsEnabled() {
    return (g_flags.load(std::memory_order_relaxed) & StatsOn) != 0;
}

inline void Count(Counter c, u64 n = 1) {
    if (StatsEnabled()) [[unlikely]] {
        g_counters[size_t(c)].fetch_add(n, std::memory_order_relaxed);
    }
}

/// Out-of-line part of OnDraw: records the bound targets of one draw (GPU thread).
void RecordTargets(bool indirect, VideoCore::TextureCache& tc, const VideoCore::ImageId* colors,
                   u32 num_colors, VideoCore::ImageId depth);

/// Rasterizer hook, after BeginRendering: `cbs` is Rasterizer::cb_descs (entries below
/// `num_cbs` with a valid image_id are bound), `db` is Rasterizer::db_desc.
template <typename CbDescs, typename DbDesc>
inline void OnDraw(bool indirect, VideoCore::TextureCache& tc, const CbDescs& cbs, u32 num_cbs,
                   const DbDesc& db) {
    const u32 flags = g_flags.load(std::memory_order_relaxed);
    if (flags == 0) [[likely]] {
        return;
    }
    if (flags & StatsOn) {
        g_counters[size_t(indirect ? Counter::DrawIndirect : Counter::Draw)].fetch_add(
            1, std::memory_order_relaxed);
    }
    if (flags & RecordRt) {
        std::array<VideoCore::ImageId, 8> ids{};
        u32 n = 0;
        for (u32 i = 0; i < num_cbs && i < ids.size(); ++i) {
            ids[n++] = cbs[i].image_id;
        }
        RecordTargets(indirect, tc, ids.data(), n, db.image_id);
    }
}

// sotcgpu.cpp hooks.
/// sotcgpu_init: reads the variables, sets dump_shaders, starts the stats thread.
void Init();
/// sotcgpu_shutdown, while the device is still alive.
void Shutdown();
/// sotcgpu_screenshot (every --shots tick): arms the render-target dump.
void OnScreenshotTick();
/// SubmitFlipInternal (GPU thread): frame boundary for the render-target dump.
void OnFlip(Vulkan::Rasterizer& rasterizer);

} // namespace SotcDebug
