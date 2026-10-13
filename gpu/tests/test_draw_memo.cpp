// SPDX-License-Identifier: GPL-2.0-or-later
// Whole-draw memo W4: Vulkan::SotcDrawMemo, the generic memo table (sotc_draw_memo.h), and the
// SOTC_DRAW_MEMO parsing. Ends with a hit-cost microbenchmark (64- and 256-byte keys).
#include <algorithm>
#include <array>
#include <cstddef>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "video_core/renderer_vulkan/sotc_draw_memo.h"

using namespace Vulkan;

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

// A W5-shaped key and payload: a stage's V# words (16 bytes each, up to 16) and per binding the
// buffer id, offset and size, plus an inline write-sequence snapshot for the validator.
struct BufKey {
    const void* stage;
    std::uint64_t pgm_hash;
    std::uint32_t vsharps[16][4];
};
struct BufBinding {
    std::uint32_t buffer_id;
    std::uint32_t pad;
    std::uint64_t offset;
    std::uint64_t size;
};
struct BufPayload {
    std::uint32_t count = 0;
    std::array<BufBinding, 16> bindings{};
    std::uint32_t snap_count = 0;
    std::array<std::uint64_t, 4> snap{}; // write-sequence values the bindings saw
};
static bool SameBuf(const BufPayload& a, const BufPayload& b) {
    return a.count == b.count &&
           std::memcmp(a.bindings.data(), b.bindings.data(), a.count * sizeof(BufBinding)) == 0;
}
static std::uint32_t KeyLen(std::uint32_t n_vsharps) {
    return static_cast<std::uint32_t>(offsetof(BufKey, vsharps) + n_vsharps * 16);
}

static void TestBasics() {
    SotcDrawMemo<BufKey, BufPayload, 2> memo("buffer", SameBuf);
    int stage_a = 0, stage_b = 0;
    BufKey key{};
    key.stage = &stage_a;
    key.pgm_hash = 0x1234;
    key.vsharps[0][0] = 0xdead0000;
    key.vsharps[0][2] = 0x100;
    const std::uint32_t len = KeyLen(1);
    const auto slot = memo.SlotOf(&stage_a);
    std::array<std::uint64_t, 2> gens{7, 9};

    CHECK(memo.Lookup(slot, key, gens, len) == nullptr);
    CHECK(memo.Stats().miss_cold == 1);
    BufPayload p;
    p.count = 1;
    p.bindings[0] = {42, 0, 0x10, 0x100};
    memo.Store(slot, key, gens, p, len);
    const BufPayload* hit = memo.Lookup(slot, key, gens, len);
    CHECK(hit && hit->count == 1 && hit->bindings[0].buffer_id == 42);
    CHECK(memo.Stats().hits == 1);

    // Bytes past key_len are never compared.
    BufKey key2 = key;
    key2.vsharps[5][1] = 0xffff;
    CHECK(memo.Lookup(slot, key2, gens, len) != nullptr);
    // A different key length is a different key.
    CHECK(memo.Lookup(slot, key, gens, KeyLen(2)) == nullptr);
    CHECK(memo.Stats().miss_key == 1);
    // A changed V# word misses.
    key2 = key;
    key2.vsharps[0][2] = 0x200;
    CHECK(memo.Lookup(slot, key2, gens, len) == nullptr);
    CHECK(memo.Stats().miss_key == 2);

    // Generation change invalidates; going back to the stored generation hits again.
    std::array<std::uint64_t, 2> gens2{7, 10};
    CHECK(memo.Lookup(slot, key, gens2, len) == nullptr);
    CHECK(memo.Stats().miss_stale == 1);
    CHECK(memo.Lookup(slot, key, gens, len) != nullptr);

    // Validator: same key and gens, but the snapshot moved.
    memo.Store(slot, key, gens, p, len).snap = {5};
    std::uint64_t seq = 5;
    auto valid = [&](const BufPayload& q) { return q.snap[0] == seq; };
    CHECK(memo.Lookup(slot, key, gens, len, valid) != nullptr);
    seq = 6;
    CHECK(memo.Lookup(slot, key, gens, len, valid) == nullptr);
    CHECK(memo.Stats().miss_rejected == 1);

    // Collision / eviction: another stage forced into the same slot replaces the entry.
    BufKey other = key;
    other.stage = &stage_b;
    memo.Store(slot, other, gens, p, len);
    CHECK(memo.Stats().evicts == 1);
    CHECK(memo.Lookup(slot, key, gens, len) == nullptr);
    CHECK(memo.Lookup(slot, other, gens, len) != nullptr);
    // Storing the same key again is not an eviction.
    memo.Store(slot, other, gens, p, len);
    CHECK(memo.Stats().evicts == 1);

    // Invalidate(slot) and Clear().
    memo.Invalidate(slot);
    CHECK(memo.Lookup(slot, other, gens, len) == nullptr);
    memo.Store(slot, other, gens, p, len);
    memo.Clear();
    CHECK(memo.Lookup(slot, other, gens, len) == nullptr);
    memo.Store(slot, other, gens, p, len);
    CHECK(memo.Lookup(slot, other, gens, len) != nullptr);

    // Slot-by-key hashing is deterministic and in range; it only reads key_len bytes.
    CHECK(memo.SlotOfKey(key, len) == memo.SlotOfKey(key2.vsharps[0][2] ? key : key, len));
    BufKey k3 = key;
    k3.vsharps[9][0] = 1;
    CHECK(memo.SlotOfKey(key, len) == memo.SlotOfKey(k3, len));
    CHECK(memo.SlotOfKey(key, len) < memo.Slots);

    // Log line format.
    const std::string line = memo.LogLine();
    CHECK(line.rfind("buffer memo: ", 0) == 0);
    std::printf("%s\n", line.c_str());
    memo.ResetStats();
    CHECK(memo.Stats().hits == 0 && memo.Stats().misses == 0);
    CHECK(memo.LogLine().rfind("buffer memo: 0 hits, 0 misses", 0) == 0);
}

