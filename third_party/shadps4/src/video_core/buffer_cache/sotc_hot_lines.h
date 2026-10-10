// SPDX-License-Identifier: GPL-2.0-or-later
// shadow-pc (session 33): the hot lines behind the hot flush (session 10). A hot line is a
// 256-byte line a fault download needed while its GPU data was still in the batch being
// recorded; a later written binding over one makes the dispatch submit its batch right away.
// The list held at most 256 lines and never forgot one, so the gameplay load filled it with
// lines gameplay never reads and no gameplay reader became hot again (every in-batch drain
// "with the hot list full"). With aging on (SOTC_HOT_AGING=1, opt-in) each line keeps the flip
// of its last reader hit (any download of the line refreshes it): lines not hit for
// MaxIdleFlips are dropped at the flip, and an insert into a full list replaces the stalest
// line. Off (default): the session 10 list, full is full. Aging cost ~4% fps (session 33,
// docs/NEXT.md): the in-batch drains come from tiny writes elsewhere in the reader's 4 KiB, so
// the extra flushes only moved the wait. The overlap scan reads only the line array, as cheap
// as before. GPU thread only. Vulkan-free; unit test: gpu/tests/test_hot_lines.cpp.
#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>
#include "common/types.h"

namespace VideoCore {

class SotcHotLines {
public:
    static constexpr size_t Capacity = 256;
    static constexpr u64 MaxIdleFlips = 240;

    void SetAging(bool on) noexcept {
        aging = on;
    }
    [[nodiscard]] bool Aging() const noexcept {
        return aging;
    }

    [[nodiscard]] bool Empty() const noexcept {
        return lines.empty();
    }
    [[nodiscard]] size_t Size() const noexcept {
        return lines.size();
    }
    [[nodiscard]] bool Full() const noexcept {
        return lines.size() >= Capacity;
    }

    /// A download read this line: refresh it if it is hot. Returns whether it was.
    bool Touch(u64 line) noexcept {
        const auto it = std::ranges::find(lines, line);
        if (it == lines.end()) {
            return false;
        }
        last_hit[static_cast<size_t>(it - lines.begin())] = flip;
        return true;
    }

    /// Makes a line hot (refreshes it if it already is). Returns true when the list was full:
    /// with aging the stalest line was replaced, without it the line was not added.
    bool Insert(u64 line) {
        if (Touch(line)) {
            return false;
        }
        if (!Full()) {
            lines.push_back(line);
            last_hit.push_back(flip);
            return false;
        }
        if (!aging) {
            return true;
        }
        const size_t victim =
            static_cast<size_t>(std::ranges::min_element(last_hit) - last_hit.begin());
        lines[victim] = line;
        last_hit[victim] = flip;
        return true;
    }

    /// Whether any hot line lies in [first, last] (line numbers, inclusive).
    [[nodiscard]] bool Overlaps(u64 first, u64 last) const noexcept {
        for (const u64 l : lines) {
            if (l >= first && l <= last) {
                return true;
            }
        }
        return false;
    }

    /// Once per flip, with aging: advance the clock and drop lines idle for more than
    /// MaxIdleFlips.
    /// Returns how many were dropped.
    size_t OnFlip() {
        if (!aging) {
            return 0;
        }
        ++flip;
        size_t kept = 0;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (flip - last_hit[i] <= MaxIdleFlips) {
                lines[kept] = lines[i];
                last_hit[kept] = last_hit[i];
                ++kept;
            }
        }
        const size_t dropped = lines.size() - kept;
        lines.resize(kept);
        last_hit.resize(kept);
        return dropped;
    }

private:
    std::vector<u64> lines;    ///< scanned by Overlaps for every written binding
    std::vector<u64> last_hit; ///< flip of the last reader hit, parallel to lines
    u64 flip = 0;
    bool aging = false;
};

} // namespace VideoCore
