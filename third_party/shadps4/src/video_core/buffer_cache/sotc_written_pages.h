// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (session 34): 4 KiB guest pages that may hold GPU-written bytes. Every
// gpu_modified_ranges.Add marks its pages here, and every Subtract unmarks the pages it covers
// whole (a partly covered page stays marked: a stale "maybe" only costs a search). A page that
// isn't marked holds no byte of gpu_modified_ranges, so BufferCache::IsRangeGpuWritten answers
// "no" without the boost::icl search; a marked page still takes the byte-precise search. In the
// temple ~1M walker table checks/s, ~0.1% GPU-written: ~48% sit on pages no binding ever
// wrote, most of the rest on pages downloaded since. GPU thread only.
// Test: gpu/tests/test_written_pages.cpp.
#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>

namespace VideoCore {

class SotcWrittenPages {
public:
    static constexpr std::uint32_t PageShift = 12;
    static constexpr std::uint32_t ChunkShift = 30; ///< one bitmap per 1 GiB
    static constexpr std::uint32_t AddressBits = 40; ///< tracked: [0, 1 TiB); above is "maybe"
    static constexpr std::uint64_t WordsPerChunk = (1ull << (ChunkShift - PageShift)) / 64;
    static constexpr std::uint64_t Chunks = 1ull << (AddressBits - ChunkShift);

    /// Marks the pages [addr, addr + size) lies in (nothing for size 0).
    void Mark(std::uint64_t addr, std::uint64_t size) {
        ForEachWord(addr, size, [&](std::uint64_t chunk, std::uint64_t word, std::uint64_t mask) {
            auto& bits = chunks[chunk];
            if (!bits) {
                bits = std::make_unique<std::uint64_t[]>(WordsPerChunk); // zeroed
            }
            bits[word] |= mask;
            return true;
        });
    }

    /// Unmarks the pages [addr, addr + size) covers whole: none of their bytes is left.
    void Unmark(std::uint64_t addr, std::uint64_t size) {
        const std::uint64_t page = 1ull << PageShift;
        const std::uint64_t first = (addr + page - 1) & ~(page - 1);
        const std::uint64_t end = addr + size < addr ? ~0ull : addr + size;
        const std::uint64_t last = end & ~(page - 1);
        if (first < addr || last <= first) {
            return; // no whole page (or wrapped)
        }
        ForEachWord(first, last - first, [&](std::uint64_t chunk, std::uint64_t word, std::uint64_t mask) {
            if (auto& bits = chunks[chunk]) {
                bits[word] &= ~mask;
            }
            return true;
        });
    }

    /// False only if no page of [addr, addr + size) is marked (and size != 0): then no byte of
    /// it is GPU-written. True means "maybe": ask the precise ranges.
    [[nodiscard]] bool Maybe(std::uint64_t addr, std::uint64_t size) const {
        const std::uint64_t last = addr + size - 1;
        if (size == 0 || last < addr || (last >> AddressBits) != 0) {
            return true; // let the precise search decide
        }
        bool maybe = false;
        ForEachWord(addr, size, [&](std::uint64_t chunk, std::uint64_t word, std::uint64_t mask) {
            const auto& bits = chunks[chunk];
            maybe = bits && (bits[word] & mask) != 0;
            return !maybe;
        });
        return maybe;
    }

private:
    static constexpr std::uint64_t PagesPerChunk = 1ull << (ChunkShift - PageShift);

    /// fn(chunk, word, mask) for each 64-page word [addr, addr + size) touches, with the bits
    /// of its pages, while fn returns true. Pages at or above 1 TiB are left out.
    template <typename Fn>
    static void ForEachWord(std::uint64_t addr, std::uint64_t size, Fn&& fn) {
        if (size == 0) {
            return;
        }
        const std::uint64_t last_byte = addr + size - 1 < addr ? ~0ull : addr + size - 1;
        const std::uint64_t first = addr >> PageShift;
        const std::uint64_t last = std::min(last_byte >> PageShift, (Chunks * PagesPerChunk) - 1);
        for (std::uint64_t p = first; p <= last && p >= first;) {
            const std::uint64_t word_first = p & ~63ull;
            const std::uint32_t lo = static_cast<std::uint32_t>(p - word_first);
            const std::uint32_t hi = static_cast<std::uint32_t>(std::min(last, word_first + 63) -
                                                                word_first);
            const std::uint64_t mask = (hi == 63 ? ~0ull : (2ull << hi) - 1) & (~0ull << lo);
            if (!fn(p / PagesPerChunk, (p % PagesPerChunk) / 64, mask)) {
                return;
            }
            p = word_first + 64;
        }
    }

    std::unique_ptr<std::uint64_t[]> chunks[Chunks]; ///< 32 KiB each, allocated on first mark
};

} // namespace VideoCore
