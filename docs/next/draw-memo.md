# Whole-draw memo: skip what stage B already resolved (plan, after session 66; runs before shape D step 3b item 2)

Stage B spends ~5.8 us per record on Windows (~7,200 records per flip at the altar, ~42 ms/flip),
where a native renderer pays well under 1 us per draw. Session 59 (`draw-prep.md` section 2)
found that **92.7% of records change nothing in the caches**: B resolves the same buffers,
images and render state as last time. This plan caches those results per layer, so that a draw
whose inputs didn't change skips the lookups.

## 1. Lessons from the memos we already have

| memo | session | hits | gain |
|---|---|---|---|
| render state (`PrepareRenderState`) | 38 | 81.7% | noise (-0.9% B) |
| texture set (first pass of `BindTextures`) | 40 | ~82-92% | kept |
| written-binding notes | 48 | 74% skipped | kept |
| descriptor sets (`SOTC_DESC_CACHE=1`) | 55 | 99.9% | **none** on NVIDIA (43.9 vs 43.3 ms/flip) |

**A high hit rate is worth nothing unless the skipped work is the expensive part.** Session 55
cached what was already cheap: NVIDIA's push descriptors. So every layer below is judged by the
**time its hits would save** (session 59's per-phase costs on the draws that would hit), never by
hits alone. The probe (W1) measures exactly that before anything is built.

## 2. Shape: one key, one generation and one payload per layer

A hit is "key bytes equal and generation equal": a memcmp plus a few compares. Existing model:
`SotcTexMemo` (`vk_rasterizer.h`), keyed by stage + T# words, generation `SotcFindState()`.

| layer | key | generation | a hit skips | B cost (session 59) |
|---|---|---|---|---|
| L1 buffers | V# words per stage (ring bindings as ring id + size, see 3) | buffer-cache generation + per-range CPU write sequence | `ObtainBuffer` (residency, tracker sync, write notes stay) | buf ~1,936 ms/10 s, the largest |
| L2 textures, second pass | the first-pass result (already memoized) | find state + image layout/access state | views, samplers, transitions, descriptor building | part of tex ~1,157 |
| L3 render state + `BeginRendering` | already memoized (session 38) | - | extend to `beginrt` only if W1 shows time | rstate 501 + beginrt 185 |
| L4 barriers | the bound image/buffer ids and their access state | access-state generation | barrier collection | part of barr 492 |
| (dropped) descriptor sets | - | - | - | session 55: no gain |

The buffer layer is the main target. The hard part is that the CPU rewrites guest memory every frame:

- **A per-range CPU write sequence** (`sotc_cpu_write_seq.h`, new): a counter per 64 KiB, bumped
  wherever the memory tracker marks pages CPU-dirty. A cached binding keeps the counters of the
  blocks it covers. Unchanged means skip; changed takes today's path for that binding only. Shape
  it like `SotcWriteTicks` (blocks of contiguous counters, one hash lookup per block).
- **Rings.** Per-draw constants usually come from a frame ring, so a V# address moves every frame
  and a key holding the address never matches. W3 finds Bluepoint's rings (statically from the
  eboot, then confirmed by W1's runtime classification). A ring becomes one resident buffer: a
  binding into it is ring base + offset, plus the copy of the bytes the CPU wrote.

## 3. Correctness

- **The invariants in `CLAUDE.md` are the generations.** Every new layer adds its own rule there
  (for example "anything that marks pages CPU-dirty bumps the write sequence").
- **Verify mode, `SOTC_DRAW_MEMO_VERIFY=1`.** A hit also runs the full path, compares the results
  and logs the first mismatch with its layer and key. A layer is kept only when verify is clean
  in the altar, the intro and the first colossus, and the alternating measurement
  (`flipcpu.py`, B ms/flip) shows a gain.
- Each layer gets its own switch (`SOTC_DRAW_MEMO=L1,L2,...`), so a bug bisects to one layer.

## 4. Work units and waves (parallel plan)

Rules (private `CLAUDE.private.md`, "Parallel work"):
- Each agent works in its own worktree and starts with `git reset --hard <local HEAD>`.
- Each agent creates only the files it owns, plus `docs/next/draw-memo-<ws>.md`.
- One build at a time (`-j 6`).
- Only the main session runs the GPU.
- `vk_rasterizer.cpp`, `buffer_cache.{h,cpp}` and `memory_tracker.h` are shared. Agents
  don't edit them: integration does.

| id | unit | owns (new files) | depends on |
|---|---|---|---|
| W1 | Probe `SOTC_DRAW_MEMO_PROBE=1`: per-layer key hashes per record and would-hit counts vs the last record of the same stage, rdtsc time of each phase split hit/miss, plus the buffer-address classification (static, moving with a fixed stride, i.e. a ring candidate, one-shot) | `renderer_vulkan/sotc_memo_probe.{h,cpp}` + a hook diff `docs/next/draw-memo-probe.diff` (not applied) | - |
| W2 | CPU write sequence: `Bump(addr, size)`, `Snapshot(addr, size)`, `Unchanged(snap)`; block storage like `SotcWriteTicks`; a unit test; the list of tracker sites that mark CPU-dirty (where integration bumps it) | `buffer_cache/sotc_cpu_write_seq.h`, `gpu/tests/test_cpu_write_seq.cpp` | - |
| W3 | Ring recon (static): find the engine's frame/constant ring allocators in the eboot (the libGnm/renderer address map from the 2026-10-13 recon is in the private notes), their base/size/wrap logic, and how V#s into them are built. Deliver addresses plus a Vulkan-free `SotcRingMap` (address → ring id, offset) with a test | `buffer_cache/sotc_ring_map.h`, `gpu/tests/test_ring_map.cpp`; addresses go in the **private** notes (game-derived) | - |
| W4 | Memo table: a direct-mapped table (key bytes, generation vector, payload), hit/miss counters, a verify hook, `SOTC_DRAW_MEMO` parsing; generalise `SotcTexMemo`'s pattern; a unit test | `renderer_vulkan/sotc_draw_memo.h`, `gpu/tests/test_draw_memo.cpp` | - |
| I1 | Main: apply W1's hooks, run the probe (altar + intro, Pro mode), write the results | `vk_rasterizer.cpp` (shared) | W1 |
| W5 | L1 buffer layer in `BindBuffers` behind `SOTC_DRAW_MEMO=L1` + verify | `vk_rasterizer.cpp`, `buffer_cache.cpp` | I1 (go/no-go), W2, W4, W3 if I1 shows rings matter |
| W6 | L2 texture second pass in `BindTextures` | `vk_rasterizer.cpp` | I1, W4; after W5 (same file) |
| W7 | L4 barriers | `vk_rasterizer.cpp`, `tile_manager`/runtime barrier code | I1, W4; after W6 |

```
Wave 1 (parallel agents): W1, W2, W3, W4     <- Vulkan-free or hook-diff only; no shared file edits
Wave 2 (main session):    I1                  <- applies W1's diff, GPU runs, decides L1/L2/L4 order
Wave 3 (one per session): W5 -> W6 -> W7      <- same file (vk_rasterizer.cpp): sequential, each measured
```

**Go/no-go after I1.** A layer gets built only if its would-hit draws spend at least ~3 ms/flip
in the phases it skips. If L1's misses are mostly moving addresses, W3's ring map goes first
inside W5. If they are mostly real CPU writes, W2 does the work.

**Expected.** If L1 and L2 skip most of buf and tex on hits, B drops by up to ~15-20 ms/flip.
That is an upper bound, not a measurement. A (~25 ms/flip) then becomes the limit.

## 5. Agent briefs (wave 1)

Each one first runs `git reset --hard <local HEAD sha>`, checks it with `git log -1`, reads
`CLAUDE.md` and this file, builds `build-vk` (`-j 6`, behind the build lock), runs its own unit
test and writes `docs/next/draw-memo-<id>.md` (what was built, API, open questions). No GPU runs.

- **W1 (probe).** Model it on `draw-prep-probe.diff` (session 59: rdtsc laps in
  `BindBuffers`/`BindTextures`/`PrepareRenderState`) and `SOTC_INDIRECT_PROBE` (session 66: a
  log line every 10 s). Per record kind and per layer, log: would-hit %, the hits' summed phase
  time and the misses'. Key per layer:
  - L1: the stage's V# words.
  - L2: T# + S# words.
  - L3: the RT/DB registers via `Regs()`.

  Compare against the last record with the same `Shader::Info*` (direct-mapped like
  `SotcTexMemo`). For buffers, also classify each binding's base address over 10 s. Never
  read `liverpool->regs` or the stage members directly (see `CLAUDE.md`). Deliver the new files
  plus `docs/next/draw-memo-probe.diff` with the hook lines in `vk_rasterizer.cpp`, checked by
  applying it, building and reverting.
- **W2 (CPU write sequence).** Vulkan-free header, GPU-thread-only like `SotcWriteTicks`. List
  each place in `memory_tracker.h`/`buffer_cache.cpp`/`page_manager.cpp` that marks pages
  CPU-dirty, or clears that state, and say where integration must call `Bump`. Snapshot cost
  target: under 50 ns for a binding of 64 KiB or less.
- **W3 (ring recon).** Python only, plus the small header. Use the scan scripts kept in the
  private notes (`sotc-pc-notes/tools/rhi_scan/`). The starting points are listed in
  `sotc-pc-notes/history/draw-memo-rings.md`: the engine functions with the most calls into
  libGnm, the most-called libGnm entry point (probably command-buffer space allocation), and
  code that builds V# words (stride and num_records fields). Write the addresses and logic **only** in
  `sotc-pc-notes/history/draw-memo-rings.md` (game-derived, never in the public repo).
- **W4 (memo table).** Templated over the key, payload and generation count; no allocation on
  the hit path; verify hook `bool (*same)(const Payload&, const Payload&)`; counters logged like
  `texture-set memo: H hits, M misses`.

## 6. Estimated time savings

- **Sequentially:** W1, W2, W3, W4, I1, W5, W6 and W7 come to about 8 steps.
- **Planned:** wave 1 is 4 units in one step, then I1, then W5, W6 and W7, so about 5 steps.
- The saving comes from wave 1. W5, W6 and W7 stay sequential because they share
  `vk_rasterizer.cpp` and each needs its own GPU measurement.

## 7. Wave 1 results (session 67)

All four units are in, as new files only; no upstream file has hooks yet. Unit tests pass
(`test_draw_memo`, `test_cpu_write_seq`, `test_ring_map`), and the full `build-vk` builds with the
probe's hook diff applied (checked by W1, then reverted). Details: `draw-memo-W1.md` to `-W4.md`.

- **W1, probe** (`sotc_memo_probe.{h,cpp}`, `draw-memo-probe.diff`, 35 hook lines in
  `vk_rasterizer.cpp`, `buffer_cache.cpp`, `texture_cache.cpp`, `sotc_pipe.cpp`, `liverpool.cpp`).
  Every 10 s B logs `probe: draw-memo:` lines. **The go/no-go input is the `summary` line**: the
  ms/flip that would-hit calls spend in each layer's phase, with a "clean" value (no stream copy,
  buffer upload or image upload in the phase). A layer is worth building at >= ~3 ms/flip clean.
  Also logged: the buffer-binding classes (static, multi, ring-fixed, ring-var, irregular) and the
  top ring regions/slots. The probe slows B: never compare frame rates with it on.
