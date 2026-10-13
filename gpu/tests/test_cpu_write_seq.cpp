// SPDX-License-Identifier: GPL-2.0-or-later
// Whole-draw memo W2: VideoCore::SotcCpuWriteSeq, the per-64 KiB CPU write sequence a cached
// buffer binding snapshots (docs/next/draw-memo-W2.md). Correctness cases, a concurrent-bump
// check, and a microbenchmark of Snapshot + Unchanged (target: < 50 ns for <= 64 KiB).
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "video_core/buffer_cache/sotc_cpu_write_seq.h"

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

using Seq = VideoCore::SotcCpuWriteSeq;

static void Correctness() {
    Seq seq;
    const u64 base = 0x1042730000; // 64 KiB aligned, inside the dmem arena

    // Nothing bumped yet: snapshots of untouched memory stay unchanged.
    const auto s0 = seq.Snapshot(base, 0x1000);
    CHECK(s0.sum == 0);
    CHECK(seq.Unchanged(s0));

    // An unaligned binding crossing a 64 KiB boundary covers two blocks.
    const auto cross = seq.Snapshot(base + 0xff00, 0x200); // blocks base and base+0x10000
    const auto first = seq.Snapshot(base, 0x100);
    const auto second = seq.Snapshot(base + 0x10000, 0x100);
    const auto third = seq.Snapshot(base + 0x20000, 0x100);
    seq.Bump(base + 0x10004, 8); // a write fault in the second block
    CHECK(!seq.Unchanged(cross));
    CHECK(seq.Unchanged(first));
    CHECK(!seq.Unchanged(second));
    CHECK(seq.Unchanged(third));
    CHECK(seq.Counter(base + 0x1ffff) == 1);
    CHECK(seq.Counter(base) == 0);

    // A new snapshot after the bump is unchanged again.
    const auto cross2 = seq.Snapshot(base + 0xff00, 0x200);
    CHECK(seq.Unchanged(cross2));

    // Bump outside the range (just before and just after): unchanged.
    seq.Bump(base - 1, 1);           // the block before `base`
    seq.Bump(base + 0x20000, 0x10);  // the block after the cross binding
    CHECK(seq.Unchanged(cross2));
    CHECK(!seq.Unchanged(third));

    // Bump spanning several blocks (unaligned on both ends) bumps each once.
    seq.Bump(base + 0x30010, 0x30000); // blocks 3,4,5,6
    for (u64 b = 3; b <= 6; ++b) {
        CHECK(seq.Counter(base + b * 0x10000) == 1);
    }
    CHECK(seq.Counter(base + 7 * 0x10000) == 0);

    // Zero size: Bump does nothing; a zero-size snapshot is always unchanged.
    const auto z = seq.Snapshot(base + 0x80000, 0);
    seq.Bump(base + 0x80000, 0);
    CHECK(seq.Counter(base + 0x80000) == 0);
    seq.Bump(base + 0x80000, 1);
    CHECK(seq.Unchanged(z));

    // A large binding (many blocks, across a 16 MiB leaf boundary) needs no heap either.
    const u64 leaf_edge = 0x1043000000; // a 16 MiB boundary
    const auto big = seq.Snapshot(leaf_edge - (8ull << 20), 16ull << 20);
    CHECK(seq.Unchanged(big));
    seq.Bump(leaf_edge + (7ull << 20), 4); // in the second leaf, never allocated before
    CHECK(!seq.Unchanged(big));
    const auto big2 = seq.Snapshot(leaf_edge - (8ull << 20), 16ull << 20);
    seq.Bump(leaf_edge + (8ull << 20), 4); // first byte past it
    CHECK(seq.Unchanged(big2));
    // A bump straddling the leaf boundary.
    const auto edge = seq.Snapshot(leaf_edge - 0x10, 0x20);
    seq.Bump(leaf_edge - 4, 8);
    CHECK(!seq.Unchanged(edge));
    CHECK(seq.Counter(leaf_edge - 1) == 1);
    CHECK(seq.Counter(leaf_edge) == 1);

    // Two bumps never cancel: counters only grow (sum equality == every counter equal).
    const auto two = seq.Snapshot(base + 0x40000, 0x20000);
    seq.Bump(base + 0x40000, 1);
    seq.Bump(base + 0x50000, 1);
    CHECK(!seq.Unchanged(two));

    // Outside the 1 TiB guest space: one shared "far" counter (conservative).
    const u64 far = u64{1} << 41;
    const auto f = seq.Snapshot(far, 0x1000);
    const auto f2 = seq.Snapshot(far + (1ull << 30), 0x1000);
    seq.Bump(far, 4);
    CHECK(!seq.Unchanged(f));
    CHECK(!seq.Unchanged(f2)); // shares the counter
    // A range that wraps the address space is clamped, not looped forever.
    seq.Bump(~u64{0} - 0x10, 0x100);
    const auto w = seq.Snapshot(~u64{0} - 0x10, 0x100);
    CHECK(seq.Unchanged(w));
    // A range from inside the space into the far area.
    const auto across = seq.Snapshot(Seq::SpaceEnd - 0x10000, 0x20000);
    seq.Bump(Seq::SpaceEnd + 0x5000, 1);
    CHECK(!seq.Unchanged(across));
}

