// SPDX-License-Identifier: GPL-2.0-or-later
// Session 33: VideoCore::SotcHotLines, the hot flush's 256-byte lines that age out by flip and
// replace their stalest line when full (SOTC_HOT_AGING); without aging, the session 10 list.
// Session 52: the per-line flush budget per flip (SOTC_HOT_BUDGET).
#include <cstdio>

#include "video_core/buffer_cache/sotc_hot_lines.h"

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

using VideoCore::SotcHotLines;

int main() {
    {
        SotcHotLines hot;
        CHECK(hot.Empty());
        CHECK(!hot.Touch(5));
        CHECK(!hot.Overlaps(0, ~0ull));
        CHECK(!hot.Insert(5));
        CHECK(hot.Touch(5));
        CHECK(!hot.Insert(5)); // already hot: no duplicate
        CHECK(hot.Size() == 1);
        CHECK(hot.Overlaps(5, 5));
        CHECK(hot.Overlaps(0, 5));
        CHECK(hot.Overlaps(5, 9));
        CHECK(!hot.Overlaps(6, 9));
        CHECK(!hot.Overlaps(0, 4));
    }
    {
        // The session 33 bug: a load fills the list, gameplay lines must still get in.
        SotcHotLines hot;
        hot.SetAging(true);
        for (u64 i = 0; i < SotcHotLines::Capacity; ++i) {
            CHECK(!hot.Insert(1000 + i));
        }
        CHECK(hot.Full());
        hot.OnFlip();
        hot.Touch(1000 + 7); // the only load line a reader still hits
        CHECK(hot.Insert(1)); // full: replaces the stalest, not line 1007
        CHECK(hot.Size() == SotcHotLines::Capacity);
        CHECK(hot.Touch(1));
        CHECK(hot.Touch(1000 + 7));
        CHECK(!hot.Touch(1000)); // the first stalest (lowest index) went
    }
    {
        // Lines idle for more than MaxIdleFlips are dropped at the flip; a hit keeps a line.
        SotcHotLines hot;
        hot.SetAging(true);
        hot.Insert(10);
        hot.Insert(20);
        for (u64 f = 0; f < SotcHotLines::MaxIdleFlips; ++f) {
            CHECK(!hot.OnFlip());
            hot.Touch(20);
        }
        CHECK(hot.Size() == 2); // exactly MaxIdleFlips idle: still there
        CHECK(hot.OnFlip());
        CHECK(!hot.Touch(10));
        CHECK(hot.Touch(20));
        CHECK(hot.Overlaps(20, 20) && !hot.Overlaps(10, 10));
        for (u64 f = 0; f <= SotcHotLines::MaxIdleFlips; ++f) {
            hot.OnFlip();
        }
        CHECK(hot.Empty());
        CHECK(!hot.Insert(10)); // it can come back
        CHECK(hot.Touch(10));
    }
    {
        // Replacement order is by last hit: refresh all but one, then that one goes first.
        SotcHotLines hot;
        hot.SetAging(true);
        for (u64 i = 0; i < SotcHotLines::Capacity; ++i) {
            hot.Insert(i);
        }
        hot.OnFlip();
        for (u64 i = 0; i < SotcHotLines::Capacity; ++i) {
            if (i != 100) {
                hot.Touch(i);
            }
        }
        CHECK(hot.Insert(5000));
        CHECK(!hot.Touch(100));
        CHECK(hot.Touch(5000));
        CHECK(hot.Touch(0) && hot.Touch(255));
    }
    {
        // Default (no aging): full is full, a new line is refused, flips change nothing.
        SotcHotLines hot;
        CHECK(!hot.Aging());
        for (u64 i = 0; i < SotcHotLines::Capacity; ++i) {
            CHECK(!hot.Insert(i));
        }
        CHECK(hot.Insert(9999)); // reports full
        CHECK(!hot.Touch(9999));
        CHECK(hot.Size() == SotcHotLines::Capacity);
        for (u64 f = 0; f < 2 * SotcHotLines::MaxIdleFlips; ++f) {
            CHECK(!hot.OnFlip());
        }
        CHECK(hot.Touch(0) && hot.Overlaps(255, 255));
    }
    {
        // Session 52: each line flushes at most `budget` times per flip; the flip refills.
        SotcHotLines hot;
        hot.SetBudget(2);
        hot.Insert(5);
        hot.Insert(7);
        CHECK(!hot.TakeFlush(0, 4)); // no hot line: not a denial
        CHECK(hot.TakeFlush(5, 5) && hot.TakeFlush(5, 5));
        CHECK(!hot.TakeFlush(5, 5)); // line 5 spent
        CHECK(hot.TakeFlush(5, 7));  // line 7 still has budget
        CHECK(hot.TakeFlush(7, 7));
        CHECK(!hot.TakeFlush(5, 7));
        CHECK(hot.TakeDenied() == 2 && hot.TakeDenied() == 0);
        CHECK(hot.OnFlip());  // a denial this flip: answers change with the refill
        CHECK(!hot.OnFlip()); // none since
        CHECK(hot.TakeFlush(5, 5));
        hot.SetBudget(0); // no limit
        for (int i = 0; i < 1000; ++i) {
            CHECK(hot.TakeFlush(7, 7));
        }
        CHECK(hot.TakeDenied() == 0);
    }
    {
        // ...and all lines together at most TotalBudgetFactor x budget per flip.
        SotcHotLines hot;
        hot.SetBudget(1);
        for (u64 i = 0; i < 10; ++i) {
            hot.Insert(i);
        }
        for (u64 i = 0; i < SotcHotLines::TotalBudgetFactor; ++i) {
            CHECK(hot.TakeFlush(i, i));
        }
        CHECK(!hot.TakeFlush(9, 9)); // its own budget is left, the total is spent
        CHECK(hot.TakeDenied() == 1);
        CHECK(hot.OnFlip());
        CHECK(hot.TakeFlush(9, 9));
    }
    {
        // Aging keeps the budgets parallel to the lines it keeps.
        SotcHotLines hot;
        hot.SetAging(true);
        hot.SetBudget(1);
        hot.Insert(10);
        hot.Insert(20);
        CHECK(hot.TakeFlush(20, 20) && !hot.TakeFlush(20, 20));
        for (u64 f = 0; f <= SotcHotLines::MaxIdleFlips; ++f) {
            hot.Touch(20);
            hot.OnFlip();
        }
        CHECK(!hot.Touch(10) && hot.Size() == 1);
        CHECK(hot.TakeFlush(20, 20) && !hot.TakeFlush(20, 20));
    }
    if (g_failures == 0) {
        std::printf("test_hot_lines: OK\n");
    }
    return g_failures == 0 ? 0 : 1;
}