- **W2, CPU write sequence** (`sotc_cpu_write_seq.h`). **It can't be B-only:** pages turn
  CPU-modified on the faulting thread (guest threads, A, unmap), so the counters are atomic. It's a
  two-level table (no hash, no lock), and Snapshot + Unchanged costs ~4 ns for a binding up to 64 KiB.
  Integration: three `Bump`s inside `MemoryTracker` (`InvalidateRegion`, `MarkRegionAsCpuModified`,
  `UnmarkRegionAsGpuModified(is_write)`), each **after** the CPU bit is set. The snapshot is taken
  **before** the `SynchronizeMemory` that covers the binding. Stream-path and texel-buffer
  results must never be memoized. W2 suggests that `IsRegionCpuModified` on the binding
  alone may do the same job with no bump sites: W5 times both.
- **W3, rings** (`sotc_ring_map.h`; findings with addresses in the private notes). **There is no
  contiguous ring.** Dynamic constants are "map-discard" allocations from a per-frame tagged heap
  made of per-thread 1 MiB bump pages that come back every 2 frames (another heap: 5). Only the V#
  base changes; stride, num_records and dword3 stay. So ring regions must be **learned at run
  time** (`SotcRingMap::Register` merges pages with the same id). The CPU rewrites them every
  frame, so a memo can't skip their content, only resolve them cheaply. Per-draw constants may
  also reach shaders as SRT pointer reads (the walker or flat reads) rather than V#s: the probe's
  L1 base classes and "other change" share will show it.
