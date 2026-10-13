# Whole-draw memo, W4: the memo table

Brief: `draw-memo.md` section 5 (W4), shape in section 2, verify mode in section 3.

## What was built

- `third_party/shadps4/src/video_core/renderer_vulkan/sotc_draw_memo.h` (new, header-only,
  Vulkan-free, logging-free).
- `gpu/tests/test_draw_memo.cpp` (new): unit test plus a hit-cost microbenchmark.
- `gpu/CMakeLists.txt`: registers `test_draw_memo` next to `test_write_ticks`.

Integration still has to add a row for `sotc_draw_memo.h` to `third_party/shadps4/README.md`
(that file is shared, so this unit did not touch it). The row is needed once a layer includes
the header.

## API

```cpp
namespace Vulkan {
// Switches
enum SotcMemoLayer : u32 { SotcMemoL1 = 1, SotcMemoL2 = 2, SotcMemoL3 = 4, SotcMemoL4 = 8, SotcMemoAll = 0xFF };
u32  SotcParseDrawMemo(const char* s, int* bad = nullptr); // "L1,L2,L4", "all"/"1", ""/"0"/"none"/"off"
bool SotcParseFlag(const char* s);                         // unset, "" and "0" are off
struct SotcDrawMemoConfig { u32 layers; bool verify; int bad_tokens; bool On(u32) const; std::string Describe() const; };
const SotcDrawMemoConfig& SotcDrawMemoEnv();               // SOTC_DRAW_MEMO + SOTC_DRAW_MEMO_VERIFY, read once

// Table: Key is trivially copyable (memcmp over its first key_len bytes), N generations
template <class Key, class Payload, size_t N, unsigned Bits = 10>
class SotcDrawMemo {
    using Gens = std::array<u64, N>;
    SotcDrawMemo(const char* name, bool (*same)(const Payload&, const Payload&) = nullptr);
    static u32 SlotOf(const void* owner);                  // SotcTexMemo's Info* hash
    static u32 SlotOfKey(const Key&, u32 key_len = sizeof(Key));
    const Payload* Lookup(u32 slot, const Key&, const Gens&, u32 key_len = sizeof(Key));
    const Payload* Lookup(u32 slot, const Key&, const Gens&, u32 key_len, Valid&& valid); // validator
    Payload& Store(u32 slot, const Key&, const Gens&, const Payload& / Payload&&, u32 key_len = sizeof(Key));
    void Invalidate(u32 slot);  void Clear();              // Clear is O(1) (epoch)
    bool Verify(const Payload* hit, const Payload& fresh, std::string (*describe)(hit, fresh) = nullptr);
    const SotcMemoStats& Stats() const;  const std::string& FirstMismatch() const;
    bool LogDue(u64 mask = 0xFFFFF) const;  std::string LogLine() const;  void ResetStats();
};
}
```

- **A hit** means the key bytes match (same `key_len`, then a memcmp), every generation is
  equal, and the validator, if given, returns true. Each miss is counted under one reason:
  `cold` (empty or cleared slot), `key` (the slot holds another key), `stale` (a generation
  changed) or `rejected` (the validator said no). A `Store` that replaces a live entry holding
  another key counts as an `evict`.
- **Variable-length keys.** A stage's V#s are passed as one fixed-capacity struct plus the
  length in use. Bytes past `key_len` are never read or hashed. Padding inside the used prefix
  must be zero: value-initialise the key (`Key k{}`).
- **The hit path does no allocation.** The table is allocated once, in the constructor.
  `Store` copies into its slot, so a payload that owns heap memory may allocate there, but
  that only happens on a miss.
- **`LogLine()`** prints `"<name> memo: H hits, M misses (cold, key, stale, rejected; evicts)"`,
  then `", V verified, X mismatches"` once `Verify` has run. `LogDue()` follows SotcTexMemo's
  cadence (every 2^20 lookups). The caller does `LOG_INFO(Render_Vulkan, "{}", m.LogLine());`
  and then `m.ResetStats()`.
