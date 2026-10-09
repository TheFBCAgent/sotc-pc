// SPDX-License-Identifier: GPL-2.0-or-later
// Session 23: the staging ring policy (video_core/renderer_vulkan/sotc_staging_ring.h), run on a
// model of StagingBufferPool's ring of StreamBuffers (VideoCore::StreamBuffer's wrap and watch
// logic, in units instead of bytes) and a GPU that finishes ticks on demand.
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <optional>
#include <vector>

#include "video_core/renderer_vulkan/sotc_staging_ring.h"

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

using u64 = std::uint64_t;
namespace R = SotcStagingRing;

struct Gpu {
    u64 current = 1; ///< tick being recorded
    u64 done = 0;    ///< newest finished tick
    u64 waits = 0;
    bool IsFree(u64 t) const {
        return t <= done;
    }
    void Wait(u64 t) {
        ++waits;
        if (t >= current) {
            ++current; // Scheduler::Wait submits the batch being recorded
        }
        done = std::max(done, t);
    }
};

/// VideoCore::StreamBuffer's PrepareMap / WaitPendingOperations / AdvanceAndWatch.
struct Block {
    struct Watch {
        u64 tick, upper;
    };
    static constexpr u64 Size = 16;
    Gpu* gpu;
    u64 offset = 0, last_tick = 0, last_used_frame = 0;
    std::vector<Watch> current, previous;
    std::optional<size_t> mark;
    size_t wait_cursor = 0;
    u64 wait_bound = 0;

    std::optional<u64> Reserve(u64 size, bool allow_wait) {
        if (offset + size > Size) {
            mark = current.size();
            std::swap(previous, current);
            current.clear();
            offset = 0;
            wait_cursor = 0;
            wait_bound = 0;
        }
        if (mark) {
            while (offset + size > wait_bound && wait_cursor < *mark) {
                const Watch& w = previous[wait_cursor];
                if (!gpu->IsFree(w.tick)) {
                    if (!allow_wait) {
                        return std::nullopt;
                    }
                    gpu->Wait(w.tick);
                }
                wait_bound = w.upper;
                ++wait_cursor;
            }
        }
        const u64 at = offset;
        offset += size;
        last_tick = gpu->current;
        if (!current.empty() && current.back().tick == gpu->current) {
            current.back().upper = offset;
        } else {
            current.push_back({gpu->current, offset});
        }
        return at;
    }
    u64 BlockingTick() const {
        return mark && wait_cursor < *mark ? previous[wait_cursor].tick : 0;
    }
};

/// StagingBufferPool::RequestFromRing and TrimRing, with the upstream order as an option.
struct Ring {
    Gpu* gpu;
    bool policy; ///< false: upstream (round robin, never capped)
    std::vector<Block> blocks;
    size_t cur = 0;
    u64 frame = 0, grown = 0, capped = 0;

    void Request(u64 size) {
        std::optional<size_t> hit;
        auto attempt = [&](size_t i) { return blocks[i].Reserve(size, false).has_value(); };
        if (policy) {
            hit = R::FindBlock(blocks.size(), cur, attempt);
        } else {
            for (size_t i = 0; i < blocks.size() && !hit; ++i) {
                const size_t index = (cur + i) % blocks.size();
                if (attempt(index)) {
                    hit = index;
                }
            }
        }
        if (!hit && policy && !R::MayGrow(blocks.size())) {
            hit = R::OldestBlock(blocks.size(), [&](size_t i) { return blocks[i].BlockingTick(); });
            blocks[*hit].Reserve(size, true);
            ++capped;
        }
        if (!hit) {
            blocks.push_back(Block{.gpu = gpu});
            hit = blocks.size() - 1;
            CHECK(blocks.back().Reserve(size, false).has_value());
            ++grown;
        }
        cur = *hit;
        blocks[cur].last_used_frame = frame;
    }
    void TickFrame() {
        ++frame;
        for (size_t i = blocks.size(); i-- > 0;) {
            const Block& b = blocks[i];
            auto is_free = [&] { return gpu->IsFree(b.last_tick); };
            const bool trim = policy ? R::ShouldTrim(frame, b.last_used_frame, is_free)
                                     : frame - b.last_used_frame >= 300 && is_free();
            if (!trim) {
                continue;
            }
            blocks.erase(blocks.begin() + i);
            if (i < cur) {
                --cur;
            } else if (i == cur) {
                cur = 0;
            }
        }
        if (cur >= blocks.size()) {
            cur = 0;
        }
    }
};

