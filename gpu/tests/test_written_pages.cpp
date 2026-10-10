// SPDX-License-Identifier: GPL-2.0-or-later
// Session 34: VideoCore::SotcWrittenPages, the page prefilter in front of
// BufferCache::IsRangeGpuWritten (video_core/buffer_cache/sotc_written_pages.h). It may answer
// "maybe" too often, never "no" for a range holding a GPU-written byte.
#include <cstdio>
#include <random>
#include <vector>

#include "video_core/buffer_cache/sotc_written_pages.h"

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

using VideoCore::SotcWrittenPages;
constexpr std::uint64_t K4 = 4096;
constexpr std::uint64_t GiB = 1ull << 30;

static void TestBasics() {
    auto pages = std::make_unique<SotcWrittenPages>();
    CHECK(!pages->Maybe(0x1042710110, 0xb00));
    CHECK(pages->Maybe(0x1042710110, 0)); // size 0: the precise search decides

    pages->Mark(0x1042710110, 0xb00); // one page
    CHECK(pages->Maybe(0x1042710000, 1));
    CHECK(pages->Maybe(0x1042710ffc, 4));
    CHECK(pages->Maybe(0x104270fffc, 8)); // straddles into the page
    CHECK(!pages->Maybe(0x104270f000, K4));
    CHECK(!pages->Maybe(0x1042711000, 0x100));

    pages->Unmark(0x1042710100, 0xc00); // not the whole page: stays marked
    CHECK(pages->Maybe(0x1042710800, 4));
    pages->Unmark(0x1042710000, K4);
    CHECK(!pages->Maybe(0x1042710000, K4));

    // A range over a 1 GiB chunk boundary and many words.
    pages->Mark(GiB * 66 - 3 * K4, 300 * K4);
    CHECK(pages->Maybe(GiB * 66 - 3 * K4, 1));
    CHECK(pages->Maybe(GiB * 66 + 296 * K4, 1));
    CHECK(!pages->Maybe(GiB * 66 + 297 * K4, 1));
    CHECK(!pages->Maybe(GiB * 66 - 4 * K4, K4));
    CHECK(pages->Maybe(GiB * 65, 2 * GiB)); // long query finds it
    pages->Unmark(GiB * 66 - 3 * K4 + 1, 300 * K4); // first and last pages partly covered
    CHECK(pages->Maybe(GiB * 66 - 3 * K4, 1));
    CHECK(!pages->Maybe(GiB * 66 - 2 * K4, 297 * K4));
    pages->Mark(GiB * 66 + 297 * K4, 1); // the partly covered last page keeps its mark
    pages->Unmark(GiB * 66 + 297 * K4, K4 - 1);
    CHECK(pages->Maybe(GiB * 66 + 297 * K4 + 4, 1));
}

static void TestBounds() {
    auto pages = std::make_unique<SotcWrittenPages>();
    const std::uint64_t top = 1ull << SotcWrittenPages::AddressBits;
    CHECK(pages->Maybe(top, 4));            // untracked: always maybe
    CHECK(pages->Maybe(top - 4, 8));        // straddles out of the tracked space
    CHECK(pages->Maybe(~0ull - 3, 8));      // wraps
    pages->Mark(top - K4, 2 * K4);          // the tracked part is marked
    CHECK(pages->Maybe(top - 4, 4));
    pages->Mark(~0ull - 3, 8);              // wraps: must not crash
    pages->Unmark(~0ull - 3, 8);
    pages->Unmark(top - K4, K4);
    CHECK(!pages->Maybe(top - K4, K4));
}

// Random Add/Subtract on a byte model around a chunk boundary: a range holding a written byte
// must answer "maybe"; a range whose pages were all wholly subtracted since or never added
// must answer "no".
static void TestRandom() {
    auto pages = std::make_unique<SotcWrittenPages>();
    constexpr std::uint64_t Pages = 192;
    const std::uint64_t base = GiB * 67 - 96 * K4;
    std::vector<std::uint8_t> bytes(Pages * K4);
    std::vector<std::uint8_t> marked(Pages); // the model: Mark sets, whole-page Unmark clears
    std::mt19937_64 rng(34);
    for (int step = 0; step < 20000; ++step) {
        const std::uint64_t off = rng() % (Pages * K4);
        const std::uint64_t len = 1 + rng() % std::min<std::uint64_t>(Pages * K4 - off, 9 * K4);
        const int op = static_cast<int>(rng() % 3);
        if (op == 0) {
            pages->Mark(base + off, len);
            for (std::uint64_t i = off; i < off + len; ++i) {
                bytes[i] = 1;
            }
            for (std::uint64_t p = off / K4; p <= (off + len - 1) / K4; ++p) {
                marked[p] = 1;
            }
        } else if (op == 1) {
            pages->Unmark(base + off, len);
            for (std::uint64_t i = off; i < off + len; ++i) {
                bytes[i] = 0;
            }
            for (std::uint64_t p = (off + K4 - 1) / K4; (p + 1) * K4 <= off + len; ++p) {
                marked[p] = 0;
            }
        } else {
            bool written = false, any_marked = false;
            for (std::uint64_t i = off; i < off + len && !written; ++i) {
                written = bytes[i] != 0;
            }
            for (std::uint64_t p = off / K4; p <= (off + len - 1) / K4; ++p) {
                any_marked |= marked[p] != 0;
            }
            const bool maybe = pages->Maybe(base + off, len);
            CHECK(!written || maybe);
            CHECK(maybe == any_marked);
        }
    }
}

int main() {
    TestBasics();
    TestBounds();
    TestRandom();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("test_written_pages: ok\n");
    return 0;
}