- **W4, memo table** (`sotc_draw_memo.h`). `SotcDrawMemo<Key, Payload, N>`: memcmp key,
  generation array, an optional validator (where W2's snapshot goes), verify mode with the first
  mismatch kept, and `SOTC_DRAW_MEMO` parsing. A hit costs 2 ns with a 64-byte key and ~15 ns with
  a 256-byte key (slot by `Info*`). **Open for W5: `BufferCache` has no create/delete/merge
  generation** (a `JoinOverlaps` merge changes an address's BufferId); W5 adds one.

## 8. I1: the probe's numbers (session 67)

The probe's hooks are in (`SOTC_DRAW_MEMO_PROBE=1`, off by default). One run, Pro mode, new game
through the intro (skipped by hand: the rebuilt exe's cold NVIDIA shader cache delayed the
title, so the pad script's presses came too early) to the altar. Windows of 10 s; the altar's
seven windows agree within ~5%. The probe's own cost (~4 ms/flip at the altar) is subtracted.

**What the would-hit calls spend in the phase each layer skips, ms/flip:**

| scene | records/flip | in records | L1 buf | L2 tex | L3 rstate+beginrt | L4 barr | all layers hit |
|---|---|---|---|---|---|---|---|
| altar | ~7,100 | ~43 | **3.3** | **3.9** | 1.8 | 1.0 | 13.9-15.1 |
| intro, busiest | 3,300-3,500 | ~21 | 1.8-1.9 | 2.5-2.9 | 1.1-1.2 | 0.4-0.6 | 8.5 |
| intro, light | 1,400-2,300 | 12-17 | 0.6-1.1 | 1.3-1.9 | 0.4-0.9 | 0.1-0.3 | 2.9-5.5 |

"Clean" (no stream copy, buffer upload or image upload in the phase) is within 0.05 of these.
These are upper bounds: a hit still pays the key compare, the written-binding notes, residency
touches and the descriptor writes.

Per kind at the altar:
- **Indirect draws** (~3,500/flip): 73% of records hit in every layer.
  - L1 hits 85%, 2.1 ms/flip at 0.35 us per call.
  - L2 hits 96%, 2.1 ms/flip.
  - L3 hits 99.7%, 1.7 ms/flip.
- **Dispatches** (~2,300/flip): L1 hits only 48%.
  - **Their misses cost 4.1 ms/flip at 3.4 us per call**, about 6x a hit.
  - Most of it is one compute shader's four bindings (~1,040 binds/flip each), whose V# size and
    base keep changing.
- **Direct draws** (~935/flip): only 6% of records hit in every layer.
  - L2 hits 89%, but its misses cost 3.5 us per call.
- **Indirect dispatches** (~360/flip): L1 hits 2%.

**Buffer bindings** (~36,000/flip):
- Compared with the same slot's previous binding: 75% are the same, 11% change only the base,
  14% change something else.
- By class: static 23% of binds, multi 18%, irregular 54%, ring-var 4.8%, ring-fixed 0.4%.
- The ring-var slots are W3's map-discard constants: sizes of 12-176 bytes, a main stride of
  144-176 bytes, and only the base changes.
- **So per-draw constants do come as V#s, but they are a small share.** Keying by ring id
  (W3's map) would turn at most ~5-11% of binds into hits. It waits until L1 shows that those
  misses cost time.

**Decision.**
- **L2 (textures) first**, then **L1 (buffers)**. Both are just over the ~3 ms/flip bar at the
  altar and below it in the intro.
- **L3 and L4 are not built.** Their bounds are 1.8 and 1.0 ms/flip.
- Expected: L1 + L2 together are at most ~7 ms/flip of B at the altar. Hits keep part of the
  cost (session 55), so a realistic gain is ~3-4 ms/flip, about 8% of B.
- For L1, start with the V# key alone. The write sequence (W2) or a plain
  `IsRegionCpuModified` check is its validator, and the ring map stays unused.
- **A separate lead, not a memo:** dispatch buffer misses (4.1 ms/flip, 3.4 us each) and direct
  draws' texture misses (3.5 us each) are slow per call. A profile of `ObtainBuffer` for that one
  compute shader may pay as much as L1.
- "Whole record 14-15 ms/flip" counts all phases of records that hit everywhere; even a
  perfect whole-draw memo can't skip the Vulkan recording (stage C's work), so it is not a
  target.
