// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <mutex>
#include <string>

#include <fmt/format.h>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/types.h"
#include "core/devtools/widget/gpu_memory.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/sotc_staging_ring.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"

namespace Vulkan {

using VideoCore::MemoryType;

namespace {

constexpr u64 RING_IDLE_FRAMES = 300;
constexpr u64 LARGE_IDLE_FRAMES = 120;
constexpr u64 LARGE_MIN_GRANULARITY = 64_KB;
constexpr u64 BLOCK_SIZE = 16_MB;

u64 RoundAllocationSize(u64 size) {
    const u64 step = std::max(std::bit_floor(size) / 4, LARGE_MIN_GRANULARITY);
    return Common::AlignUp(size, step);
}

} // Anonymous namespace

StagingBufferPool::StagingBufferPool(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {}

StagingBufferPool::~StagingBufferPool() = default;

StagingBufferRef StagingBufferPool::Request(u64 size, MemoryType type, u64 alignment, bool deferred,
                                            bool unsynchronized) {
    Ring& ring = rings[u32(type)];
    SotcCountRequest(sotc_stats[u32(type)], size);
    if (deferred || size > BLOCK_SIZE) {
        return RequestLarge(size, type, deferred, unsynchronized);
    }
    return RequestFromRing(ring, size, alignment, type, unsynchronized);
}

StagingBufferRef StagingBufferPool::RequestFromRing(Ring& ring, u64 size, u64 alignment,
                                                    MemoryType type, bool unsynchronized) {
    Block* best{};
    u64 best_offset{};
    // shadow-pc (session 23): the current block, then the lowest index up (sotc_staging_ring.h),
    // so surplus blocks go idle and get trimmed. Upstream went round robin from the current one.
    const bool sotc = SotcRingEnabled();
    auto try_block = [&](size_t i) {
        const auto offset = ring.blocks[i].buffer->Reserve(size, alignment, false);
        best_offset = offset.value_or(0);
        return offset.has_value();
    };
    std::optional<size_t> index;
    if (sotc) {
        index = SotcStagingRing::FindBlock(ring.blocks.size(), ring.current, try_block);
    } else {
        for (size_t i = 0; i < ring.blocks.size() && !index; ++i) {
            if (const size_t at = (ring.current + i) % ring.blocks.size(); try_block(at)) {
                index = at;
            }
        }
    }
    if (index) {
        ring.current = *index;
        best = &ring.blocks[*index];
    }

    if (!best) {
        SotcRingStats& st = sotc_stats[u32(type)];
        if (!ring.blocks.empty() && unsynchronized) {
            ++st.unsync;
            best = &ring.blocks[(ring.current + 1) % ring.blocks.size()];
            best_offset = 0;
        } else {
            // shadow-pc (session 23): why no block could take the request.
            const u64 now = scheduler.CurrentTick();
            const u64 done = scheduler.GetWorkSemaphore()->KnownGpuTick();
            for (const Block& block : ring.blocks) {
                const u64 t = block.buffer->BlockingTick();
                if (t >= now) {
                    ++st.fail_in_batch;
                } else if (t != 0) {
                    ++st.fail_submitted;
                    st.max_block_age = std::max(st.max_block_age, now - t);
                }
            }
            st.max_behind = std::max(st.max_behind, now > done ? now - done : 0);
            st.growth_request = std::max(st.growth_request, size);
        }
        if (!best && sotc && !SotcStagingRing::MayGrow(ring.blocks.size())) {
            // Past the cap: wait for the block the GPU frees first (Scheduler::Wait submits
            // the batch being recorded if that block waits for it).
            const size_t index = SotcStagingRing::OldestBlock(
                ring.blocks.size(), [&](size_t i) { return ring.blocks[i].buffer->BlockingTick(); });
            ++st.capped_waits;
            ring.current = index;
            best = &ring.blocks[index];
            best_offset = *best->buffer->Reserve(size, alignment, true);
        }
        if (!best) {
            ++st.new_blocks;
            best = &ring.blocks.emplace_back(Block{
                .buffer = std::make_unique<VideoCore::StreamBuffer>(instance, scheduler, type,
                                                                    BLOCK_SIZE),
                .last_used_frame = frame,
            });
            ring.current = ring.blocks.size() - 1;
            best_offset = *best->buffer->Reserve(size, alignment, false);
            st.peak_blocks = std::max<u64>(st.peak_blocks, ring.blocks.size());
        }
    }

    auto& buffer = *best->buffer;
    best->last_used_frame = frame;

    StagingBufferRef ref{
        .buffer = &buffer,
        .offset = best_offset,
        .size = size,
        .type = type,
    };
    if (!buffer.mapped_data.empty()) {
        ref.mapped = buffer.mapped_data.data() + best_offset;
    }
    return ref;
}

StagingBufferRef StagingBufferPool::RequestLarge(u64 size, MemoryType type, bool deferred,
                                                 bool unsynchronized) {
    auto& cache = large_caches[u32(type)];
    const u64 max_size = std::max(size + size / 4, RoundAllocationSize(size));

    LargeBuffer* best{};
    for (LargeBuffer& entry : cache) {
        const u64 entry_size = entry.buffer->SizeBytes();
        if (entry.held || entry_size < size || entry_size > max_size ||
            (!scheduler.IsFree(entry.tick) && !unsynchronized)) {
            continue;
        }
        if (!best || entry_size < best->buffer->SizeBytes()) {
            best = &entry;
        }
    }

    SotcRingStats& st = sotc_stats[u32(type)];
    ++st.large_requests;
    if (!best) {
        ++st.large_new;
        best = &cache.emplace_back(LargeBuffer{
            .buffer = std::make_unique<VideoCore::Buffer>(instance, 0, RoundAllocationSize(size),
                                                          type, "StagingBufferPool:Dedicated"),
            .tick = 0,
            .last_used_frame = frame,
            .held = false,
        });
    }

    best->tick = scheduler.CurrentTick();
    best->last_used_frame = frame;
    best->held = deferred;

    VideoCore::Buffer& buffer = *best->buffer;
    StagingBufferRef ref{
        .buffer = &buffer,
        .offset = 0,
        .size = size,
        .type = type,
    };
    if (!buffer.mapped_data.empty()) {
        ref.mapped = buffer.mapped_data.data();
    }
    return ref;
}

void StagingBufferPool::FreeDeferred(const StagingBufferRef& ref) {
    auto& cache = large_caches[u32(ref.type)];
    const auto it = std::ranges::find_if(
        cache, [&](const LargeBuffer& entry) { return entry.buffer.get() == ref.buffer; });
    ASSERT_MSG(it != cache.end() && it->held, "FreeDeferred on an allocation that is not held");
    it->held = false;
    it->tick = scheduler.CurrentTick();
    it->last_used_frame = frame;
}

bool StagingBufferPool::SotcRingEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("SOTC_STAGING_RING");
        const bool on = !(env && *env == '0');
        LOG_INFO(Render_Vulkan, "SOTC_STAGING_RING {}", on ? "on" : "off (upstream ring)");
        return on;
    }();
    return enabled;
}

