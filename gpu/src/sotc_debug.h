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
    WindowTrimmed,       ///< fault downloads whose window SotcWriteTicks::TrimWindow shrank
    Submit,              ///< Scheduler::SubmitExecution calls (every scheduler, graphics queue)
    Flip,                ///< SubmitFlipInternal
    GpuThreadWaitNs,     ///< blocking Scheduler::Wait + side-copy fence time on the GPU thread
    OtherWaitNs,         ///< the same on other threads (presenter, guest threads)
    SideCopyWaitNs,      ///< of those, side-copy fence waits (any thread)
    // Session 13 step 1 (OnDownloadPages): downloads whose 64 KiB pages were all downloaded in
    // the previous 2 frames too (a prefetch of hot pages would have covered them), by write tick.
    DownloadRecentInBatch,
    DownloadRecentSubmitted,
    DownloadRecentDone,
    DownloadPages,       ///< distinct 64 KiB pages downloaded this second
    DownloadBytes,       ///< bytes downloaded (all paths)
    // Session 13 readback prefetch (BufferCache::SotcServeFromPrefetch, SotcPrefetchAtBatchEnd):
    PrefetchHit,             ///< downloads served from the prefetch ring
    PrefetchHitWaited,       ///< of those, served after waiting for a submitted batch
    PrefetchMissNotHot,      ///< a page that wasn't hot before this download
    PrefetchMissNoEntry,     ///< a hot page with no entry (not written since it became hot)
    PrefetchMissPartial,     ///< an entry that doesn't cover every byte
    PrefetchMissOverwritten, ///< the ring reused the entry's bytes
    PrefetchMissStale,       ///< a newer batch wrote the bytes after the copy
    PrefetchPages,           ///< hot pages copied at batch end
    PrefetchBytes,           ///< bytes copied at batch end
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
/// Set on Liverpool's command processor thread (OnGpuThreadStart).
extern thread_local bool t_gpu_thread;

/// Liverpool::Process, first thing on the GPU thread.
inline void OnGpuThreadStart() {
    t_gpu_thread = true;
}

/// Time a thread spent blocked on the GPU (stats on): Scheduler::Wait, side-copy fences.
inline void AddWaitNs(u64 ns) {
    g_counters[size_t(t_gpu_thread ? Counter::GpuThreadWaitNs : Counter::OtherWaitNs)].fetch_add(
        ns, std::memory_order_relaxed);
}

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

/// Program hash of the compute shader whose bindings are being set up (0 outside a dispatch).
/// Only maintained while stats are on (session 17: the writer of in-batch drains).
extern thread_local u64 t_shader;
extern thread_local u64 t_dispatch_seq; ///< counts SetShader(non-zero) calls
/// Rasterizer::DispatchDirect/Indirect, before BindResources (0 after the dispatch).
inline void SetShader(u64 hash) {
    if (StatsEnabled()) [[unlikely]] {
        t_shader = hash;
        t_dispatch_seq += hash != 0 ? 1 : 0;
    }
}

/// sotcgpu_handle_fault (stats on): one guest-memory fault, before it is dispatched.
void OnFault(VAddr addr, bool is_write, bool gpu_thread, bool gpu_modified, u64 rip);
/// BufferCache::ReadMemory (stats on): the fault at `addr` downloaded GPU data, i.e. drained the
/// GPU. `gpu_thread`: the download ran inside a GPU-thread fault (else queued by a guest fault).
/// `in_batch`: it needed the batch being recorded (Scheduler::Finish); `was_hot`: its 256-byte
/// line was already a hot line (the hot flush should have submitted it); `hot_full`: the hot
/// line list is full; `request_in_batch`: the faulting range's own 64 KiB was written in the
/// batch being recorded (else only the 512 KiB download window was). In-batch drains are tabled
/// by page, reader and writer (session 15). `tick`: the scheduler's current tick (session 17:
/// how many submits since the same line/page last needed an in-batch drain).
void OnFaultDrain(VAddr addr, bool gpu_thread, bool in_batch = false, bool was_hot = false,
                  bool hot_full = false, bool request_in_batch = false, u64 tick = 0);
/// What a download waited for, by SotcWriteTicks (BufferCache::DownloadMemory).
enum class DownloadKind : u8 { InBatch, Submitted, Done, Unknown };
/// BufferCache::DownloadMemory (stats on, GPU thread): the distinct 64 KiB pages one download
/// copies. Counts whether every page was also downloaded in the previous 2 frames (session 13).
void OnDownloadPages(DownloadKind kind, const u64* pages, size_t count);
/// BufferCache::DownloadMemory (stats on): the CPU copy and the downloaded GPU data of a range.
/// The 10 s report lists, per top drain page, the dword ranges where they differed.
void OnDownloadCompare(VAddr addr, const u8* cpu, const u8* gpu, u64 size);
/// BufferCache::ObtainBuffer (stats on, GPU thread): a binding the GPU may write, which marks
/// [addr, addr + size) GPU-modified. The report lists the ones covering the top drain pages.
/// `tick`: the scheduler tick being recorded (session 17: was the read line itself written in
/// the batch, or only a neighbour in its 64 KiB).
void OnGpuWrite(VAddr addr, u64 size, u64 tick);

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
