// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (session 13): readback prefetch bookkeeping. Pages (64 KiB, as SotcWriteTicks) that
// faults keep downloading form a small hot set. At the end of each batch that wrote a hot page,
// BufferCache copies the page's GPU-written pieces out of the arena into a ring in a persistent
// host-visible buffer and records them here. A later download whose bytes an entry covers, with
// nothing newer written over them, is served from the ring with a memcpy once the batch is done.
// After bbport's deferred readback (deadinside28/bloodborne_pc).
// GPU thread only. Vulkan-free; unit test: gpu/tests/test_prefetch.cpp.
#pragma once

#include <algorithm>
#include <unordered_map>
#include <vector>
#include "common/types.h"

namespace VideoCore {

class SotcPrefetch {
public:
    static constexpr u64 PageShift = 16;
    static constexpr u64 PageSize = u64{1} << PageShift;
    static constexpr size_t MaxHotPages = 256;
    static constexpr u64 HotTicks = 64; ///< a page leaves the hot set after this many submits
    static constexpr u64 Align = 64;

    struct Piece {
        u64 addr;
        u64 size;
        u64 ring_pos; ///< absolute ring position (offset = ring_pos % ring size)
    };

    /// Where a request's bytes are in the ring.
    struct Segment {
        u64 addr;
        u64 size;
        u64 ring_pos;
        u64 tick; ///< the batch whose end copied it
    };

    enum class Miss { None, NoEntry, Partial, Overwritten };

    explicit SotcPrefetch(u64 ring_size_) : ring_size{ring_size_} {}

    // ------------------------------------------------------------------ hot set

    /// A download (of data a submitted batch wrote) read `page` at scheduler tick `tick`.
    /// Returns false when the page wasn't hot yet.
    bool NoteDownload(u64 page, u64 tick) {
        if (const auto it = std::ranges::find(hot, page, &Hot::page); it != hot.end()) {
            it->last_tick = tick;
            return true;
        }
        if (hot.size() >= MaxHotPages) {
            // Replace the page read longest ago.
            const auto oldest = std::ranges::min_element(hot, {}, &Hot::last_tick);
            entries.erase(oldest->page);
            *oldest = {page, tick};
            return false;
        }
        hot.push_back({page, tick});
        return false;
    }

    /// Drops pages no download read in the last HotTicks submits (and their entries).
    void Age(u64 tick) {
        std::erase_if(hot, [&](const Hot& h) {
            if (h.last_tick + HotTicks >= tick) {
                return false;
            }
            entries.erase(h.page);
            return true;
        });
    }

    template <typename Func>
    void ForEachHotPage(Func&& func) const {
        for (const Hot& h : hot) {
            func(h.page);
        }
    }

    [[nodiscard]] size_t HotCount() const {
        return hot.size();
    }

    // ------------------------------------------------------------------ ring

    /// Ring space for `size` bytes; never straddles the end of the ring. Older allocations it
    /// reuses become invalid (IsLive). Returns the absolute position.
    [[nodiscard]] u64 Allocate(u64 size) {
        size = (size + Align - 1) & ~(Align - 1);
        if (head % ring_size + size > ring_size) {
            head += ring_size - head % ring_size;
        }
        const u64 pos = head;
        head += size;
        return pos;
    }

    /// Bytes at `pos` are still the ones written there (not reused by a later Allocate).
    [[nodiscard]] bool IsLive(u64 pos) const {
        return pos + ring_size >= head;
    }

    [[nodiscard]] u64 RingOffset(u64 pos) const {
        return pos % ring_size;
    }

    [[nodiscard]] u64 RingSize() const {
        return ring_size;
    }

    // ------------------------------------------------------------------ entries

    /// The copies recorded for `page` at the end of batch `tick` (each inside the page).
    void Record(u64 page, u64 tick, std::vector<Piece> pieces) {
        entries[page] = Entry{tick, std::move(pieces)};
    }

    /// Splits [addr, addr + size) at pages and finds each part in an entry's pieces. Appends
    /// the parts to `out` and returns Miss::None when every byte is covered by a live piece.
    /// The caller checks the ticks (nothing newer wrote the bytes, the batch is done).
    Miss Lookup(u64 addr, u64 size, std::vector<Segment>& out) const {
        const u64 end = addr + size;
        while (addr < end) {
            const u64 page = addr >> PageShift;
            const u64 seg_end = std::min(end, (page + 1) << PageShift);
            const auto it = entries.find(page);
            if (it == entries.end()) {
                return Miss::NoEntry;
            }
            const Entry& entry = it->second;
            const auto piece = std::ranges::find_if(entry.pieces, [&](const Piece& p) {
                return p.addr <= addr && seg_end <= p.addr + p.size;
            });
            if (piece == entry.pieces.end()) {
                return Miss::Partial;
            }
            if (!IsLive(piece->ring_pos)) {
                return Miss::Overwritten;
            }
            out.push_back({addr, seg_end - addr, piece->ring_pos + (addr - piece->addr), entry.tick});
            addr = seg_end;
        }
        return Miss::None;
    }

private:
    struct Hot {
        u64 page;
        u64 last_tick;
    };
    struct Entry {
        u64 tick;
        std::vector<Piece> pieces;
    };

    u64 ring_size;
    u64 head = 0;
    std::vector<Hot> hot;
    std::unordered_map<u64, Entry> entries;
};

} // namespace VideoCore
