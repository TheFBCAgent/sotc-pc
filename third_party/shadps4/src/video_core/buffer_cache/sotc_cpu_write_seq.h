// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (whole-draw memo, W2; docs/next/draw-memo.md and draw-memo-W2.md): a per-64 KiB
// CPU write sequence. A counter per 64 KiB block of guest memory is bumped wherever the memory
// tracker marks pages CPU-modified (a CPU-modified page is one ObtainBuffer uploads again). A
// cached buffer binding keeps a snapshot of the counters of the blocks it covers; while
// Unchanged(snapshot) holds, no page of the binding was marked CPU-modified since the snapshot.
//
// Correctness rules for the user (the L1 buffer memo):
// - Take the Snapshot *before* the SynchronizeMemory that clears the binding's CPU-modified
//   pages, and only when that sync covers the whole binding (not the stream path, whose pages
//   stay CPU-modified until FlushStreamedRanges). Then every later Set of a CPU bit in the
//   range comes with a Bump that the next Unchanged sees.
// - Bump *after* the tracker has set the CPU bits (after the RegionManager's mutex is released),
//   never before: a Bump before the Set could be read by a Snapshot whose sync then misses the
//   bit, and the memo would keep stale arena data.
//
// Storage: two levels, like MemoryTracker's top_tier. The top level is a flat array of 65536
// leaf pointers (one per 16 MiB of the 1 TiB guest space, 512 KiB allocated up front); a leaf is
// 256 contiguous u64 counters (one per 64 KiB, 2 KiB), allocated on the first Bump in it and
// never freed. One array index per block instead of SotcWriteTicks' hash lookup per block, and
// it makes reads lock-free.
//
// Threading: unlike SotcWriteTicks (GPU thread only), Bump runs on any thread. The tracker sets
// CPU bits in MemoryTracker::InvalidateRegion on the *faulting* thread (guest threads,
// Liverpool's thread, the thread that unmaps memory; Linux: the uffd thread), concurrently with
// stage B's Snapshot/Unchanged. So the counters are std::atomic<u64> (relaxed fetch_add; a lost
// update is impossible, ordering against the CPU bits comes from the region mutex and the submit
// chain, see draw-memo-W2.md) and leaves are installed with a CAS. Snapshot/Unchanged are
// plain relaxed loads and may run on any thread too, but the memo using them lives on B.
//
// Snapshot: counters only grow, so the sum of the covered counters is unchanged if and only if
// every one of them is (u64 counters never wrap: 2^64 bumps). A Snap is therefore a fixed 24
// bytes whatever the binding size: no heap, no cap. The cost is linear in the blocks covered:
// a binding of <= 64 KiB covers 1 or 2 blocks. Unchanged reads them again and compares.
// Blocks at or above 1 TiB (outside the guest space) share one "far" counter (conservative).
// Vulkan-free, header-only; unit test and microbenchmark: gpu/tests/test_cpu_write_seq.cpp.
#pragma once

#include <algorithm>
#include <atomic>
#include <memory>
#include "common/types.h"

namespace VideoCore {

class SotcCpuWriteSeq {
public:
    static constexpr u64 BlockShift = 16;      ///< 64 KiB per counter
    static constexpr u64 LeafShift = 24;       ///< 16 MiB per leaf (MemoryTracker's HIGHER_PAGE)
    static constexpr u64 SpaceShift = 40;      ///< 1 TiB guest space (MAX_CPU_PAGE_BITS)
    static constexpr u64 LeafBlocks = u64{1} << (LeafShift - BlockShift);  ///< 256
    static constexpr u64 NumLeaves = u64{1} << (SpaceShift - LeafShift);   ///< 65536
    static constexpr u64 SpaceEnd = u64{1} << SpaceShift;

    /// What Snapshot captured: the range and the sum of its counters.
    struct Snap {
        u64 addr = 0;
        u64 size = 0;
        u64 sum = 0;
    };