static void TestVerify() {
    SotcDrawMemo<BufKey, BufPayload, 1> memo("buffer", SameBuf);
    int stage = 0;
    BufKey key{};
    key.stage = &stage;
    key.vsharps[0][0] = 0xabcdef01;
    const auto len = KeyLen(1);
    const auto slot = memo.SlotOf(&stage);
    std::array<std::uint64_t, 1> gens{3};
    BufPayload p;
    p.count = 1;
    p.bindings[0] = {1, 0, 0, 64};
    memo.Store(slot, key, gens, p, len);

    const BufPayload* hit = memo.Lookup(slot, key, gens, len);
    CHECK(memo.Verify(hit, p));
    CHECK(memo.FirstMismatch().empty());

    BufPayload fresh = p;
    fresh.bindings[0].buffer_id = 2;
    hit = memo.Lookup(slot, key, gens, len);
    CHECK(!memo.Verify(hit, fresh, [](const BufPayload& a, const BufPayload& b) {
        return "buffer " + std::to_string(a.bindings[0].buffer_id) + " vs " +
               std::to_string(b.bindings[0].buffer_id);
    }));
    CHECK(memo.Stats().mismatches == 1 && memo.Stats().verified == 2);
    const std::string& m = memo.FirstMismatch();
    std::printf("%s\n", m.c_str());
    CHECK(m.find("buffer memo: verify mismatch #1") == 0);
    CHECK(m.find("01efcdab") != std::string::npos); // the V# word, little-endian bytes
    CHECK(m.find("buffer 1 vs 2") != std::string::npos);
    CHECK(m.find("gens 3") != std::string::npos);
    // Healed: the next hit returns the fresh payload and verifies clean.
    hit = memo.Lookup(slot, key, gens, len);
    CHECK(hit && hit->bindings[0].buffer_id == 2);
    CHECK(memo.Verify(hit, fresh));
    // A later mismatch keeps the first record.
    BufPayload fresh2 = fresh;
    fresh2.count = 0;
    CHECK(!memo.Verify(memo.Lookup(slot, key, gens, len), fresh2));
    CHECK(memo.FirstMismatch() == m);
    CHECK(memo.Stats().mismatches == 2);
    CHECK(memo.LogLine().find("4 verified, 2 mismatches") != std::string::npos);
    // N == 0 tables work too.
    SotcDrawMemo<std::uint64_t, int, 0, 4> tiny("tiny");
    tiny.Store(3, 99, {}, 5);
    CHECK(tiny.Lookup(3, 99, {}) && *tiny.Lookup(3, 99, {}) == 5);
    CHECK(tiny.Lookup(3, 98, {}) == nullptr);
}

