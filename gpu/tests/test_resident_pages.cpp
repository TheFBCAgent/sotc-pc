// SPDX-License-Identifier: GPL-2.0-or-later
// Session 22: VideoCore::SotcResidentPages, the pages SOTC_CPU_COPY may write
// (video_core/buffer_cache/sotc_resident_pages.h).
#include <cstdio>

#include "video_core/buffer_cache/sotc_resident_pages.h"

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

int main() {
    using VideoCore::SotcResidentPages;
    constexpr std::uint64_t base = 0x1042300000;

    {
        SotcResidentPages r;
        CHECK(!r.All(base, 4));
        CHECK(!r.All(base, 0));
        CHECK(r.Covered(base, 0x2000).total == 2 && r.Covered(base, 0x2000).marked == 0);
        r.Mark(base + 0x10, 4); // a partial page counts as a whole
        CHECK(r.All(base, 0x1000));
        CHECK(!r.All(base, 0x1001));
        CHECK(r.Covered(base, 0x1001).marked == 1 && r.Covered(base, 0x1001).total == 2);
        CHECK(r.Pages() == 1);
    }
    {
        // A range across 1 MiB chunks: the 4 MiB buffer.
        SotcResidentPages r;
        r.Mark(base, 4u << 20);
        CHECK(r.Pages() == 1024);
        CHECK(r.All(base, 4u << 20));
        CHECK(r.Covered(base, 4u << 20).All());
        CHECK(!r.All(base, (4u << 20) + 1));
        CHECK(!r.All(base - 1, 2));
    }
    {
        // Unaligned start and end across a chunk boundary; one hole.
        SotcResidentPages r;
        const std::uint64_t a = base + 0xff800;
        r.Mark(a, 0x2000);       // pages 0xff, 0x100, 0x101
        CHECK(r.Pages() == 3);
        CHECK(r.All(base + 0xff000, 0x3000));
        CHECK(!r.All(base + 0xfe000, 0x2000));
        r.Mark(base + 0x103000, 1);
        CHECK(!r.All(base + 0xff000, 0x5000)); // 0x102 missing
        CHECK(r.Covered(base + 0xff000, 0x5000).marked == 4);
        r.Mark(base + 0x102fff, 1);
        CHECK(r.All(base + 0xff000, 0x5000));
    }
    {
        // A whole chunk takes the bitset::all() path; a full chunk minus one page fails.
        SotcResidentPages r;
        r.Mark(base, (1u << 20) - 0x1000);
        CHECK(!r.All(base, 1u << 20));
        r.Mark(base + (1u << 20) - 1, 1);
        CHECK(r.All(base, 1u << 20));
    }

    {
        // Runs, clipped to the range: [hole][marked x2][hole], unaligned ends.
        SotcResidentPages r;
        r.Mark(base + 0x1000, 0x2000);
        struct Run {
            std::uint64_t a, n;
            bool res;
        };
        Run runs[8];
        int k = 0;
        r.ForEachRun(base + 0x800, 0x3000, [&](std::uint64_t a, std::uint64_t n, bool res) {
            if (k < 8) {
                runs[k++] = {a, n, res};
            }
        });
        CHECK(k == 3);
        CHECK(runs[0].a == base + 0x800 && runs[0].n == 0x800 && !runs[0].res);
        CHECK(runs[1].a == base + 0x1000 && runs[1].n == 0x2000 && runs[1].res);
        CHECK(runs[2].a == base + 0x3000 && runs[2].n == 0x800 && !runs[2].res);
        k = 0;
        r.ForEachRun(base + 0x1004, 8, [&](std::uint64_t a, std::uint64_t n, bool res) {
            runs[k++] = {a, n, res};
        });
        CHECK(k == 1 && runs[0].a == base + 0x1004 && runs[0].n == 8 && runs[0].res);
        k = 0;
        r.ForEachRun(base, 0, [&](std::uint64_t, std::uint64_t, bool) { ++k; });
        CHECK(k == 0);
    }

    if (g_failures == 0) {
        std::printf("test_resident_pages: OK\n");
    }
    return g_failures == 0 ? 0 : 1;
}