void StagingBufferPool::TickFrame() {
    // shadow-pc (session 23): with SOTC_STAGING_RING on, the pool ticks per flip instead
    // (SotcFlipTick); OnSubmit, the upstream caller, stops in gameplay.
    if (!SotcRingEnabled()) {
        DoTickFrame();
    }
}

void StagingBufferPool::SotcFlipTick() {
    if (SotcRingEnabled()) {
        DoTickFrame();
    }
    PublishSotcStats();
}

void StagingBufferPool::DoTickFrame() {
    ++frame;
    for (Ring& ring : rings) {
        TrimRing(ring);
    }
    for (auto& cache : large_caches) {
        TrimLarge(cache);
    }
    PublishStats();
}

void StagingBufferPool::TrimRing(Ring& ring) {
    auto& blocks = ring.blocks;
    for (size_t i = blocks.size(); i-- > 0;) {
        const Block& block = blocks[i];
        // shadow-pc (session 23): SotcStagingRing::IdleFrames (60) instead of 300.
        auto is_free = [&] { return scheduler.IsFree(block.buffer->LastTick()); };
        if (SotcRingEnabled() ? !SotcStagingRing::ShouldTrim(frame, block.last_used_frame, is_free)
                              : frame - block.last_used_frame < RING_IDLE_FRAMES || !is_free()) {
            continue;
        }
        blocks.erase(blocks.begin() + i);
        ++sotc_stats[&ring - rings.data()].trimmed_blocks;
        if (i < ring.current) {
            --ring.current;
        } else if (i == ring.current) {
            ring.current = 0;
        }
    }
    if (ring.current >= blocks.size()) {
        ring.current = 0;
    }
}