- **`Verify`.** It is called only after a hit, with the payload from the full path. A mismatch
  is counted, and the first one is kept: name, hit count, slot, key in hex (8-byte groups,
  separated by dots), generations, plus the optional `describe` text. The entry is then
  healed with the fresh payload. `FirstMismatch()` survives `ResetStats()`. The caller logs
  it once, when it first becomes non-empty.
- **Threads.** The table is meant for one thread (stage B), like SotcTexMemo, and has no
  locks.

## Test and benchmark results

`ctest --test-dir build-vk -R test_draw_memo` passes. It covers:
- hits and misses, with each miss reason
- bytes past `key_len` being ignored
- a generation change invalidating an entry, and the old generation hitting again
- validator rejection
- a forced collision evicting an entry
- `Invalidate` and `Clear`
- the `SlotOfKey` range and that it ignores the tail
- the `LogLine` format
- verify: no mismatch, a mismatch (key hex, gens, describe text), healing, the first record
  being kept, and the verified/mismatch counts
- a table with `N == 0`
- parsing: null, `""`, `0`, `none`, `L1`, `L1,L2,L4`, `l2; L3`, `,L4,,`, `all`, `ALL`, `1`,
  and unknown tokens (`L9`, `foo`, `2`, an over-long token)
- `Describe`

Hit-cost microbenchmark. Setup: 64 stages, each in its own slot, looked up round-robin with
`Gens` = 2 and a W5-sized payload (16 bindings). The time includes the slot computation. The
numbers are the best of 3 runs of 20M lookups on a Ryzen 5 3600 with a RelWithDebInfo
clang-cl build:

| key | slot by owner pointer | slot by key hash |
|---|---|---|
| 64 bytes | 2.0 ns | 6.0 ns |
| 256 bytes | 14.7 ns | 26.5 ns |

Why the 256-byte key costs more:
- It is mostly cache. 64 entries of about 720 bytes, plus 16 KB of keys, is more than L1. The
  memcmp itself is around 5 ns, which a run where colliding owners shared slots happened to
  show.
- Hashing 256 bytes adds about 12 ns. A serial multiply-xorshift chain cost about 59 ns for 256
  bytes. It was replaced by independent 128-bit folded multiplies (`unsigned __int128`, which
  clang supports on both platforms).

Either way, a hit costs well under 0.1 us, against B's ~5.8 us per record. When a stable owner
pointer exists, use `SlotOf(Info*)`.

## Recommended usage (W5 buffers, W6 textures)

Pattern, in the shape SotcTexMemo already uses:

```cpp
// member of Rasterizer (B only):
SotcDrawMemo<BufKey, BufPayload, 2> sotc_buf_memo{"buffer", &SameBuf};

const auto& cfg = SotcDrawMemoEnv();
if (cfg.On(SotcMemoL1)) {
    BufKey key{};                                    // zeroed: padding and the unused tail
    key.stage = &stage; key.pgm_hash = stage.pgm_hash;
    u32 n = 0; for (auto& desc : stage.buffers) key.vsharps[n++] = desc.GetSharp(stage);  // via Info, never members
    const u32 len = offsetof(BufKey, vsharps) + n * sizeof(key.vsharps[0]);
    const auto slot = sotc_buf_memo.SlotOf(&stage);
    const BufMemo::Gens gens{buffer_cache.SotcGeneration(), /*e.g.*/ sotc_note_gen};
    auto valid = [&](const BufPayload& p) { return cpu_seq.Unchanged(p.snap, p.snap_count); };
    if (sotc_buf_memo.LogDue()) { LOG_INFO(..., "{}", sotc_buf_memo.LogLine()); sotc_buf_memo.ResetStats(); }
    if (const BufPayload* hit = sotc_buf_memo.Lookup(slot, key, gens, len, valid)) {
        if (!cfg.verify) { ReplayHit(*hit); goto done; }  // side effects that must still happen
        BufPayload fresh = FullPath(); sotc_buf_memo.Verify(hit, fresh, &DescribeBuf); ...
    } else {
        BufPayload fresh = FullPath();               // records snap[] as it goes
        if (cacheable) sotc_buf_memo.Store(slot, key, gens, std::move(fresh), len);
    }
}
```

