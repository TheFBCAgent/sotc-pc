// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (whole-draw memo, W3): guest address -> (ring id, offset) for the memory a game's
// per-frame allocators hand out. A binding into such memory gets a new base address every frame,
// so a memo key holding the address never matches; with the map, the key holds the ring id and the
// binding resolves to one resident buffer per ring plus an offset (docs/next/draw-memo-W3.md).
//
// Rings are registered at run time: from the memo probe's address classification (bases that
// move every frame inside a few pages), or from any other discovery. Nothing here is specific to a
// game. A ring is a half-open range [base, base + size) with a caller-chosen id; ranges never
// overlap. Registering a range that touches a ring with the same id merges the two, so learning one
// page at a time builds a window. Every change bumps Generation(): a memo keyed by (ring id,
// offset) stores it and treats a different value as a miss (a merge to the left moves offsets).
//
// Lookup is a binary search over at most MaxRings sorted entries (no allocation, no hashing):
// called per binding on stage B. Stage B only, like SotcWriteTicks. Vulkan-free; unit test:
// gpu/tests/test_ring_map.cpp.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include "common/types.h"

namespace VideoCore {

class SotcRingMap {
public:
    static constexpr std::size_t MaxRings = 64;

    struct Ring {
        u64 base;
        u64 size;
        u32 id;

        [[nodiscard]] u64 End() const {
            return base + size;
        }
    };

    struct Hit {
        u32 id;
        u64 offset; ///< from the ring's base
        u64 base;   ///< the ring's base
        u64 size;   ///< the ring's size
    };

    enum class Result {
        Added,   ///< a new ring
        Merged,  ///< joined a touching or covering ring with the same id
        Present, ///< already inside a ring with the same id (nothing changed)
        Overlap, ///< overlaps a ring with another id (rejected; touching one is fine)
        Full,    ///< MaxRings rings already (rejected)
        Empty,   ///< size 0, or base + size wraps (rejected)
    };

    /// Registers [base, base + size) as ring `id`. A range touching or overlapping only rings
    /// with the same id merges with them (one entry, the union). Overlapping a ring with another
    /// id is rejected.
    Result Register(u64 base, u64 size, u32 id) {
        if (size == 0 || base + size < base) {
            return Result::Empty;
        }
        u64 lo = base;
        u64 hi = base + size;
        // Entries that touch or overlap [lo, hi]: a contiguous run in sorted order.
        std::size_t first = LowerTouching(lo);
        std::size_t last = first;
        while (last < count_ && rings_[last].base <= hi) {
            if (rings_[last].id != id) {
                // Touching (end == lo or base == hi) a ring with another id is fine; overlap isn't.
                if (rings_[last].End() > lo && rings_[last].base < hi) {
                    return Result::Overlap;
                }
                if (rings_[last].End() == lo) {
                    ++first; // only touches on the left: not part of the merge
                    ++last;
                    continue;
                }
                break; // touches on the right: stops the run
            }
            ++last;
        }
        if (first == last) {
            if (count_ == MaxRings) {
                return Result::Full;
            }
            std::move_backward(rings_.begin() + first, rings_.begin() + count_,
                               rings_.begin() + count_ + 1);
            rings_[first] = Ring{lo, size, id};
            ++count_;
            ++generation_;
            return Result::Added;
        }
        lo = std::min(lo, rings_[first].base);
        hi = std::max(hi, rings_[last - 1].End());
        if (lo == rings_[first].base && hi == rings_[first].End() && last - first == 1) {
            return Result::Present;
        }
        rings_[first] = Ring{lo, hi - lo, id};
        std::move(rings_.begin() + last, rings_.begin() + count_, rings_.begin() + first + 1);
        count_ -= last - first - 1;
        ++generation_;
        return Result::Merged;
    }

    /// Removes every ring with this id; returns how many.
    std::size_t Unregister(u32 id) {
        const auto end = std::remove_if(rings_.begin(), rings_.begin() + count_,
                                        [id](const Ring& r) { return r.id == id; });
        const std::size_t removed = static_cast<std::size_t>(rings_.begin() + count_ - end);
        count_ -= removed;
        if (removed != 0) {
            ++generation_;
        }
        return removed;
    }

    void Clear() {
        if (count_ != 0) {
            count_ = 0;
            ++generation_;
        }
    }

    /// The ring holding byte `addr`, if any.
    [[nodiscard]] std::optional<Hit> Find(u64 addr) const {
        const Ring* r = Containing(addr);
        if (!r) {
            return std::nullopt;
        }
        return Hit{r->id, addr - r->base, r->base, r->size};
    }

    /// The ring holding all of [addr, addr + size), size > 0; none if the range leaves it.
    [[nodiscard]] std::optional<Hit> FindRange(u64 addr, u64 size) const {
        const Ring* r = Containing(addr);
        if (!r || size == 0 || size > r->End() - addr) {
            return std::nullopt;
        }
        return Hit{r->id, addr - r->base, r->base, r->size};
    }

    /// Bumped by every change of the set (add, merge, unregister, clear).
    [[nodiscard]] u64 Generation() const {
        return generation_;
    }
    [[nodiscard]] std::size_t Count() const {
        return count_;
    }
    [[nodiscard]] bool Empty() const {
        return count_ == 0;
    }
    /// The rings in address order (for logs).
    [[nodiscard]] const Ring* begin() const {
        return rings_.data();
    }
    [[nodiscard]] const Ring* end() const {
        return rings_.data() + count_;
    }

private:
    /// The ring containing addr, or null.
    [[nodiscard]] const Ring* Containing(u64 addr) const {
        // First ring whose base is > addr; the one before it is the only candidate.
        const Ring* it = std::upper_bound(rings_.data(), rings_.data() + count_, addr,
                                          [](u64 a, const Ring& r) { return a < r.base; });
        if (it == rings_.data()) {
            return nullptr;
        }
        --it;
        return addr - it->base < it->size ? it : nullptr;
    }

    /// Index of the first ring whose end is >= addr (it may touch or contain addr).
    [[nodiscard]] std::size_t LowerTouching(u64 addr) const {
        const Ring* it = std::lower_bound(rings_.data(), rings_.data() + count_, addr,
                                          [](const Ring& r, u64 a) { return r.End() < a; });
        return static_cast<std::size_t>(it - rings_.data());
    }

    std::array<Ring, MaxRings> rings_{};
    std::size_t count_{};
    u64 generation_{};
};

} // namespace VideoCore
