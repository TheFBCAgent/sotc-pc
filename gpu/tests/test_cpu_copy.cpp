// SPDX-License-Identifier: GPL-2.0-or-later
// Session 21: Vulkan::SotcCpuCopy, the plan and copy for SotC's copy kernel 0x50784801
// (SOTC_CPU_COPY, video_core/renderer_vulkan/sotc_cpu_copy.h).
#include <cstdio>
#include <vector>

#include "video_core/renderer_vulkan/sotc_cpu_copy.h"

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

using namespace Vulkan::SotcCpuCopy;

static Sharp Dword(std::uint64_t base, std::uint32_t records) {
    return {.base = base, .stride = 4, .num_records = records, .data_format = 4, .num_format = 7,
            .dst_sel_x = 4};
}

static Dispatch Groups(std::uint32_t x) {
    return {.dim_x = x, .dim_y = 1, .dim_z = 1, .threads_x = 64, .threads_y = 1, .threads_z = 1};
}

/// The kernel as the IR has it, thread by thread (cs_0x50784801.irprogram.txt).
static std::vector<std::uint32_t> Reference(const std::vector<std::uint32_t>& src,
                                            std::vector<std::uint32_t> dst, std::uint32_t ctl0,
                                            std::uint32_t ctl1, std::uint32_t groups) {
    for (std::uint32_t g = 0; g < groups; ++g) {
        for (std::uint32_t l = 0; l < 64; ++l) {
            const std::uint32_t i = g * 64 + l;
            if (i < ctl0 && i < dst.size()) {
                const std::uint32_t j = i % ctl1;
                dst[i] = j < src.size() ? src[j] : 0;
            }
        }
    }
    return dst;
}

static void CheckMatches(std::uint32_t src_n, std::uint32_t dst_n, std::uint32_t ctl0,
                         std::uint32_t ctl1, std::uint32_t groups) {
    std::vector<std::uint32_t> src(src_n), dst(dst_n, 0xdeadbeef);
    for (std::uint32_t i = 0; i < src_n; ++i) {
        src[i] = 0x1000 + i;
    }
    const Plan p = MakePlan(Dword(0x1000, src_n), Dword(0x100000, dst_n), 8, ctl0, ctl1,
                            Groups(groups));
    CHECK(p.reject == Reject::None);
    if (p.reject != Reject::None) {
        return;
    }
    CHECK(p.src_dwords <= src_n);
    std::vector<std::uint32_t> got = dst;
    Run(p, src.data(), got.data());
    CHECK(got == Reference(src, dst, ctl0, ctl1, groups));
}

