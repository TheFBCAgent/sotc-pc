// SPDX-License-Identifier: GPL-2.0-or-later
// Session 32: VideoCore::SotcRangeLog, FlushStreamedRanges' log of streamed ranges
// (video_core/buffer_cache/sotc_range_log.h). Its merged ranges must equal a set's.
#include <cstdio>
#include <random>
#include <set>
#include <utility>
#include <vector>

#include "video_core/buffer_cache/sotc_range_log.h"

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

using Ranges = std::vector<std::pair<std::uint64_t, std::uint64_t>>;

static Ranges Collect(VideoCore::SotcRangeLog& log) {
    Ranges out;
    log.ForEach([&](std::uint64_t s, std::uint64_t e) { out.emplace_back(s, e); });
    return out;
}

// Reference: the merged runs of a byte set (adjacent ranges join, as in boost::icl).
static Ranges Reference(const std::set<std::uint64_t>& bytes) {
    Ranges out;
    for (const std::uint64_t b : bytes) {
        if (!out.empty() && out.back().second == b) {
            out.back().second = b + 1;
        } else {
            out.emplace_back(b, b + 1);
        }
    }
    return out;
}

int main() {
    using VideoCore::SotcRangeLog;
    constexpr std::uint64_t base = 0x1042300000;
    {
        SotcRangeLog log;
        CHECK(Collect(log).empty());
        log.Add(base + 0x100, 0x10);
        log.Add(base, 0x10);
        log.Add(base + 0x10, 0x10); // touches the first: joins
        log.Add(base + 0x100, 0x10); // a repeat widens nothing
        log.Add(base + 0x8, 0x4);    // inside
        const Ranges r = Collect(log);
        CHECK(r.size() == 2);
        CHECK(r[0] == std::make_pair(base, base + 0x20));
        CHECK(r[1] == std::make_pair(base + 0x100, base + 0x110));
        log.Clear();
        CHECK(Collect(log).empty() && log.Entries() == 0);
    }
    {
        // Random binds over a small window, many more than the compaction threshold.
        std::mt19937_64 rng(32);
        SotcRangeLog log;
        std::set<std::uint64_t> bytes;
        for (int i = 0; i < 50000; ++i) {
            const std::uint64_t at = rng() % 0x8000;
            const std::uint64_t size = 1 + rng() % 0x40;
            log.Add(base + at, size);
            for (std::uint64_t b = at; b < at + size; ++b) {
                bytes.insert(base + b);
            }
            CHECK(log.Entries() <= 2 * SotcRangeLog::MinCompact + 0x8000);
        }
        CHECK(Collect(log) == Reference(bytes));
    }
    {
        // Disjoint repeats stay bounded by the distinct ranges (the set's size).
        SotcRangeLog log;
        for (int round = 0; round < 100; ++round) {
            for (std::uint64_t i = 0; i < 1000; ++i) {
                log.Add(base + i * 0x100, 0x10);
            }
        }
        CHECK(log.Entries() <= 2 * SotcRangeLog::MinCompact);
        CHECK(Collect(log).size() == 1000);
    }
    if (g_failures == 0) {
        std::printf("test_range_log: ok\n");
    }
    return g_failures == 0 ? 0 : 1;
}
