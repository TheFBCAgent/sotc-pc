// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>
#include <vector>

#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"

namespace Vulkan {

class Instance;
class Scheduler;

struct StagingBufferRef {
    VideoCore::Buffer* buffer{};
    u64 offset{};
    u64 size{};
    u8* mapped{};
    VideoCore::MemoryType type{};

    void Flush() const noexcept {
        buffer->Flush(offset, size);
    }

    void Invalidate() const noexcept {
        buffer->Invalidate(offset, size);
    }
};

class StagingBufferPool {
public:
    explicit StagingBufferPool(const Instance& instance, Scheduler& scheduler);
    ~StagingBufferPool();

    StagingBufferPool(const StagingBufferPool&) = delete;
    StagingBufferPool& operator=(const StagingBufferPool&) = delete;

    /// Requests size bytes of memory of the given type.
    [[nodiscard]] StagingBufferRef Request(u64 size, VideoCore::MemoryType type, u64 alignment = 0,
                                           bool deferred = false, bool unsynchronized = false);

    /// Releases a deferred allocation. It becomes reusable once the current tick completes.
    void FreeDeferred(const StagingBufferRef& ref);

    /// Releases idle memory.
    void TickFrame();

    /// shadow-pc (session 23): per flip (Rasterizer::SotcGcTick). The tick itself when
    /// SOTC_STAGING_RING is on (default), and the "[vram] staging:" stats either way.
    void SotcFlipTick();
    static bool SotcRingEnabled();

private:
    struct Block {
        std::unique_ptr<VideoCore::StreamBuffer> buffer;
        u64 last_used_frame;
    };

    struct Ring {
        std::vector<Block> blocks;
        size_t current = 0;
    };

    struct LargeBuffer {
        std::unique_ptr<VideoCore::Buffer> buffer;
        u64 tick;
        u64 last_used_frame;
        bool held;
    };

    static constexpr size_t NUM_TYPES = 3;

    StagingBufferRef RequestFromRing(Ring& ring, u64 size, u64 alignment,
                                     VideoCore::MemoryType type, bool unsynchronized);
    StagingBufferRef RequestLarge(u64 size, VideoCore::MemoryType type, bool deferred,
                                  bool unsynchronized);

    void DoTickFrame();
    void TrimRing(Ring& ring);
    void TrimLarge(std::vector<LargeBuffer>& cache);

    void PublishStats() const;

    // shadow-pc (session 23): why the rings grow. Counted per memory type, reset every ~10 s by
    // PublishSotcStats, which writes Vulkan::SotcDiag::staging_breakdown ("[vram] staging:").
    struct SotcRingStats {
        u64 requests = 0;
        u64 bytes = 0;
        u64 max_request = 0;
        u64 new_blocks = 0;
        u64 trimmed_blocks = 0;
        u64 peak_blocks = 0;
        u64 capped_waits = 0;      ///< requests that waited instead of growing past SoftCap
        u64 unsync = 0;            ///< unsynchronized requests that overwrote a block
        u64 fail_in_batch = 0;     ///< at growth: blocks waiting for the batch being recorded
        u64 fail_submitted = 0;    ///< ...for a submitted batch the GPU hasn't finished
        u64 max_behind = 0;        ///< at growth: CurrentTick - newest finished tick
        u64 max_block_age = 0;     ///< at growth: CurrentTick - the oldest blocking tick
        u64 growth_request = 0;    ///< at growth: largest request that grew the ring
        u64 batch_bytes = 0;       ///< bytes requested in the batch `batch_tick`
        u64 batch_tick = 0;
        u64 max_batch_bytes = 0;   ///< most bytes requested in one batch
        u64 large_new = 0;
        u64 large_requests = 0;
        u64 large_bytes = 0;
    };
    void SotcCountRequest(SotcRingStats& st, u64 size);
    void PublishSotcStats();
    std::array<SotcRingStats, NUM_TYPES> sotc_stats{};
    u64 sotc_last_publish_ns = 0;

    const Instance& instance;
    Scheduler& scheduler;

    std::array<Ring, NUM_TYPES> rings;
    std::array<std::vector<LargeBuffer>, NUM_TYPES> large_caches;
    u64 frame = 0;
};

} // namespace Vulkan