int main() {
    // Straight copies, fills, a period shorter than the count, limits from each bound.
    CheckMatches(32, 32, 32, 32, 1);     // the 0x80 descriptor tables
    CheckMatches(704, 704, 704, 704, 11); // the 0xb00 per-view table
    CheckMatches(4, 64, 64, 4, 1);       // repeat a 4-dword pattern
    CheckMatches(1, 100, 100, 1, 2);     // fill
    CheckMatches(64, 64, 20, 64, 1);     // ctl[0] bound
    CheckMatches(64, 10, 64, 64, 1);     // dst.num_records bound
    CheckMatches(200, 200, 200, 200, 2); // dispatch bound (128 threads)
    {
        const Plan p = MakePlan(Dword(0x1000, 200), Dword(0x100000, 200), 8, 200, 200, Groups(2));
        CHECK(p.count == 128 && p.src_dwords == 128);
    }

    // Rejections.
    Sharp s = Dword(0x1000, 32);
    Sharp d = Dword(0x100000, 32);
    CHECK(MakePlan(s, d, 8, 32, 32, Groups(1)).reject == Reject::None);
    {
        Sharp t = s;
        t.stride = 8;
        CHECK(MakePlan(t, d, 8, 32, 32, Groups(1)).reject == Reject::Sharp);
        t = s;
        t.data_format = 10; // Format8_8_8_8
        CHECK(MakePlan(t, d, 8, 32, 32, Groups(1)).reject == Reject::Sharp);
        t = s;
        t.num_format = 0; // Unorm
        CHECK(MakePlan(t, d, 8, 32, 32, Groups(1)).reject == Reject::Sharp);
        t = s;
        t.num_format = 4; // Uint vs Float destination
        CHECK(MakePlan(t, d, 8, 32, 32, Groups(1)).reject == Reject::Sharp);
        t = s;
        t.dst_sel_x = 5;
        CHECK(MakePlan(t, d, 8, 32, 32, Groups(1)).reject == Reject::Sharp);
        t = s;
        t.swizzle = true;
        CHECK(MakePlan(t, d, 8, 32, 32, Groups(1)).reject == Reject::Sharp);
        t = d;
        t.add_tid = true;
        CHECK(MakePlan(s, t, 8, 32, 32, Groups(1)).reject == Reject::Sharp);
    }
    {
        Dispatch g = Groups(1);
        g.threads_x = 32;
        CHECK(MakePlan(s, d, 8, 32, 32, g).reject == Reject::Dispatch);
        g = Groups(1);
        g.start_x = 1;
        CHECK(MakePlan(s, d, 8, 32, 32, g).reject == Reject::Dispatch);
        g = Groups(1);
        g.partial_x = 16;
        CHECK(MakePlan(s, d, 8, 32, 32, g).reject == Reject::Dispatch);
        g = Groups(1);
        g.dim_y = 0;
        CHECK(MakePlan(s, d, 8, 32, 32, g).reject == Reject::Empty);
        g = Groups(1);
        g.dim_y = 3; // more groups in y write the same values again
        CHECK(MakePlan(s, d, 8, 32, 32, g).reject == Reject::None);
    }
    CHECK(MakePlan(s, d, 8, 32, 32, Groups(0)).reject == Reject::Empty);
    CHECK(MakePlan(s, d, 8, 0, 32, Groups(1)).reject == Reject::Empty);
    CHECK(MakePlan(s, d, 4, 32, 32, Groups(1)).reject == Reject::CtlSize);
    CHECK(MakePlan(s, d, 8, 32, 0, Groups(1)).reject == Reject::ZeroPeriod);
    // Size cap: MaxDwords is fine, one more is left to the GPU.
    CHECK(MakePlan(Dword(0x1000, 4), Dword(0x10000000, MaxDwords + 64), 8, MaxDwords, 4,
                   Groups(MaxDwords / 64 + 1))
              .reject == Reject::None);
    CHECK(MakePlan(Dword(0x1000, 4), Dword(0x10000000, MaxDwords + 64), 8, MaxDwords + 1, 4,
                   Groups(MaxDwords / 64 + 1))
              .reject == Reject::Large);
    CHECK(MakePlan(Dword(0x1000, 16), d, 8, 32, 32, Groups(1)).reject == Reject::SrcBounds);
    CHECK(MakePlan(Dword(0x1000, 16), d, 8, 32, 16, Groups(1)).reject == Reject::None);
    // Overlap: the destination starts inside the source's read range, and the reverse.
    CHECK(MakePlan(Dword(0x1000, 32), Dword(0x1040, 32), 8, 32, 32, Groups(1)).reject ==
          Reject::Overlap);
    CHECK(MakePlan(Dword(0x1040, 32), Dword(0x1000, 32), 8, 32, 32, Groups(1)).reject ==
          Reject::Overlap);
    // Adjacent is fine; a source past the read range (count 16) doesn't overlap.
    CHECK(MakePlan(Dword(0x1080, 32), Dword(0x1000, 32), 8, 32, 32, Groups(1)).reject ==
          Reject::None);
    CHECK(MakePlan(Dword(0x1000, 32), Dword(0x1040, 16), 8, 16, 16, Groups(1)).reject ==
          Reject::None);

    if (g_failures == 0) {
        std::printf("test_cpu_copy: all passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
