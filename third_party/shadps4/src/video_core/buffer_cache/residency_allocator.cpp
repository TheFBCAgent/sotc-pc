// SPDX-FileCopyrightText: Copyright 2026 shadow-pc contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// shadow-pc patch (workstream R): see residency_allocator.h.

#include <algorithm>
#include <iterator>

#include "video_core/buffer_cache/residency_allocator.h"

namespace VideoCore {

ResidencyAllocator::ResidencyAllocator(u64 block_bytes_, u64 chunk_bytes, AllocateFn allocate,
                                       FreeFn free)
    : block_bytes{block_bytes_},
      chunk_blocks{std::max<u64>(1, chunk_bytes / std::max<u64>(1, block_bytes_))},
      allocate_fn{std::move(allocate)}, free_fn{std::move(free)} {}

ResidencyAllocator::~ResidencyAllocator() {
    for (const Chunk& c : chunks) {
        if (c.live) {
            free_fn(c.memory);
        }
    }
}

void ResidencyAllocator::InsertFree(u32 chunk, u64 offset, u64 blocks) {
    chunks[chunk].free.emplace(offset, blocks);
    free_index.insert({blocks, chunk, offset});
}

void ResidencyAllocator::EraseFree(u32 chunk, std::map<u64, u64>::iterator it) {
    free_index.erase({it->second, chunk, it->first});
    chunks[chunk].free.erase(it);
}

ResidencyAllocator::Slice ResidencyAllocator::Take(FreeKey key, u64 n) {
    Chunk& c = chunks[key.chunk];
    EraseFree(key.chunk, c.free.find(key.offset));
    if (key.blocks > n) {
        InsertFree(key.chunk, key.offset + n, key.blocks - n);
    }
    c.used += n;
    used_blocks += n;
    return {c.memory, key.offset, n};
}

int ResidencyAllocator::NewChunk(u64 blocks) {
    const u64 memory = allocate_fn(blocks * block_bytes);
    if (memory == 0) {
        ++failed;
        return -1;
    }
    ++allocations;
    u32 index = 0;
    while (index < chunks.size() && chunks[index].live) {
        ++index;
    }
    if (index == chunks.size()) {
        chunks.emplace_back();
    }
    Chunk& c = chunks[index];
    c = Chunk{};
    c.memory = memory;
    c.blocks = blocks;
    c.live = true;
    chunk_of_memory[memory] = index;
    InsertFree(index, 0, blocks);
    return static_cast<int>(index);
}

bool ResidencyAllocator::Allocate(u64 blocks, std::vector<Slice>& out) {
    if (blocks == 0) {
        return true;
    }
    std::scoped_lock lk{mutex};

    // 1. One contiguous slice: the smallest free range that fits.
    if (auto it = free_index.lower_bound({blocks, 0, 0}); it != free_index.end()) {
        out.push_back(Take(*it, blocks));
        return true;
    }

    // 2. Fill free ranges, largest first (none fits the whole request). Stop at the first one
    //    that fits what is left, or when the slice budget is spent.
    const size_t first = out.size();
    u64 remaining = blocks;
    while (remaining > 0 && !free_index.empty() && out.size() - first + 1 < MAX_SLICES) {
        if (auto it = free_index.lower_bound({remaining, 0, 0}); it != free_index.end()) {
            out.push_back(Take(*it, remaining));
            remaining = 0;
            break;
        }
        const FreeKey largest = *std::prev(free_index.end());
        out.push_back(Take(largest, largest.blocks));
        remaining -= largest.blocks;
    }

    // 3. The rest from a new chunk: a regular one, or a dedicated one if the rest is larger.
    //    Near exhaustion, retry with exactly the rest.
    if (remaining > 0) {
        int chunk = NewChunk(std::max(chunk_blocks, remaining));
        if (chunk < 0 && remaining < chunk_blocks) {
            chunk = NewChunk(remaining);
        }
        if (chunk < 0) {
            for (size_t i = first; i < out.size(); ++i) {
                FreeLocked(out[i]);
            }
            out.resize(first);
            return false;
        }
        const auto& c = chunks[chunk];
        out.push_back(Take({c.free.begin()->second, static_cast<u32>(chunk), 0}, remaining));
    }
    return true;
}

bool ResidencyAllocator::FreeLocked(const Slice& slice) {
    if (slice.blocks == 0) {
        return true;
    }
    const auto found = chunk_of_memory.find(slice.memory);
    if (found == chunk_of_memory.end()) {
        return false;
    }
    const u32 index = found->second;
    Chunk& c = chunks[index];
    u64 offset = slice.offset;
    u64 end = slice.offset + slice.blocks;
    if (end > c.blocks || end < offset) {
        return false;
    }
    auto next = c.free.lower_bound(offset);
    if (next != c.free.end() && next->first < end) {
        return false; // overlaps a free range
    }
    if (next != c.free.begin()) {
        const auto prev = std::prev(next);
        if (prev->first + prev->second > offset) {
            return false;
        }
    }

    c.used -= slice.blocks;
    used_blocks -= slice.blocks;
    if (next != c.free.end() && next->first == end) {
        end += next->second;
        const auto after = std::next(next);
        EraseFree(index, next);
        next = after;
    }
    if (next != c.free.begin()) {
        const auto prev = std::prev(next);
        if (prev->first + prev->second == offset) {
            offset = prev->first;
            EraseFree(index, prev);
        }
    }
    InsertFree(index, offset, end - offset);
    return true;
}

bool ResidencyAllocator::Free(const Slice& slice) {
    std::scoped_lock lk{mutex};
    return FreeLocked(slice);
}

u32 ResidencyAllocator::ReleaseEmptyChunks(u32 keep) {
    std::scoped_lock lk{mutex};
    u32 kept = 0;
    u32 released = 0;
    for (u32 i = 0; i < chunks.size(); ++i) {
        Chunk& c = chunks[i];
        if (!c.live || c.used != 0) {
            continue;
        }
        if (c.blocks == chunk_blocks && kept < keep) {
            ++kept;
            continue;
        }
        EraseFree(i, c.free.begin()); // an empty chunk is one free range
        chunk_of_memory.erase(c.memory);
        free_fn(c.memory);
        ++frees;
        c = Chunk{};
        ++released;
    }
    return released;
}

ResidencyAllocator::Stats ResidencyAllocator::GetStats() const {
    std::scoped_lock lk{mutex};
    Stats s{};
    for (const Chunk& c : chunks) {
        if (c.live) {
            ++s.chunks;
            s.chunk_bytes += c.blocks * block_bytes;
        }
    }
    s.used_bytes = used_blocks * block_bytes;
    s.allocations = allocations;
    s.failed = failed;
    s.frees = frees;
    s.free_ranges = free_index.size();
    s.largest_free = free_index.empty() ? 0 : std::prev(free_index.end())->blocks;
    return s;
}

} // namespace VideoCore
