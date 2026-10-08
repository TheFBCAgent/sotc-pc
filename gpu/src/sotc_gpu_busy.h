// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (session 12): GPU busy time from timestamp intervals (SOTC_GPU_STATS). Vulkan-free;
// the query pool lives in gpu_timer.cpp. Unit test: gpu/tests/test_gpu_busy.cpp.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace SotcGpuBusy {

/// One submitted command buffer's [begin, end) on the GPU, in timestamp ticks.
struct Interval {
    uint64_t begin;
    uint64_t end;
};

/// Masks a raw timestamp to the queue family's timestampValidBits (1..64).
inline uint64_t MaskTicks(uint64_t raw, uint32_t valid_bits) {
    return valid_bits >= 64 ? raw : raw & ((uint64_t{1} << valid_bits) - 1);
}

/// Length of the union of `intervals` (overlaps counted once), counting only time after
/// `floor`, the end of what an earlier call already counted; raises `floor` to the latest end.
/// Intervals collected late can overlap the previous call's, so carrying `floor` keeps the
/// per-second sums from counting that overlap twice. Empty or inverted intervals (a wrapped
/// counter) are skipped. Sorts `intervals`.
inline uint64_t UnionTicks(std::vector<Interval>& intervals, uint64_t& floor) {
    std::sort(intervals.begin(), intervals.end(),
              [](const Interval& a, const Interval& b) { return a.begin < b.begin; });
    uint64_t total = 0;
    uint64_t covered = floor; // everything before this is counted
    for (const Interval& iv : intervals) {
        if (iv.end <= iv.begin) {
            continue;
        }
        const uint64_t begin = std::max(iv.begin, covered);
        if (iv.end > begin) {
            total += iv.end - begin;
            covered = iv.end;
        }
    }
    floor = std::max(floor, covered);
    return total;
}

/// Ticks to milliseconds with VkPhysicalDeviceLimits::timestampPeriod (ns per tick).
inline double TicksToMs(uint64_t ticks, float period_ns) {
    return double(ticks) * double(period_ns) / 1e6;
}

} // namespace SotcGpuBusy