- **Key.**
  - Put identity in it: the owner `Info*`, `pgm_hash` and the V# words.
  - Ring bindings (W3) go in as ring id + offset-independent fields, not the address,
    otherwise a moving ring never hits.
  - Keep it fixed-capacity, with `key_len` covering what is in use. The 16-V# case is
    16 + 256 bytes, so put the V#s that change most often first. memcmp exits early either way.
- **Generations.** Use cheap global counters that move whenever any cached answer could
  change:
  - the buffer cache's register/unregister/resize generation (W5 has to add it if
    `BufferCache` has none);
  - for L2, `texture_cache.SotcFindState().first` plus an image layout/access generation;
  - when a counter moves often (every frame), it belongs in the validator, not in `Gens`.
- **Validator: the extension point for W2's per-range CPU write sequence.** That sequence is
  variable-size, so it does not fit a fixed generation vector.
  - Store the snapshot inline in the payload, for example `std::array<u64, 4> snap` plus a
    count. A binding of 64 KiB or less touches 1-2 blocks.
  - The validator checks it with `Unchanged`.
  - A binding whose snapshot would not fit is not stored (or the store is marked "always
    rejected"), so the hit path stays allocation-free.
  - The validator runs only after the key and generations match, so a W2 lookup (one hash per
    block) is paid only on candidates that would otherwise hit.
  - Rejections show as `rejected` in the log. A high `rejected` count means real CPU writes,
    and that is the case where W3's ring mapping matters.
- **Payload.**
  - For L1, per binding: `BufferId`, offset and size. Also keep whatever `BindBuffers` needs
    to rebuild the descriptor and the `buffer_infos` entry without `ObtainBuffer`, plus the
    snapshot.
  - Use value types only: no pointers into B's reused storage (CLAUDE.md, the session-62
    rule).
  - For L2, the per-binding view/sampler ids and the layout/access the second pass computed.
- **Hit replay.** SotcTexMemo keeps the side effects of the skipped work, and W5/W6 must too:
  - the written-binding notes (`SotcNoteGpuWrite`, `SotcPipeNoteWrites` on A);
  - residency/LRU touches;
  - `force_general` / `is_bound`.

  The verify payload has to cover what those side effects depend on.
- **Invariants for CLAUDE.md**, to add when W5/W6 land: anything that changes what a cached
  binding resolves to bumps the layer's generation. If that cannot be done, it calls `Clear()`
  on the memo. One example is a buffer cache GC that frees a `BufferId` that might be reused.
- **Verify.** It must stay clean in the altar, the intro and the first colossus before a layer
  is kept. Compare the full payload, including the snapshot. `FirstMismatch()` should be logged
  once, at its first non-empty value, with `LOG_WARNING`.

## Open questions

- Is there a `BufferCache` generation for buffer creation, deletion or merge, or does W5 add
  one? A merge (`JoinOverlaps`) can change the `BufferId` of an address that was cached.
- Is `SotcFindState().first` enough for L2, or does the second pass also depend on image
  layout/access state? That needs a separate counter, or the validator.
- The 2^20-lookup `LogDue` cadence copies SotcTexMemo. The probe logs every 10 s, so a
  time-based cadence may suit B better.
- `Bits = 10` (1024 slots), as in SotcTexMemo. The `evict` count will show whether L1 needs
  more slots.
- The parser accepts `1` as "all", like the other `SOTC_*=1` switches, so `SOTC_DRAW_MEMO=1`
  enables every layer. Is that the semantics we want, or should only `all` do that?
