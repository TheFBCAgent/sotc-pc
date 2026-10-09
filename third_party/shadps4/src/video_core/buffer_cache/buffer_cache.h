// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <deque>
#include <map>
#include <boost/container/small_vector.hpp>

#include "common/interval_set.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/fault_manager.h"
#include "video_core/buffer_cache/range_set.h"
#include "video_core/buffer_cache/sotc_prefetch.h"
#include "video_core/buffer_cache/sotc_write_ticks.h"
#include "video_core/buffer_cache/residency_allocator.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"
#include "video_core/texture_cache/sotc_gc_policy.h" // shadow-pc patch: SotcGc::Span

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {
class GraphicsPipeline;
struct SubmitInfo;
class Runtime;
class StagingBufferPool;
} // namespace Vulkan

namespace VideoCore {

class TextureCache;
class MemoryTracker;
class PageManager;

class BufferCache {
    static constexpr u64 ADDRESS_SPACE_BITS = 40;
    static constexpr u64 ARENA_PAGE_BITS = 32;
    static constexpr u64 ARENA_PAGE_SIZE = u64{1} << ARENA_PAGE_BITS;
    static constexpr u64 NUM_ARENA_PAGES = u64{1} << (ADDRESS_SPACE_BITS - ARENA_PAGE_BITS);
    static constexpr u64 MIN_BLOCK_SIZE = 16_KB;
    static constexpr u64 STREAM_THRESHOLD = 16_KB;

public:
    explicit BufferCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         Vulkan::Runtime& runtime, AmdGpu::Liverpool* liverpool,
                         TextureCache& texture_cache, PageManager& tracker);
    ~BufferCache();

    /// Returns a pointer to GDS device local buffer.
    [[nodiscard]] const Buffer* GetGdsBuffer() const noexcept {
        return &gds_buffer;
    }

    /// Retrieves the device local DBA page table buffer.
    [[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept {
        return bda_pagetable_buffer.get();
    }

    /// Retrieves the fault buffer.
    [[nodiscard]] Buffer* GetFaultBuffer() noexcept {
        return fault_manager->GetFaultBuffer();
    }

    /// Retrieves the stream buffer.
    [[nodiscard]] StreamBuffer& GetStreamBuffer() noexcept {
        return stream_buffer;
    }

    /// Returns minimum granularity of a sparse memory bind.
    u32 GetSparsePageShift() const noexcept {
        return block_shift;
    }

    void TickFrame();

    /// Invalidates any buffer in the logical page range.
    void InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks = false);

    /// Flushes any GPU modified buffer in the logical page range back to CPU memory.
    void ReadMemory(VAddr device_addr, u64 size, bool is_write = false, bool assume_locks = false);

    /// Finds a buffer for the specified region.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBuffer(VAddr device_addr, u32 size,
                                                             bool is_written,
                                                             bool is_texel_buffer = false);

    /// Attempts to obtain a buffer without modifying the cache contents.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBufferForImage(VAddr device_addr, u32 size);

    /// Return true when a region is modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(VAddr addr, size_t size);

    /// Return true when a region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(VAddr addr, size_t size);

    /// shadow-pc (session 10): a written binding touched a page where the GPU's data was
    /// recently needed mid-batch; the caller submits the batch after its dispatch (no wait).
    [[nodiscard]] bool TakeSotcFlushRequest() noexcept {
        return std::exchange(sotc_flush_requested, false);
    }

    /// shadow-pc (session 10): byte-precise, unlike IsRegionGpuModified (4 KiB pages): may the
    /// GPU have written any byte of [addr, addr + size) since the last download? GPU thread only.
    [[nodiscard]] bool IsRangeGpuWritten(VAddr addr, size_t size) const {
        return gpu_modified_ranges.Intersects(addr, size);
    }

    /// shadow-pc (session 21, SOTC_CPU_COPY): the next written binding inside [addr, addr + size)
    /// holds data the CPU already wrote into guest memory (Rasterizer::SotcCpuCopy), so it
    /// isn't marked in gpu_modified_ranges or the write ticks. Cleared after the dispatch's
    /// BindResources (size 0).
    void SotcSetCpuMirroredWrite(VAddr addr, u64 size) noexcept {
        sotc_mirrored_addr = addr;
        sotc_mirrored_size = size;
        if (size == 0) {
            sotc_check_addr = 0;
        }
    }

    /// shadow-pc (session 21, SOTC_CPU_COPY=2): remembers the CPU's result of a copy dispatch
    /// (still marked GPU-written) until another written binding overlaps it; DownloadMemory
    /// compares downloaded dwords with it (counters CpuCopyCheckEqual/Differ).
    void SotcCheckCpuCopy(VAddr addr, const u32* values, u32 count);

    /// shadow-pc (session 13): at the end of each batch (scheduler submit callback), copies the
    /// GPU-written pieces of hot readback pages this batch wrote into the prefetch ring.
    void SotcPrefetchAtBatchEnd();

    /// Synchronizes all buffers needed for DMA.
    void SynchronizeDmaBuffers();

    /// Commits pending sparse buffer memory binds. Must be called before every scheduler submit.
    void SubmitPendingArenaBinds(Vulkan::SubmitInfo& info);

