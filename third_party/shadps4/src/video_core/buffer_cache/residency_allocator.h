// SPDX-FileCopyrightText: Copyright 2026 shadow-pc contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// shadow-pc patch (workstream R, docs/next/r.md): suballocator for the sparse arena's residency
// memory. BufferCache::EnsureResident used to make one vkAllocateMemory per call and never free
// it; with ~2,000 calls a session it hit NVIDIA's maxMemoryAllocationCount (4096) and failed with
// ErrorOutOfDeviceMemory. This class carves large device-memory chunks into block-granular slices.
//
// The core (offsets, free lists) does not depend on Vulkan: device memory is an opaque non-zero
// u64 handle, created and destroyed through injected callbacks, so it is unit-tested without a
// GPU (gpu/tests/test_residency.cpp).

#pragma once

#include <compare>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

#include "common/types.h"

namespace VideoCore {

class ResidencyAllocator {
public:
    /// Size of a regular chunk. A request whose remainder exceeds it gets a dedicated chunk of
    /// exactly that many blocks.
    static constexpr u64 DEFAULT_CHUNK_BYTES = 256ull << 20;
    /// A request is split into at most this many slices (sparse binds). Past it, the rest comes
    /// from a new chunk.
    static constexpr u32 MAX_SLICES = 16;

    /// Allocates `bytes` of device memory. Returns an opaque non-zero handle, or 0 on failure.
    using AllocateFn = std::function<u64(u64 bytes)>;
    /// Frees a handle returned by AllocateFn.
    using FreeFn = std::function<void(u64 memory)>;

    /// A block-granular piece of one chunk. `offset` and `blocks` are in blocks.
    struct Slice {
        u64 memory;
        u64 offset;
        u64 blocks;
        bool operator==(const Slice&) const = default;
    };

    struct Stats {
        u64 chunks;        ///< live chunks (= live vkAllocateMemory objects)
        u64 chunk_bytes;   ///< bytes allocated in them
        u64 used_bytes;    ///< bytes handed out in slices
        u64 allocations;   ///< successful AllocateFn calls, cumulative
        u64 failed;        ///< failed AllocateFn calls, cumulative
        u64 frees;         ///< FreeFn calls, cumulative
        u64 free_ranges;   ///< free ranges across all chunks (fragmentation)
        u64 largest_free;  ///< largest free range, in blocks
    };

    ResidencyAllocator(u64 block_bytes, u64 chunk_bytes, AllocateFn allocate, FreeFn free);
    /// Frees every chunk. The caller makes sure the GPU no longer uses them.
    ~ResidencyAllocator();

    ResidencyAllocator(const ResidencyAllocator&) = delete;
    ResidencyAllocator& operator=(const ResidencyAllocator&) = delete;

    /// Allocates `blocks` blocks as one or more slices, appended to `out` in order. Prefers one
    /// contiguous slice (best fit); otherwise fills free ranges, largest first, and takes the rest
    /// from a new chunk. Returns false, with `out` unchanged and nothing allocated, if device
    /// memory could not be allocated.
    bool Allocate(u64 blocks, std::vector<Slice>& out);

    /// Returns a block range to its chunk. It may be any sub-range of what Allocate handed out,
    /// within one chunk (e.g. a BufferCache Backing). Returns false, changing nothing, if the
    /// range is not inside a live chunk or overlaps a free range (double free).
    bool Free(const Slice& slice);

    /// Frees chunks with nothing allocated in them, keeping at most `keep` empty regular chunks
    /// (dedicated chunks are always released). Returns how many were freed. The caller makes sure
    /// the GPU no longer uses them (sparse binds removed and completed).
    u32 ReleaseEmptyChunks(u32 keep);

    Stats GetStats() const;

    u64 BlockBytes() const {
        return block_bytes;
    }
    u64 ChunkBlocks() const {
        return chunk_blocks;
    }

private:
    struct Chunk {
        u64 memory{};
        u64 blocks{};
        u64 used{};
        std::map<u64, u64> free; ///< offset -> length, in blocks; coalesced
        bool live{};
    };
    struct FreeKey {
        u64 blocks;
        u32 chunk;
        u64 offset;
        auto operator<=>(const FreeKey&) const = default;
    };

    /// Takes `n` blocks from the start of the free range `key`.
    Slice Take(FreeKey key, u64 n);
    bool FreeLocked(const Slice& slice);
    /// Allocates a chunk of `blocks` blocks; returns its index or -1.
    int NewChunk(u64 blocks);
    void InsertFree(u32 chunk, u64 offset, u64 blocks);
    void EraseFree(u32 chunk, std::map<u64, u64>::iterator it);

    const u64 block_bytes;
    const u64 chunk_blocks;
    AllocateFn allocate_fn;
    FreeFn free_fn;

    mutable std::mutex mutex;
    std::vector<Chunk> chunks; ///< indices are stable; dead slots are reused
    std::unordered_map<u64, u32> chunk_of_memory;
    std::set<FreeKey> free_index; ///< every free range, ordered by size
    u64 used_blocks{};
    u64 allocations{};
    u64 failed{};
    u64 frees{};
};

} // namespace VideoCore
