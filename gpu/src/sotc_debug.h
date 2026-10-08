// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc debug tools (workstream X), implemented in debug_dump.cpp:
// - SOTC_DUMP_RT=<dir>: render-target dump of one frame on the next screenshot tick.
// - SOTC_GPU_STATS=1: per-second counters (draws, dispatches, waits by call site, copies,
//   faults) and, every 10 s, the top faulting pages (session 10 drain diagnostic).
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
    SrtEmulated, ///< SRT walker loads served from the CPU copy (sotcgpu_emulate_load)
    SrtAliased,  ///< SRT walker tables read through the backing view (SrtResolveTable)
    // BufferCache::DownloadMemory, by when the GPU wrote the data (SotcWriteTicks):
    DownloadInBatch,     ///< in the batch still being recorded (needs a flush)
    DownloadSubmitted,   ///< in a submitted batch the GPU hasn't finished
    DownloadDone,        ///< in work the GPU has finished
    DownloadUnknownTick, ///< no written binding recorded for it
    SideReadback,        ///< downloads done by a side submission (no Scheduler::Finish)
    SrtPatchedTables,    ///< walker tables with GPU-written constants patched on the GPU
    SrtPatchedDwords,    ///< flattened-buffer dwords copied from the arena for them
    HotFlush,            ///< batches submitted after a dispatch wrote a hot readback page
    Count,
};
extern std::array<std::atomic<u64>, size_t(Counter::Count)> g_counters;

/// Who called Scheduler::Wait (directly or through Finish). Callers tag themselves with a
/// WaitScope; a blocking wait counts under the innermost tag of the waiting thread.
enum class WaitSite : u8 {
    Other,
    BufDownloadGpu,    ///< BufferCache::DownloadMemory inside a GPU-thread fault
    BufDownloadQueued, ///< BufferCache::DownloadMemory queued by a guest fault (SendCommand)
    ImageDownload,     ///< TextureCache::DownloadImageMemory(sync) from ProcessDownloadImages
    GcEmergency,       ///< TextureCache emergency GC
    GdsStore,          ///< EVENT_WRITE_EOS GDS store (Rasterizer::Finish)
    StreamBuffer,      ///< StreamBuffer::WaitPendingOperations
    FaultBuffer,       ///< FaultManager::ProcessFaultBuffer
    Present,           ///< presenter schedulers
    Count,
};
extern std::array<std::atomic<u64>, size_t(WaitSite::Count)> g_wait_sites;
extern thread_local WaitSite t_wait_site;

struct WaitScope {
    explicit WaitScope(WaitSite site) : prev{t_wait_site} {
        t_wait_site = site;
    }
    ~WaitScope() {
        t_wait_site = prev;
    }
    WaitScope(const WaitScope&) = delete;
    WaitScope& operator=(const WaitScope&) = delete;
    WaitSite prev;
};

/// PM4 packet the GPU thread is processing: opcode | 0x100 for the compute queues,
/// NoPm4 outside packet processing. Only maintained while stats are on.
inline constexpr u32 NoPm4 = 0xffff;
extern thread_local u32 t_pm4;

[[nodiscard]] inline bool StatsEnabled() {
    return (g_flags.load(std::memory_order_relaxed) & StatsOn) != 0;
}

inline void Count(Counter c, u64 n = 1) {
    if (StatsEnabled()) [[unlikely]] {
        g_counters[size_t(c)].fetch_add(n, std::memory_order_relaxed);
    }
}

/// Scheduler::Wait, when the wait blocks (stats on).
inline void CountWait() {
    g_counters[size_t(Counter::SchedulerWait)].fetch_add(1, std::memory_order_relaxed);
    g_wait_sites[size_t(t_wait_site)].fetch_add(1, std::memory_order_relaxed);
}

/// Liverpool: the packet about to be processed (opcode | 0x100 on compute), or NoPm4 after it.
inline void SetPm4(u32 op) {
    if (StatsEnabled()) [[unlikely]] {
        t_pm4 = op;
    }
}

/// sotcgpu_handle_fault (stats on): one guest-memory fault, before it is dispatched.
void OnFault(VAddr addr, bool is_write, bool gpu_thread, bool gpu_modified, u64 rip);
/// BufferCache::ReadMemory (stats on): the fault at `addr` downloaded GPU data, i.e. drained the
/// GPU. `gpu_thread`: the download ran inside a GPU-thread fault (else queued by a guest fault).
void OnFaultDrain(VAddr addr, bool gpu_thread);
/// BufferCache::DownloadMemory (stats on): the CPU copy and the downloaded GPU data of a range.
/// The 10 s report lists, per top drain page, the dword ranges where they differed.
void OnDownloadCompare(VAddr addr, const u8* cpu, const u8* gpu, u64 size);
/// BufferCache::ObtainBuffer (stats on, GPU thread): a binding the GPU may write, which marks
/// [addr, addr + size) GPU-modified. The report lists the ones covering the top drain pages.
void OnGpuWrite(VAddr addr, u64 size);

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
