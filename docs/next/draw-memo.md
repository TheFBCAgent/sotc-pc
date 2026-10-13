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
