// SPDX-License-Identifier: GPL-2.0-or-later
// Session 10: VideoCore::SotcWriteTicks, the newest GPU write tick per 64 KiB (1 MiB for big
// bindings) that tells a buffer download which submission produced its data.
#include <cstdio>

#include "video_core/buffer_cache/sotc_write_ticks.h"

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

int main() {
    VideoCore::SotcWriteTicks ticks;
    CHECK(ticks.Newest(0x1000000000, 0x1000) == 0);
    // A 200 KB binding written at tick 5 covers four 64 KiB pages.
    ticks.Note(0x104273ed10, 0x30d40, 5);
    CHECK(ticks.Newest(0x104276fa60, 4) == 5);  // same 64 KiB page as its end
    CHECK(ticks.Newest(0x1042730000, 4) == 5);  // its first page
    CHECK(ticks.Newest(0x1042780000, 4) == 0);  // the page after it
    // A later write to part of it raises only the pages it covers.
    ticks.Note(0x1042740000, 0x100, 9);
    CHECK(ticks.Newest(0x1042740010, 4) == 9);
    CHECK(ticks.Newest(0x1042750000, 4) == 5);
    CHECK(ticks.Newest(0x1042730000, 0x30000) == 9); // the max over a range
    // Ticks never go back.
    ticks.Note(0x1042740000, 0x100, 7);
    CHECK(ticks.Newest(0x1042740000, 4) == 9);
    // Bindings over 16 MiB go to 1 MiB pages.
    ticks.Note(0x2000000000, 64ull << 20, 12);
    CHECK(ticks.Newest(0x2000000000 + (40ull << 20) + 5, 4) == 12);
    CHECK(ticks.Newest(0x2000000000 + (64ull << 20), 4) == 0);
    CHECK(ticks.Newest(0x2000000000, 0) == 0);

    // Session 15: TrimWindow. A 512 KiB window 0x3000000000..+0x80000 with the reader's
    // constants at +0x10110 written at tick 20 (submitted; the current batch is 21), a
    // neighbour at +0x30000 written in the current batch, and one at +0x4000 written earlier.
    VideoCore::SotcWriteTicks tw;
    const u64 base = 0x3000000000;
    tw.Note(base + 0x10110, 0xb00, 20);
    tw.Note(base + 0x30000, 0x100, 21);
    tw.Note(base + 0x4000, 0x40, 3);
    {
        // Down to the window start (all older), up to the in-batch page.
        const auto [s, e] = tw.TrimWindow(base, base + 0x80000, base + 0x10200, 4, 21);
        CHECK(s == base && e == base + 0x30000);
    }
    {
        // The reader's own data is in the batch: unchanged.
        const auto [s, e] = tw.TrimWindow(base, base + 0x80000, base + 0x30010, 4, 21);
        CHECK(s == base && e == base + 0x80000);
    }
    {
        // Nothing in the batch: unchanged.
        const auto [s, e] = tw.TrimWindow(base, base + 0x80000, base + 0x10200, 4, 22);
        CHECK(s == base && e == base + 0x80000);
    }
    {
        // An in-batch page below the reader stops the downward walk.
        tw.Note(base + 0x8000, 0x10, 21);
        const auto [s, e] = tw.TrimWindow(base, base + 0x80000, base + 0x10200, 4, 21);
        CHECK(s == base + 0x10000 && e == base + 0x30000);
    }
    {
        // A window not 64 KiB aligned (arena start), and a request spanning two pages.
        const auto [s, e] =
            tw.TrimWindow(base + 0x12345, base + 0x2f000, base + 0x1fff0, 0x20, 21);
        CHECK(s == base + 0x12345 && e == base + 0x2f000);
        CHECK(s <= base + 0x1fff0 && base + 0x20010 <= e);
    }
    if (g_failures) {
        std::printf("test_write_ticks: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("test_write_ticks: ok\n");
    return 0;
}