static void TestParse() {
    int bad = -1;
    CHECK(SotcParseDrawMemo(nullptr) == 0);
    CHECK(SotcParseDrawMemo("") == 0);
    CHECK(SotcParseDrawMemo("0") == 0);
    CHECK(SotcParseDrawMemo("none") == 0);
    CHECK(SotcParseDrawMemo("L1") == SotcMemoL1);
    CHECK(SotcParseDrawMemo("L1,L2,L4") == (SotcMemoL1 | SotcMemoL2 | SotcMemoL4));
    CHECK(SotcParseDrawMemo("l2; L3") == (SotcMemoL2 | SotcMemoL3));
    CHECK(SotcParseDrawMemo(",L4,,") == SotcMemoL4);
    CHECK(SotcParseDrawMemo("all") == SotcMemoAll);
    CHECK(SotcParseDrawMemo("ALL") == SotcMemoAll);
    CHECK(SotcParseDrawMemo("1") == SotcMemoAll);
    CHECK(SotcParseDrawMemo("L1,L9,foo,2", &bad) == SotcMemoL1 && bad == 3);
    CHECK(SotcParseDrawMemo("averyveryverylongtoken,L2", &bad) == SotcMemoL2 && bad == 1);
    CHECK(!SotcParseFlag(nullptr) && !SotcParseFlag("") && !SotcParseFlag("0"));
    CHECK(SotcParseFlag("1"));
    auto c = SotcParseDrawMemoConfig("L1,L2", "1");
    CHECK(c.On(SotcMemoL1) && c.On(SotcMemoL2) && !c.On(SotcMemoL4) && c.verify);
    std::printf("%s\n", c.Describe().c_str());
    CHECK(c.Describe() == "draw memo: L1,L2 (verify, SOTC_DRAW_MEMO_VERIFY=1)");
    CHECK(SotcParseDrawMemoConfig(nullptr, nullptr).Describe() == "draw memo: off");
    // The environment reader is callable (whatever the variables are here).
    (void)SotcDrawMemoEnv().layers;
}

// Hit cost: `stages` stages, each with its own slot and key, looked up round-robin.
template <std::size_t KeyBytes>
struct BenchKey {
    std::uint8_t bytes[KeyBytes];
};
template <std::size_t KeyBytes, bool KeyHashSlot>
static double BenchHit(std::uint64_t iters) {
    using Key = BenchKey<KeyBytes>;
    SotcDrawMemo<Key, BufPayload, 2> memo("bench", SameBuf);
    constexpr int stages = 64;
    struct Owner { // Shader::Info-sized owners, like the stages a real caller keys by
        std::uint8_t bytes[1024];
    };
    std::vector<Owner> owners(stages);
    std::vector<Key> keys(stages);
    std::vector<std::uint32_t> slots(stages);
    std::vector<bool> used(memo.Slots);
    const std::array<std::uint64_t, 2> gens{11, 13};
    for (int i = 0; i < stages; ++i) {
        for (std::size_t b = 0; b < KeyBytes; ++b) {
            keys[i].bytes[b] = static_cast<std::uint8_t>(b * 7 + i);
        }
        // Distinct slots (a benchmark of hits, not of conflicts): perturb the last key byte
        // until the hash slot is free; owner pointers 1 KiB apart may still collide, then the
        // test reports it.
        for (std::uint8_t t = 0; KeyHashSlot && used[memo.SlotOfKey(keys[i])]; ++t) {
            keys[i].bytes[KeyBytes - 1] = t;
        }
        slots[i] = KeyHashSlot ? memo.SlotOfKey(keys[i]) : memo.SlotOf(&owners[i]);
        used[slots[i]] = true;
        BufPayload p;
        p.count = 4;
        memo.Store(slots[i], keys[i], gens, p);
    }
    std::uint64_t sink = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (std::uint64_t n = 0; n < iters; ++n) {
        const int i = static_cast<int>(n & (stages - 1));
        // The slot computation is part of the cost (a real caller does it per stage).
        const auto slot = KeyHashSlot ? memo.SlotOfKey(keys[i]) : memo.SlotOf(&owners[i]);
        const BufPayload* hit = memo.Lookup(slot, keys[i], gens);
        sink += hit ? hit->count : 1000;
    }
    const auto t1 = std::chrono::steady_clock::now();
    if (sink != iters * 4 || memo.Stats().hits != iters) {
        std::printf("FAIL bench: not all hits (%llu)\n", (unsigned long long)sink);
        ++g_failures;
    }
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / double(iters);
}

int main() {
    TestBasics();
    TestVerify();
    TestParse();

    const std::uint64_t iters = 20'000'000;
    double best[4] = {1e9, 1e9, 1e9, 1e9};
    for (int r = 0; r < 3; ++r) { // best of 3
        best[0] = std::min(best[0], BenchHit<64, false>(iters));
        best[1] = std::min(best[1], BenchHit<256, false>(iters));
        best[2] = std::min(best[2], BenchHit<64, true>(iters));
        best[3] = std::min(best[3], BenchHit<256, true>(iters));
    }
    std::printf("hit cost, slot by owner pointer: 64-byte key %.2f ns, 256-byte key %.2f ns\n",
                best[0], best[1]);
    std::printf("hit cost, slot by key hash:      64-byte key %.2f ns, 256-byte key %.2f ns\n",
                best[2], best[3]);

    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("test_draw_memo: all passed\n");
    return 0;
}