/// A loading batch of `load` units in one tick, then `frames` frames of `per_frame` units in
/// `batches` submits each, the GPU finishing everything older than `behind` ticks.
static Ring Run(bool policy, u64 load, u64 frames, u64 per_frame, u64 batches, u64 behind) {
    static Gpu gpu;
    gpu = Gpu{};
    Ring ring{.gpu = &gpu, .policy = policy};
    for (u64 i = 0; i < load; ++i) {
        ring.Request(1);
    }
    ++gpu.current;
    for (u64 f = 0; f < frames; ++f) {
        for (u64 b = 0; b < batches; ++b) {
            for (u64 i = 0; i < per_frame / batches; ++i) {
                ring.Request(1);
            }
            ++gpu.current;
            if (gpu.current > behind + 1) {
                gpu.done = std::max(gpu.done, gpu.current - 1 - behind);
            }
        }
        ring.TickFrame();
    }
    return ring;
}

int main() {
    {
        // Order: the current block, then the lowest index up.
        std::vector<size_t> tried;
        auto none = [&](size_t i) {
            tried.push_back(i);
            return false;
        };
        CHECK(!R::FindBlock(4, 2, none));
        CHECK((tried == std::vector<size_t>{2, 0, 1, 3}));
        tried.clear();
        CHECK(R::FindBlock(4, 2, [&](size_t i) { return i == 1; }) == 1u);
        CHECK(!R::FindBlock(0, 0, none));
        tried.clear();
        CHECK(!R::FindBlock(3, 7, none)); // a stale current index is skipped
        CHECK((tried == std::vector<size_t>{0, 1, 2}));
    }
    {
        CHECK(R::ShouldTrim(100, 40, [] { return true; }));
        CHECK(!R::ShouldTrim(100, 41, [] { return true; }));
        CHECK(!R::ShouldTrim(100, 0, [] { return false; }));
        bool asked = false;
        CHECK(!R::ShouldTrim(10, 5, [&] { return asked = true; }));
        CHECK(!asked); // not asked for a busy block
        CHECK(R::MayGrow(R::SoftCap - 1));
        CHECK(!R::MayGrow(R::SoftCap));
        const u64 ticks[] = {9, 0, 4, 7};
        CHECK(R::OldestBlock(4, [&](size_t i) { return ticks[i]; }) == 1);
        const u64 ticks2[] = {9, 5, 4, 7};
        CHECK(R::OldestBlock(4, [&](size_t i) { return ticks2[i]; }) == 2);
    }
    {
        // SotC as counted in session 23: a ~1 GB loading batch (78 blocks), then ~2 blocks of
        // uploads per frame in 8 submits, the GPU 3 submits behind. Upstream ran TickFrame only
        // from OnSubmit, which stopped in gameplay (0 frames: never trimmed); ticked, 300 idle
        // frames. The policy caps the growth and trims after IdleFrames.
        const Ring up = Run(false, 78 * 16, 0, 32, 8, 3);
        CHECK(up.blocks.size() == 78);
        const Ring up_ticked = Run(false, 78 * 16, R::IdleFrames + 10, 32, 8, 3);
        CHECK(up_ticked.blocks.size() >= 78);
        const Ring now = Run(true, 78 * 16, R::IdleFrames + 10, 32, 8, 3);
        CHECK(now.grown == R::SoftCap);
        CHECK(now.capped > 0);
        CHECK(now.blocks.size() <= 2);
    }
    {
        // The GPU 3 frames behind (~6 blocks in flight): round robin from the current block
        // cycles through every block, so none idles; lowest index first keeps the rest idle.
        const Ring up = Run(false, 32 * 16, 1000, 32, 8, 24);
        CHECK(up.blocks.size() >= 32); // grew past the load and never trims
        const Ring now = Run(true, 32 * 16, 1000, 32, 8, 24);
        CHECK(now.blocks.size() <= 8);
        CHECK(now.blocks.size() >= 6);
        std::printf("gpu 3 frames behind after a load: upstream %zu blocks, policy %zu\n",
                    up.blocks.size(), now.blocks.size());
    }
    {
        // Steady state alone never waits and holds about what is in flight.
        const Ring now = Run(true, 0, 400, 32, 8, 3);
        CHECK(now.capped == 0);
        CHECK(now.blocks.size() <= 2);
        const Ring far = Run(true, 0, 400, 32, 8, 24);
        CHECK(far.capped == 0);
        CHECK(far.blocks.size() <= 8);
    }
    if (g_failures == 0) {
        std::printf("test_staging_ring: OK\n");
    }
    return g_failures == 0 ? 0 : 1;
}