static void Concurrent() {
    // Guest threads bump while "stage B" snapshots: every bump is counted, and a snapshot
    // taken before the bumps ends never compares unchanged after them.
    Seq seq;
    const u64 base = 0x1100000000;
    constexpr int Threads = 4;
    constexpr int PerThread = 100000;
    const auto before = seq.Snapshot(base, 64ull << 20);
    std::vector<std::thread> threads;
    for (int t = 0; t < Threads; ++t) {
        threads.emplace_back([&seq, t] {
            for (int i = 0; i < PerThread; ++i) {
                // All threads race on leaf creation for each of 4 leaves, and on block 0.
                seq.Bump(base + (u64(i % 4) << 24) + (u64(t) << 16), 1);
                seq.Bump(base, 1);
            }
        });
    }
    u64 snaps = 0;
    for (int i = 0; i < 1000; ++i) {
        (void)seq.Snapshot(base, 64ull << 20);
        ++snaps;
    }
    for (auto& th : threads) {
        th.join();
    }
    CHECK(!seq.Unchanged(before));
    const auto after = seq.Snapshot(base, 64ull << 20);
    CHECK(after.sum == u64(Threads) * PerThread * 2);
    CHECK(seq.Counter(base) >= u64(Threads) * PerThread);
    (void)snaps;
}

static void Benchmark() {
    Seq seq;
    // Populate a realistic working set: bumps across 256 MiB of the dmem arena.
    const u64 base = 0x1000000000;
    for (u64 a = 0; a < (256ull << 20); a += 0x9000) {
        seq.Bump(base + a, 8);
    }
    struct Case {
        const char* name;
        u64 size;
    } cases[] = {{"256 B", 0x100}, {"4 KiB", 0x1000}, {"64 KiB (2 blocks, unaligned)", 0x10000},
                 {"1 MiB", 1ull << 20}, {"16 MiB", 16ull << 20}};
    using Clock = std::chrono::steady_clock;
    double ns_64k = 0;
    for (const auto& c : cases) {
        // Many different addresses, like the bindings of a frame.
        constexpr int Bindings = 1024;
        std::vector<Seq::Snap> snaps(Bindings);
        std::vector<u64> addrs(Bindings);
        for (int i = 0; i < Bindings; ++i) {
            addrs[i] = base + ((u64(i) * 0x3c4d1) % (200ull << 20)) + 0x1230;
        }
        const int Rounds = c.size >= (16ull << 20) ? 4 : c.size >= (1ull << 20) ? 64 : 2048;
        u64 unchanged = 0;
        const auto t0 = Clock::now();
        for (int r = 0; r < Rounds; ++r) {
            for (int i = 0; i < Bindings; ++i) {
                if (r == 0 || !seq.Unchanged(snaps[i])) {
                    snaps[i] = seq.Snapshot(addrs[i], c.size);
                } else {
                    ++unchanged;
                }
            }
        }
        // Snapshot + Unchanged per op: time a pair explicitly as well.
        const auto t1 = Clock::now();
        for (int r = 0; r < Rounds; ++r) {
            for (int i = 0; i < Bindings; ++i) {
                const auto s = seq.Snapshot(addrs[i], c.size);
                unchanged += seq.Unchanged(s);
            }
        }
        const auto t2 = Clock::now();
        const double ops = double(Rounds) * Bindings;
        const double unch_ns =
            std::chrono::duration<double, std::nano>(t1 - t0).count() / ops;
        const double pair_ns =
            std::chrono::duration<double, std::nano>(t2 - t1).count() / ops;
        std::printf("bench %-30s Unchanged (hit path): %7.1f ns/op  Snapshot+Unchanged: %7.1f "
                    "ns/op (%llu)\n",
                    c.name, unch_ns, pair_ns, static_cast<unsigned long long>(unchanged & 1));
        if (c.size == 0x10000) {
            ns_64k = pair_ns;
        }
    }
    // Target < 50 ns; fail only if grossly off (a loaded machine can be slower).
    CHECK(ns_64k < 500.0);
}

int main() {
    Correctness();
    Concurrent();
    Benchmark();
    if (g_failures == 0) {
        std::printf("test_cpu_write_seq: OK\n");
        return 0;
    }
    std::printf("test_cpu_write_seq: %d failure(s)\n", g_failures);
    return 1;
}
