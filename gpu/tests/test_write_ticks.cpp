// SPDX-License-Identifier: GPL-2.0-or-later
// Session 10: VideoCore::SotcWriteTicks, the newest GPU write tick per 64 KiB (1 MiB for big
// bindings) that tells a buffer download which submission produced its data.
#include <algorithm>
#include <cstdio>
#include <map>
#include <random>

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

    // Session 17: 4 KiB pages (SOTC_FINE_TICKS). SotC's guest readers fault on a line next to
    // a small binding the batch being recorded wrote in the same 64 KiB.
    VideoCore::SotcWriteTicks ft;
    ft.SetFinePages(true);
    CHECK(ft.FinePages());
    ft.Note(base + 0x10110, 0x434, 20);    // the reader's struct, submitted (current is 21)
    ft.Note(base + 0x12000, 0x8, 21);      // a neighbour written in the current batch
    ft.Note(base + 0x40000, 0x200000, 21); // a 2 MiB binding: still 64 KiB pages
    CHECK(ft.Newest(base + 0x10200, 4) == 20);
    CHECK(ft.Newest(base + 0x11000, 0x1000) == 0); // the 4 KiB page between them
    CHECK(ft.Newest(base + 0x12004, 4) == 21);
    CHECK(ft.Newest(base + 0x10000, 0x10000) == 21); // a 64 KiB query sees the 4 KiB pages
    CHECK(ft.Newest(base + 0x40000 + 0x1fff00, 4) == 21);
    CHECK(ft.Newest(base + 0x240000, 4) == 0);
    {
        // The window stops at the in-batch 4 KiB page instead of keeping the whole 64 KiB.
        const auto [s, e] = ft.TrimWindow(base, base + 0x80000, base + 0x10200, 4, 21);
        CHECK(s == base && e == base + 0x12000);
    }
    {
        // The reader's own 4 KiB page written in the batch: unchanged (Finish).
        const auto [s, e] = ft.TrimWindow(base, base + 0x80000, base + 0x12100, 4, 21);
        CHECK(s == base && e == base + 0x80000);
    }
    {
        // An in-batch 4 KiB page below the reader stops the downward walk on a 4 KiB boundary.
        ft.Note(base + 0xe000, 0x10, 21);
        const auto [s, e] = ft.TrimWindow(base, base + 0x80000, base + 0x10200, 4, 21);
        CHECK(s == base + 0xf000 && e == base + 0x12000);
    }
    {
        // Ticks never go back on 4 KiB pages either; a binding across pages raises each one.
        ft.Note(base + 0x10ff0, 0x20, 25);
        ft.Note(base + 0x10ff0, 0x20, 22);
        CHECK(ft.Newest(base + 0x10000, 4) == 25 && ft.Newest(base + 0x11000, 4) == 25);
        CHECK(ft.Newest(base + 0x13000, 4) == 0);
    }

    // Session 20: pages are stored in blocks of 512 (2 MiB of 4 KiB pages, 32 MiB of 64 KiB
    // pages, 512 MiB of 1 MiB pages). Bindings and queries across block boundaries.
    VideoCore::SotcWriteTicks bt;
    bt.SetFinePages(true);
    const u64 b4 = 0x4000000000 + (2ull << 20); // a 4 KiB-page block boundary
    bt.Note(b4 - 0x1000, 0x2000, 30);           // the last page of one block, first of the next
    CHECK(bt.Newest(b4 - 0x1000, 4) == 30 && bt.Newest(b4, 4) == 30);
    CHECK(bt.Newest(b4 - 0x2000, 4) == 0 && bt.Newest(b4 + 0x1000, 4) == 0);
    CHECK(bt.Newest(b4 - (4ull << 20), 8ull << 20) == 30); // a query over three blocks
    const u64 b64 = 0x5000000000 + (32ull << 20);           // a 64 KiB-page block boundary
    bt.Note(b64 - (3ull << 20), 6ull << 20, 31);            // 6 MiB binding across it
    CHECK(bt.Newest(b64 + (3ull << 20) - 4, 4) == 31 && bt.Newest(b64 + (3ull << 20), 4) == 0);
    CHECK(bt.Newest(b64 - (3ull << 20) - 0x10000, 0x10000) == 0);

    // Against a per-page model (the pre-session-20 map): random bindings and queries of every
    // size class, near block boundaries.
    {
        VideoCore::SotcWriteTicks rt;
        rt.SetFinePages(true);
        std::map<u64, u64> model[3]; // 4 KiB, 64 KiB, 1 MiB pages
        const u64 shifts[3] = {12, 16, 20};
        std::mt19937_64 rng(20);
        const u64 area = 0x6000000000;
        const u64 sizes[] = {4, 0x100, 0x1000, 0xfff0, 0x30d40, 1ull << 20, 3ull << 20,
                             16ull << 20, 40ull << 20};
        u64 tick = 1;
        for (int i = 0; i < 400; ++i) {
            const u64 addr = area + (rng() % (1ull << 31)) / 4 * 4;
            const u64 size = sizes[rng() % std::size(sizes)];
            tick += rng() % 3;
            const int cls = size > (16ull << 20) ? 2 : size <= (1ull << 20) ? 0 : 1;
            rt.Note(addr, size, tick);
            for (u64 p = addr >> shifts[cls]; p <= (addr + size - 1) >> shifts[cls]; ++p) {
                u64& m = model[cls][p];
                m = std::max(m, tick);
            }
        }
        for (int i = 0; i < 400; ++i) {
            const u64 addr = area + (rng() % (1ull << 31)) / 4 * 4;
            const u64 size = sizes[rng() % std::size(sizes)];
            u64 want = 0;
            for (int c = 0; c < 3; ++c) {
                for (u64 p = addr >> shifts[c]; p <= (addr + size - 1) >> shifts[c]; ++p) {
                    if (const auto it = model[c].find(p); it != model[c].end()) {
                        want = std::max(want, it->second);
                    }
                }
            }
            CHECK(rt.Newest(addr, size) == want);
        }
    }
    if (g_failures) {
        std::printf("test_write_ticks: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("test_write_ticks: ok\n");
    return 0;
}