    SotcCpuWriteSeq() : leaves_{std::make_unique<std::atomic<Leaf*>[]>(NumLeaves)} {}
    ~SotcCpuWriteSeq() {
        for (u64 i = 0; i < NumLeaves; ++i) {
            delete leaves_[i].load(std::memory_order_relaxed);
        }
    }
    SotcCpuWriteSeq(const SotcCpuWriteSeq&) = delete;
    SotcCpuWriteSeq& operator=(const SotcCpuWriteSeq&) = delete;

    /// [addr, addr + size) was marked CPU-modified: bumps every 64 KiB block it touches. Any
    /// thread. Size 0 does nothing; a range past the end of the address space is clamped.
    void Bump(u64 addr, u64 size) {
        if (size == 0) {
            return;
        }
        const u64 last = Last(addr, size);
        for (u64 b = addr >> BlockShift; b <= last;) {
            if (b >= (SpaceEnd >> BlockShift)) {
                far_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            Leaf* leaf = GetOrCreate(b >> (LeafShift - BlockShift));
            const u64 begin = b & (LeafBlocks - 1);
            const u64 end = std::min(LeafBlocks - 1, begin + (last - b));
            for (u64 i = begin; i <= end; ++i) {
                leaf->c[i].fetch_add(1, std::memory_order_relaxed);
            }
            b += end - begin + 1;
        }
    }

    /// Captures the counters of the blocks [addr, addr + size) covers. Fixed size, no heap.
    [[nodiscard]] Snap Snapshot(u64 addr, u64 size) const {
        return Snap{addr, size, Sum(addr, size)};
    }

    /// True if no block of the snapshot's range was bumped since the snapshot.
    [[nodiscard]] bool Unchanged(const Snap& snap) const {
        return Sum(snap.addr, snap.size) == snap.sum;
    }

    /// The counter of the block holding `addr` (tests, logs).
    [[nodiscard]] u64 Counter(u64 addr) const {
        return Sum(addr & ~((u64{1} << BlockShift) - 1), 1);
    }

private:
    struct Leaf {
        std::atomic<u64> c[LeafBlocks];
    };

    /// The last block of [addr, addr + size), size > 0 (clamped at 2^64 - 1).
    static u64 Last(u64 addr, u64 size) {
        const u64 end = addr + size - 1;
        return (end < addr ? ~u64{0} : end) >> BlockShift;
    }

    Leaf* GetOrCreate(u64 index) {
        Leaf* leaf = leaves_[index].load(std::memory_order_acquire);
        if (leaf) [[likely]] {
            return leaf;
        }
        Leaf* fresh = new Leaf;
        for (auto& c : fresh->c) {
            c.store(0, std::memory_order_relaxed);
        }
        if (leaves_[index].compare_exchange_strong(leaf, fresh, std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
            return fresh;
        }
        delete fresh;
        return leaf; // another thread installed it first
    }

    u64 Sum(u64 addr, u64 size) const {
        if (size == 0) {
            return 0;
        }
        const u64 last = Last(addr, size);
        u64 sum = 0;
        for (u64 b = addr >> BlockShift; b <= last;) {
            if (b >= (SpaceEnd >> BlockShift)) {
                return sum + far_.load(std::memory_order_relaxed);
            }
            const u64 begin = b & (LeafBlocks - 1);
            const u64 end = std::min(LeafBlocks - 1, begin + (last - b));
            if (const Leaf* leaf =
                    leaves_[b >> (LeafShift - BlockShift)].load(std::memory_order_acquire)) {
                for (u64 i = begin; i <= end; ++i) {
                    sum += leaf->c[i].load(std::memory_order_relaxed);
                }
            }
            b += end - begin + 1;
        }
        return sum;
    }

    std::unique_ptr<std::atomic<Leaf*>[]> leaves_;
    std::atomic<u64> far_{0};
};

} // namespace VideoCore
