// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (session 22, SOTC_CPU_COPY): 4 KiB guest pages the host has already written into
// guest memory: by a download (BufferCache::DownloadMemory) or by a CPU copy. Their backing is
// resident anyway, so writing them again costs no RAM. Rasterizer::SotcCpuCopy copies on the
// CPU only into such pages: copying into GPU-only buffers no one reads back made their backing
// resident (session 21: 6.8 -> 8.0 GB). GPU thread only. Test: gpu/tests/test_resident_pages.cpp.
#pragma once

#include <bitset>
#include <cstdint>
#include <unordered_map>

namespace VideoCore {

class SotcResidentPages {
public:
    static constexpr std::uint32_t PageShift = 12;
    static constexpr std::uint32_t ChunkShift = 20; ///< one bitset per 1 MiB
    static constexpr std::uint32_t PagesPerChunk = 1u << (ChunkShift - PageShift);

    /// Marks the pages [addr, addr + size) lies in. A partly written page counts: its backing
    /// page is resident as a whole.
    void Mark(std::uint64_t addr, std::uint64_t size) {
        ForEachChunk(addr, size, [&](std::uint64_t chunk, std::uint32_t first, std::uint32_t last) {
            auto& bits = chunks[chunk];
            for (std::uint32_t p = first; p <= last; ++p) {
                bits.set(p);
            }
            return true;
        });
    }

    /// Pages of [addr, addr + size) marked, and pages in total.
    struct Coverage {
        std::uint64_t marked = 0, total = 0;
        bool All() const {
            return total != 0 && marked == total;
        }
    };
    Coverage Covered(std::uint64_t addr, std::uint64_t size) const {
        Coverage c;
        ForEachChunk(addr, size, [&](std::uint64_t chunk, std::uint32_t first, std::uint32_t last) {
            c.total += last - first + 1;
            const auto it = chunks.find(chunk);
            if (it == chunks.end()) {
                return true;
            }
            for (std::uint32_t p = first; p <= last; ++p) {
                c.marked += it->second.test(p);
            }
            return true;
        });
        return c;
    }

    /// True if every page of [addr, addr + size) is marked (false for size 0).
    bool All(std::uint64_t addr, std::uint64_t size) const {
        bool all = size != 0;
        ForEachChunk(addr, size, [&](std::uint64_t chunk, std::uint32_t first, std::uint32_t last) {
            const auto it = chunks.find(chunk);
            if (it == chunks.end()) {
                return all = false;
            }
            if (first == 0 && last == PagesPerChunk - 1) {
                return all = it->second.all();
            }
            for (std::uint32_t p = first; p <= last; ++p) {
                if (!it->second.test(p)) {
                    return all = false;
                }
            }
            return true;
        });
        return all;
    }

    /// fn(start, size, resident) for each maximal run of marked or unmarked pages, clipped to
    /// [addr, addr + size), in address order.
    template <typename Fn>
    void ForEachRun(std::uint64_t addr, std::uint64_t size, Fn&& fn) const {
        if (size == 0) {
            return;
        }
        const std::uint64_t end = addr + size;
        std::uint64_t run_start = addr;
        bool run_resident = IsMarked(addr >> PageShift);
        for (std::uint64_t p = (addr >> PageShift) + 1; (p << PageShift) < end; ++p) {
            const bool r = IsMarked(p);
            if (r != run_resident) {
                fn(run_start, (p << PageShift) - run_start, run_resident);
                run_start = p << PageShift;
                run_resident = r;
            }
        }
        fn(run_start, end - run_start, run_resident);
    }

    std::uint64_t Pages() const {
        std::uint64_t n = 0;
        for (const auto& [chunk, bits] : chunks) {
            n += bits.count();
        }
        return n;
    }

private:
    bool IsMarked(std::uint64_t page) const {
        const auto it = chunks.find(page >> (ChunkShift - PageShift));
        return it != chunks.end() && it->second.test(page & (PagesPerChunk - 1));
    }

    /// fn(chunk, first page, last page) per 1 MiB chunk; stops when fn returns false.
    template <typename Fn>
    static void ForEachChunk(std::uint64_t addr, std::uint64_t size, Fn&& fn) {
        if (size == 0) {
            return;
        }
        const std::uint64_t first = addr >> PageShift;
        const std::uint64_t last = (addr + size - 1) >> PageShift;
        for (std::uint64_t p = first; p <= last;) {
            const std::uint64_t chunk = p >> (ChunkShift - PageShift);
            const std::uint64_t chunk_last = ((chunk + 1) << (ChunkShift - PageShift)) - 1;
            const std::uint64_t end = chunk_last < last ? chunk_last : last;
            if (!fn(chunk, static_cast<std::uint32_t>(p & (PagesPerChunk - 1)),
                    static_cast<std::uint32_t>(end & (PagesPerChunk - 1)))) {
                return;
            }
            p = end + 1;
        }
    }

    std::unordered_map<std::uint64_t, std::bitset<PagesPerChunk>> chunks;
};

} // namespace VideoCore