void StagingBufferPool::TrimLarge(std::vector<LargeBuffer>& cache) {
    std::erase_if(cache, [this](const LargeBuffer& entry) {
        return !entry.held && frame - entry.last_used_frame >= LARGE_IDLE_FRAMES &&
               scheduler.IsFree(entry.tick);
    });
}

void StagingBufferPool::PublishStats() const {
    using namespace Core::Devtools::Widget;
    if (!GpuMemoryViewer::IsEnabled()) {
        return;
    }
    static constexpr std::array<const char*, NUM_TYPES> names = {"DeviceLocal", "HostUncached",
                                                                 "HostCached"};
    GpuMemoryGroup group{.name = "Staging"};
    for (size_t i = 0; i < NUM_TYPES; ++i) {
        u64 bytes = 0;
        for (const Block& block : rings[i].blocks) {
            bytes += block.buffer->SizeBytes();
        }
        for (const LargeBuffer& entry : large_caches[i]) {
            bytes += entry.buffer->SizeBytes();
        }
        if (bytes != 0) {
            group.rows.push_back({.name = names[i], .bytes = bytes});
        }
    }
    GpuMemoryViewer::Publish(std::move(group));
}

void StagingBufferPool::SotcCountRequest(SotcRingStats& st, u64 size) {
    ++st.requests;
    st.bytes += size;
    st.max_request = std::max(st.max_request, size);
    const u64 tick = scheduler.CurrentTick();
    if (tick != st.batch_tick) {
        st.batch_tick = tick;
        st.batch_bytes = 0;
    }
    st.batch_bytes += size;
    st.max_batch_bytes = std::max(st.max_batch_bytes, st.batch_bytes);
}

void StagingBufferPool::PublishSotcStats() {
    const u64 now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    if (sotc_last_publish_ns == 0) {
        sotc_last_publish_ns = now_ns;
        return;
    }
    if (now_ns - sotc_last_publish_ns < 10'000'000'000ull) {
        return;
    }
    sotc_last_publish_ns = now_ns;
    constexpr u64 MiB = 1ull << 20;
    static constexpr std::array<const char*, NUM_TYPES> names = {"device", "uncached", "cached"};
    std::string s;
    for (size_t i = 0; i < NUM_TYPES; ++i) {
        SotcRingStats& st = sotc_stats[i];
        u64 large_bytes = 0;
        for (const LargeBuffer& entry : large_caches[i]) {
            large_bytes += entry.buffer->SizeBytes();
        }
        if (rings[i].blocks.empty() && large_caches[i].empty() && st.requests == 0) {
            continue;
        }
        s += fmt::format(
            "{}{}: {} blocks ({} MiB), new {} trimmed {} peak {}; {} req {} MiB max {} KiB, "
            "batch max {} MiB; capped waits {}; growth: in-batch {} submitted {} (oldest {} ticks), gpu behind "
            "{} ticks, req {} KiB; unsync {}; large {} ({} MiB), new {} of {} req",
            s.empty() ? "" : " | ", names[i], rings[i].blocks.size(),
            rings[i].blocks.size() * BLOCK_SIZE / MiB, st.new_blocks, st.trimmed_blocks,
            st.peak_blocks, st.requests, st.bytes / MiB, st.max_request >> 10,
            st.max_batch_bytes / MiB, st.capped_waits, st.fail_in_batch, st.fail_submitted, st.max_block_age,
            st.max_behind, st.growth_request >> 10, st.unsync, large_caches[i].size(),
            large_bytes / MiB, st.large_new, st.large_requests);
        const u64 batch_tick = st.batch_tick;
        const u64 batch_bytes = st.batch_bytes;
        st = {};
        st.batch_tick = batch_tick;
        st.batch_bytes = batch_bytes;
    }
    std::scoped_lock lk{SotcDiag::staging_mutex};
    SotcDiag::staging_breakdown = std::move(s);
}

} // namespace Vulkan
