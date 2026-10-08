// SPDX-License-Identifier: GPL-2.0-or-later
// Session 12: SotcGpuBusy, the union of GPU timestamp intervals behind "gpu busy X ms/s".
#include <cstdio>

#include "sotc_gpu_busy.h"

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

using SotcGpuBusy::Interval;
using SotcGpuBusy::UnionTicks;

int main() {
    uint64_t floor = 0;
    std::vector<Interval> v;
    CHECK(UnionTicks(v, floor) == 0);
    CHECK(floor == 0);

    // Disjoint, out of order: plain sum.
    v = {{300, 400}, {100, 150}};
    CHECK(UnionTicks(v, floor) == 150);
    CHECK(floor == 400);

    // Overlap between the main queue and the side-copy queue counts once; a nested one adds 0.
    floor = 0;
    v = {{1000, 2000}, {1500, 2500}, {1200, 1300}};
    CHECK(UnionTicks(v, floor) == 1500);
    CHECK(floor == 2500);

    // The next call (a later stats tick) only counts past the floor: [2400, 2600) adds 100.
    v = {{2400, 2600}, {3000, 3100}};
    CHECK(UnionTicks(v, floor) == 200);
    CHECK(floor == 3100);

    // Entirely before the floor: nothing; the floor never goes back.
    v = {{500, 900}};
    CHECK(UnionTicks(v, floor) == 0);
    CHECK(floor == 3100);

    // Empty and inverted (wrapped) intervals are skipped.
    floor = 0;
    v = {{10, 10}, {50, 20}, {60, 70}};
    CHECK(UnionTicks(v, floor) == 10);

    // Touching intervals: no gap, no double count.
    floor = 0;
    v = {{0, 10}, {10, 20}, {20, 25}};
    CHECK(UnionTicks(v, floor) == 25);

    // Valid bits and the tick period.
    CHECK(SotcGpuBusy::MaskTicks(0xffff'0000'0000'1234ull, 36) == 0x1234);
    CHECK(SotcGpuBusy::MaskTicks(0xffff'0000'0000'1234ull, 64) == 0xffff'0000'0000'1234ull);
    CHECK(SotcGpuBusy::TicksToMs(2'000'000, 1.0f) == 2.0);
    CHECK(SotcGpuBusy::TicksToMs(1'000'000, 0.5f) == 0.5);

    if (g_failures) {
        std::printf("test_gpu_busy: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("test_gpu_busy: ok\n");
    return 0;
}
