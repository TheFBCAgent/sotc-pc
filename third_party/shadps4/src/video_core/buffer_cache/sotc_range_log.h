// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (session 32): the ranges ObtainBuffer streamed since the last
// FlushStreamedRanges, as an append-only log instead of a boost::icl interval_set
// (RangeSet). The set's insert (a tree search plus a pool allocation under a mutex) ran for
// every streamed read-only buffer, ~7.7% of the GPU thread in the temple (perf). Add appends
// (or widens the last entry when the new range touches it); the log sorts and merges itself
// when it doubles, so it stays bounded by the distinct ranges like the set did, and ForEach
// visits the same merged ranges the set held, in address order. GPU thread only.
// Test: gpu/tests/test_range_log.cpp.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace VideoCore {

class SotcRangeLog {
public:
    static constexpr std::size_t MinCompact = 4096;

    void Add(std::uint64_t addr, std::uint64_t size) {
        const std::uint64_t end = addr + size;
        if (!ranges.empty()) {
            auto& last = ranges.back();
            if (addr <= last.second && end >= last.first) { // overlaps or touches the last
                last.first = std::min(last.first, addr);
                last.second = std::max(last.second, end);
                return;
            }
        }
        ranges.emplace_back(addr, end);
        if (ranges.size() >= compact_at) {
            Compact();
            compact_at = std::max(MinCompact, ranges.size() * 2);
        }
    }

    /// func(start, end) for each merged range, in address order.
    template <typename Func>
    void ForEach(Func&& func) {
        Compact();
        for (const auto& [start, end] : ranges) {
            func(start, end);
        }
    }

    void Clear() {
        ranges.clear();
        compact_at = MinCompact;
    }

    std::size_t Entries() const {
        return ranges.size();
    }

private:
    void Compact() {
        if (ranges.size() < 2) {
            return;
        }
        std::sort(ranges.begin(), ranges.end());
        std::size_t out = 0;
        for (std::size_t i = 1; i < ranges.size(); ++i) {
            if (ranges[i].first <= ranges[out].second) {
                ranges[out].second = std::max(ranges[out].second, ranges[i].second);
            } else {
                ranges[++out] = ranges[i];
            }
        }
        ranges.resize(out + 1);
    }

    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges; ///< [start, end)
    std::size_t compact_at = MinCompact;
};

} // namespace VideoCore
