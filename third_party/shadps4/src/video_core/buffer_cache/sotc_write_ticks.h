// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (session 10): the scheduler tick of the newest GPU write per 64 KiB of guest memory
// (per 1 MiB for bindings over 16 MiB). BufferCache::ObtainBuffer notes every written binding;
// a download then knows which submission produced the data, instead of waiting for all queued
// work. After bbport's GpuWriteTicks (deadinside28/bloodborne_pc, buffer_cache.cpp).
// GPU thread only (ObtainBuffer and DownloadMemory both run there). Vulkan-free; unit test:
// gpu/tests/test_write_ticks.cpp.
#pragma once

#include <algorithm>
#include <unordered_map>
#include "common/types.h"

namespace VideoCore {

class SotcWriteTicks {
public:
    static constexpr u64 FineShift = 16;
    static constexpr u64 CoarseShift = 20;
    static constexpr u64 FineLimit = u64{16} << 20;

    /// A binding the GPU may write, recorded at `tick` (ticks only grow).
    void Note(u64 addr, u64 size, u64 tick) {
        if (size == 0) {
            return;
        }
        const bool coarse = size > FineLimit;
        auto& pages = coarse ? coarse_ : fine_;
        const u64 shift = coarse ? CoarseShift : FineShift;
        for (u64 p = addr >> shift, last = (addr + size - 1) >> shift; p <= last; ++p) {
            u64& t = pages[p];
            t = std::max(t, tick);
        }
    }

    /// The newest tick that may have written any byte of [addr, addr + size); 0 if none.
    [[nodiscard]] u64 Newest(u64 addr, u64 size) const {
        if (size == 0) {
            return 0;
        }
        return std::max(Scan(fine_, FineShift, addr, size), Scan(coarse_, CoarseShift, addr, size));
    }

private:
    static u64 Scan(const std::unordered_map<u64, u64>& pages, u64 shift, u64 addr, u64 size) {
        if (pages.empty()) {
            return 0;
        }
        u64 newest = 0;
        for (u64 p = addr >> shift, last = (addr + size - 1) >> shift; p <= last; ++p) {
            if (const auto it = pages.find(p); it != pages.end()) {
                newest = std::max(newest, it->second);
            }
        }
        return newest;
    }

    std::unordered_map<u64, u64> fine_;
    std::unordered_map<u64, u64> coarse_;
};

} // namespace VideoCore