    /// shadow-pc patch (docs/NEXT.md, session 5): publishes how much of the arena's resident
    /// memory lies inside `image_spans` (live images' guest ranges, sorted and disjoint) to
    /// SotcDiag::arena_ranges_bytes / arena_image_bytes. GPU thread, with the image breakdown.
    void SotcPublishArenaCoverage(const std::vector<SotcGc::Span>& image_spans);

private:
    struct ArenaBinds {
        const Buffer* arena;
        boost::container::small_vector<vk::SparseMemoryBind, 32> binds;
    };

    ArenaBinds* BindsForArena(const Buffer* arena) {
        auto it = std::ranges::find(pending_binds, arena, &ArenaBinds::arena);
        if (it != pending_binds.end()) {
            return std::addressof(*it);
        }
        return &pending_binds.emplace_back(arena);
    }

    const Buffer* GetArena(u64 first_block, u64 last_block);

    void EnsureResident(const Buffer* arena, u64 first_block, u64 last_block);

    /// shadow-pc patch (workstream R): copies the residency allocator's stats to SotcDiag.
    void PublishResidencyStats();

    /// shadow-pc patch: uploads the ranges streamed since the last call into the arena.
    void FlushStreamedRanges();

    /// shadow-pc: returns whether anything was downloaded (i.e. the GPU was drained).
    bool DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size);

    /// shadow-pc (session 10): see buffer_cache.cpp.
    static bool SotcSideReadbackEnabled();
    static bool SotcHotFlushEnabled();
    static bool SotcTrimWindowEnabled();
    static bool SotcFineTicksEnabled();
    void SotcSideCopy(const Buffer* arena, vk::Buffer dst, std::span<const vk::BufferCopy> copies,
                      u64 write_tick);
    static bool SotcPrefetchEnabled();
    /// Serves a download from the prefetch ring when it covers every copy (see sotc_prefetch.h).
    bool SotcServeFromPrefetch(VAddr arena_base, std::span<const vk::BufferCopy> copies);
    struct SotcSideReadback {
        vk::CommandPool pool;
        vk::CommandBuffer cmdbuf;
        vk::Fence fence;
    } sotc_side;

    bool SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size, bool is_written,
                           bool is_texel_buffer);

    bool SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    Vulkan::Runtime& runtime;
    Vulkan::StagingBufferPool& staging_pool;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    TextureCache& texture_cache;
    std::unique_ptr<MemoryTracker> memory_tracker;

    StreamBuffer stream_buffer;
    Buffer gds_buffer;
    RangeSet gpu_modified_ranges;
    SotcWriteTicks sotc_write_ticks; ///< shadow-pc: newest GPU write per 64 KiB / 4 KiB (sessions 10, 17)
    /// shadow-pc (session 10): 256-byte lines a fault read while their GPU data was still in
    /// the batch being recorded. A later binding written over one asks for a flush right after
    /// its dispatch, so the reader finds the data in a submitted batch (a side copy).
    static constexpr u64 SotcHotLineShift = 8;
    std::vector<u64> sotc_hot_lines;
    bool sotc_flush_requested{};
    VAddr sotc_mirrored_addr{}; ///< SotcSetCpuMirroredWrite
    u64 sotc_mirrored_size{};
    std::map<VAddr, std::vector<u32>> sotc_check_copies; ///< SotcCheckCpuCopy, disjoint
    VAddr sotc_check_addr{}; ///< the record of the dispatch being bound (kept by ObtainBuffer)
    void SotcEraseCheckCopies(VAddr addr, u64 size);
    void SotcCompareCheckCopies(VAddr guest, const u8* data, u64 size);
    bool sotc_download_in_batch{}; ///< the last DownloadMemory needed the batch being recorded
    /// shadow-pc (session 13): readback prefetch (hot pages, ring entries) and its ring buffer.
    static constexpr u64 SotcPrefetchRingSize = 32_MB;
    SotcPrefetch sotc_prefetch{SotcPrefetchRingSize};
    std::unique_ptr<Buffer> sotc_prefetch_ring;
    std::vector<SotcPrefetch::Segment> sotc_prefetch_segments; ///< SotcServeFromPrefetch scratch
    RangeSet streamed_ranges; // shadow-pc patch: see FlushStreamedRanges

    std::unique_ptr<FaultManager> fault_manager;
    std::unique_ptr<Buffer> bda_pagetable_buffer;
    bool fault_process_pending{};

    std::array<const Buffer*, NUM_ARENA_PAGES> address_space{};
    // shadow-pc patch (workstream R): arena residency comes from chunks of this allocator.
    // Declared before `arenas` so the sparse buffers are destroyed before their memory.
    std::unique_ptr<ResidencyAllocator> residency;
    std::vector<ResidencyAllocator::Slice> residency_slices; ///< EnsureResident scratch
    vk::Result residency_alloc_result{vk::Result::eSuccess}; ///< last chunk allocation
    std::deque<Buffer> arenas;
    std::vector<ArenaBinds> pending_binds;
    Vulkan::Semaphore memory_semaphore;

    struct Backing : public Interval {
        vk::DeviceMemory memory;
        u64 offset;
        constexpr bool CanMergeWith(const Backing& other) const noexcept {
            return memory == other.memory && offset + (end - start) == other.offset;
        }
        constexpr Backing SubRange(u64 a, u64 b) const noexcept {
            return {{a, b}, memory, offset + (a - start)};
        }
    };
    IntervalList<Backing> resident_ranges;

    u32 arena_memory_type_index{};    u32 block_size{};
    u32 block_shift{};
    u32 blocks_per_arena_page{};
    u32 blocks_per_arena_page_shift{};
};

} // namespace VideoCore
