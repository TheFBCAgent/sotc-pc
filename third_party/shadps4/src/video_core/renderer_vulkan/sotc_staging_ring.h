// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (session 23): the policy of StagingBufferPool's rings of 16 MiB stream buffers
// (vk_staging_buffer_pool.cpp). Vulkan-free; test: gpu/tests/test_staging_ring.cpp.
//
// Upstream tries the blocks round robin from the current one and adds a block when none can
// take a request without waiting, then frees a block once it has been unused for 300 frames.
// Session 23 counted SotC: the uncached ring grew to 78 blocks (1.25 GB) while one loading batch
// requested ~1 GB of uploads, and never shrank: round robin touches every block every ~40
// frames, and the pool's TickFrame ran only from OnSubmit, which stops in gameplay. Now:
// - the current block first, then the lowest index up, so the low blocks carry the steady
//   uploads and the surplus goes idle;
// - a block unused for IdleFrames frames (ticked per flip) is freed;
// - past SoftCap blocks the ring doesn't grow: the pool submits the batch and waits for the
//   block the GPU frees first.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace SotcStagingRing {

constexpr std::uint64_t IdleFrames = 60;
constexpr std::size_t SoftCap = 32; ///< blocks (512 MiB of 16 MiB blocks)

/// Tries blocks in policy order; `try_block(i)` reserves in block i or returns false.
/// Returns the block that took the request.
template <class TryBlock>
std::optional<std::size_t> FindBlock(std::size_t count, std::size_t current, TryBlock&& try_block) {
    if (current < count && try_block(current)) {
        return current;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (i != current && try_block(i)) {
            return i;
        }
    }
    return std::nullopt;
}

/// Whether a block unused since `last_used_frame` may be freed; `is_free()` (its last batch is
/// done) is only asked for an idle block.
template <class IsFree>
bool ShouldTrim(std::uint64_t frame, std::uint64_t last_used_frame, IsFree&& is_free) {
    return frame - last_used_frame >= IdleFrames && is_free();
}

/// Whether a request no block can take grows the ring (false: submit and wait instead).
inline bool MayGrow(std::size_t count) {
    return count < SoftCap;
}

/// Past the cap: the block to wait on, the one whose blocking tick is oldest (0 = not
/// blocked, taken first). `tick(i)` gives block i's blocking tick.
template <class Tick>
std::size_t OldestBlock(std::size_t count, Tick&& tick) {
    std::size_t best = 0;
    for (std::size_t i = 1; i < count; ++i) {
        if (tick(i) < tick(best)) {
            best = i;
        }
    }
    return best;
}

} // namespace SotcStagingRing
