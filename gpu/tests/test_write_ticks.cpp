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
    if (g_failures) {
        std::printf("test_write_ticks: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("test_write_ticks: ok\n");
    return 0;
}
