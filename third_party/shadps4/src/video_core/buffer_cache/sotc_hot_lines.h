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
// as before. Session 52: each line may request at most `budget` flushes per flip and all lines
// together 4x that (TakeFlush; SOTC_HOT_BUDGET, default 8, 0 = no limit). Session 51 saw random storms of ~3,100-25,000
// hot flushes/s (normal: ~200-340/s, ~7-12 per flip in all) when gameplay compute kept writing
// a line the load had made hot. GPU thread only. Vulkan-free; unit test:
// gpu/tests/test_hot_lines.cpp.
#pragma once

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>
#include "common/types.h"

namespace VideoCore {

class SotcHotLines {
public:
    static constexpr size_t Capacity = 256;
    static constexpr u64 MaxIdleFlips = 240;
    static constexpr u32 TotalBudgetFactor = 4; ///< all lines' flushes per flip: 4x one line's

    void SetAging(bool on) noexcept {
        aging = on;
    }
    [[nodiscard]] bool Aging() const noexcept {
        return aging;
    }

    /// Flushes one line may request per flip; 0: no limit.
    void SetBudget(u32 per_flip) noexcept {
        budget = per_flip;
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
            used.push_back(0);
            return false;
        }
        if (!aging) {
            return true;
        }
        const size_t victim =
            static_cast<size_t>(std::ranges::min_element(last_hit) - last_hit.begin());
        lines[victim] = line;
        last_hit[victim] = flip;
        used[victim] = 0;
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

    /// A written binding over [first, last] (line numbers, inclusive): whether it should flush.
    /// Charges the first overlapping line with budget left; a binding over hot lines that all
    /// spent their budget this flip, or after the list spent 4x the budget, counts as denied
    /// and does not flush.
    bool TakeFlush(u64 first, u64 last) noexcept {
        bool hit = false;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (lines[i] < first || lines[i] > last) {
                continue;
            }
            if (budget == 0) {
                return true;
            }
            if (used[i] < budget && used_total < TotalBudgetFactor * budget) {
                ++used[i];
                ++used_total;
                return true;
            }
            hit = true;
        }
        denied += hit;
        denied_this_flip |= hit;
        return false;
    }

    /// Bindings TakeFlush denied since the last call.
    u64 TakeDenied() noexcept {
        return std::exchange(denied, 0);
    }

    /// Once per flip: refill the flush budgets; with aging, also advance the clock and drop
    /// lines idle for more than MaxIdleFlips. Returns whether the lines' flush answers may have
    /// changed (a line dropped, or a budget refilled after a denial).
    bool OnFlip() {
        std::ranges::fill(used, 0u);
        used_total = 0;
        const bool refilled = std::exchange(denied_this_flip, false);
        if (!aging) {
            return refilled;
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
        used.resize(kept);
        return refilled || dropped != 0;
    }

private:
    std::vector<u64> lines;    ///< scanned by Overlaps for every written binding
    std::vector<u64> last_hit; ///< flip of the last reader hit, parallel to lines
    std::vector<u32> used;     ///< flushes requested this flip, parallel to lines
    u32 used_total = 0;        ///< all lines' flushes this flip
    u64 flip = 0;
    u64 denied = 0;
    u32 budget = 8;
    bool aging = false;
    bool denied_this_flip = false;
};

} // namespace VideoCore
