# W1: the draw-memo probe (`SOTC_DRAW_MEMO_PROBE=1`)

Wave 1 of the whole-draw memo plan (`docs/next/draw-memo.md`, sections 4 and 5). The probe tells
I1 how much stage-B time each memo layer could save, before any layer is built.

## Files

- `third_party/shadps4/src/video_core/renderer_vulkan/sotc_memo_probe.{h,cpp}`: the probe.
  Both are new and compiled always, because `gpu/CMakeLists.txt` globs `video_core/*.cpp`.
  They do nothing until the hooks are applied.
- `docs/next/draw-memo-probe.diff`: the hook lines. They are **not applied**; I1 applies them
  with `git apply docs/next/draw-memo-probe.diff`. The diff touches:
  - `vk_rasterizer.cpp`: the include, plus `Record` in the four `Record*` functions,
    `Phase` + key calls in `PrepareRenderState`, `BindBuffers`, `BindTextures` and
    `BeginRendering`, and a `Barr` phase around `FlushBarriers` in the four `Record*`
    functions.
  - `buffer_cache.cpp`: `NoteDirty` on the stream copy in `ObtainBuffer` and on an upload in
    `SynchronizeMemory`.
  - `texture_cache.cpp`: `NoteDirty` in `RefreshImage`, after the early returns, where the image
    really uploads.
  - `sotc_pipe.cpp` (the pipe's flip event) and `liverpool.cpp` (the flip without the pipe):
    `OnFlip`.

  No CMake change is needed, and `vk_rasterizer.h` is not touched: the probe's state lives in the
  probe's own namespace.

## What it measures

| layer | key (compared to the last call with the same `Shader::Info*`) | phase timed |
|---|---|---|
| L1 buffers | every V# of the stage (`desc.GetSharp(stage)`), 16 bytes each; a special binding (flatbuf, clip planes, LDS, GDS, ...) adds only its type | `BindBuffers`, per stage |
| L2 textures | every T# (32 bytes) and S# (16 bytes) of the stage, plus `ta_bc_base` when it has samplers | `BindTextures`, per stage (both passes) |
| L3 render state | `mrt_mask`, `color_control`, `color_target_mask`, the `color_buffers[cb]` in the MRT mask and their `CbExtent`, `depth_control/buffer/view/htile_data_base/render_control`, `DbExtent` (all via `Regs()`) | `PrepareRenderState` and `Rasterizer::BeginRendering`, per graphics record |
| L4 barriers | "hit" = L1, L2 and L3 all hit so far in the record | `FlushBarriers` (the `if (needs_barrier)` block) |
| whole record | L1, L2 and L3 all hit | the record, from the first line of `Record*` to its return |

How the comparisons work:

- **L1 and L2** use a direct-mapped table of 4096 entries, slotted like `SotcTexMemo`
  (`(Info* >> 4) * golden >> (64 - bits)`). Each entry stores the `Info*` and a 64-bit hash of
  the key. A **conflict miss** is a miss whose slot held another `Info*`. "stages seen" counts
  the distinct `Info*`, so W4 can size its table.
- **L3** compares with the last graphics record (one entry), like the session-38 render-state
  memo.
- **Clean.** A hit is **clean** when the phase saw no cache-changing event. The events are a
  buffer stream copy (CPU-written data), a buffer upload in `SynchronizeMemory`, and an image
  upload in `RefreshImage`. A real memo whose generation tracks CPU writes (W2) would miss on
  the other hits, so the clean hit time is the realistic upper bound for a layer. For L4 and
  for the whole record, clean means no such event anywhere in the record so far.
- **Overhead.** The key functions time themselves with rdtsc. Their cycles are subtracted from
  the phase and record they ran in, and reported as "probe overhead".
- **Hit/miss split.** Every phase's time is split into hit and miss calls. The hits' time is
  what the layer could skip.

### The buffer-address classification

The probe also classifies each buffer binding, over each 10 s window. A binding slot is a
`(Info*, binding index)` pair, for guest V#s with a non-zero base and size. Each bind is
compared with the slot's previous V#: the same, only `base_address` changed, or something
else changed. The base addresses classify the slot:

- **one-shot**: bound once in the window.
- **static**: one base.
- **multi(<=4)**: 2 to 4 distinct bases (double or triple buffering; a memo with a few entries
  per slot would still hit).
- **ring-fixed**: more than 4 bases, at least 8 moves, and at least 80% of the moves are the
  same forward delta (the stride). A space-saving top 4 of the deltas is kept per slot.
- **ring-var**: more than 4 bases, at least 8 moves, at least 80% of them forward, and the rest
  backward (the wraps).
- **irregular**: everything else.

Ring-class slots are merged into **ring regions**: their `[min base, max base + size)` ranges,
merged where they overlap or are less than 64 KiB apart. The regions are what W3 should match
against the engine's ring allocators.

## Threading and cost

- **All state is stage B's**: `sotc:GpuRecord` with `SOTC_GPU_PIPE=1`, Liverpool's thread
  without it. The probe takes no locks.
- **Two atomics.** `g_flips` is counted where the flip runs: B's flip event, or Liverpool's
  without the pipe. `g_dirty` can also be bumped by stage A while it holds a pipe guard. B is
  stopped then, so the bump lands outside any of B's phases.
- **Off** (the default): each hook is one predictable branch on a constant global
  (`SotcMemoProbe::g_on`, read from the environment at static initialisation). `Phase` and
  `Record` keep a bool and do nothing else.
- **On**: the key functions run `GetSharp` a second time and do a hash-map lookup per buffer
  binding. That time is excluded from the phases, but it slows B. Don't compare frame rates
  with the probe on.

## The log block (every 10 s, from B)

Every line starts with `probe: draw-memo:`. Times are rdtsc cycles, converted with the
window's measured TSC rate. `ms/flip` is milliseconds divided by the flips in the window.

```
probe: draw-memo: 10.0 s, 600 flips, 4320000 records (7200/flip), tsc 3.59 GHz, probe overhead 812.3 ms (1.35 ms/flip), stages seen L1 410 L2 395 (table 4096 entries)
probe: draw-memo: draw: 3800000 records (6333/flip), 5.80 us each, 36.73 ms/flip; all layers hit 61.0% (clean 55.2%): hit 18.10 ms/flip (clean 15.90), miss 18.63 ms/flip
probe: draw-memo: draw L3 rstate: 3800000 calls, would-hit 80.1% (clean 79.9%), conflict misses 0; hit 2.10 ms/flip (0.41 us/call), clean hit 2.09 ms/flip, miss 1.20 ms/flip (0.95 us/call)
probe: draw-memo: draw L3 beginrt: ...
probe: draw-memo: draw L1 buf: 7600000 calls, would-hit ...
probe: draw-memo: draw L2 tex: ...
probe: draw-memo: draw L4 barr: ...
probe: draw-memo: draw-ind: ... (the same lines for each record kind with records: draw, draw-ind, disp, disp-ind)
probe: draw-memo: summary, ms/flip the would-hit calls spend in the phases a layer skips (clean = no upload/stream/image upload in the phase): L1 buf 9.10 (clean 7.80), L2 tex 5.20 (clean 5.10), L3 rstate+beginrt 2.60 (clean 2.60), L4 barr 0.40 (clean 0.30), whole record 18.10 (clean 15.90) of 42.00 ms/flip in records
probe: draw-memo: L1 bindings: 9000000 binds (15000/flip) vs the slot's last: same 70.0%, base-only change 28.0%, other change 2.0%, first 12; slots by class: one-shot 3 (0.0% of binds), static 800 (60.0% of binds), multi(<=4) 40 (5.0% of binds), ring-fixed 12 (20.0% of binds), ring-var 20 (14.0% of binds), irregular 9 (1.0% of binds),
probe: draw-memo: ring region 0: 0x1234500000-0x1234d00000 (8192 KiB), 25 slots, 2400000 binds (4000/flip)
probe: draw-memo: ring slot 0: pgm 0123456789abcdef b2 ring-fixed, 300000 binds (500/flip), base 0x...-0x... (8192 KiB), size 256, moves 299990 (fwd 299900, back/wraps 90), top stride 256 (99% of moves), V# same 10 base-only 299980 other 0
probe: draw-memo: irregular slot 0: ...
```

(The numbers above are invented to show the format.)

### Reading it, for the go/no-go (`draw-memo.md` section 4)

- **What each layer saves per flip** is in the `summary` line. Take the `clean` value for L1:
  it assumes W2's write sequence. A layer is worth building at about 3 ms/flip or more.
- **Per record kind**, the `<kind> <layer> <phase>` lines give the would-hit %, the hit and
  miss time per flip, and the cost per call. If the hits' us/call is much lower than the
  misses', the miss path is where the time goes, and the layer saves less than its hit rate
  suggests (the session-55 lesson).
- **Rings vs real CPU writes.** In the `L1 bindings` line, a high **base-only change** share,
  together with many binds in ring-fixed or ring-var, means L1's misses are moving addresses:
  W3's ring map goes first in W5. A high **other change** share, or a large gap between L1's
  would-hit and clean percentages, means real CPU writes: W2 does the work.
- **conflict misses**: if they are a noticeable share of the misses, W4's table needs more bits
  than 12 (or than `SotcTexMemo`'s 10).

## Checked

- The diff applies cleanly to `3d212c9` (`git apply --check`).
- With the diff applied, `build-vk` (RelWithDebInfo, clang-cl) builds fully: `sotcgpu` and
  `sotc-runtime.exe`. The diff was then reverted.
- Not run: W1 makes no GPU runs. I1 does them.

## Open questions

1. **L4 isolates only the emission.** Barrier collection (`IsBufferAccessed` and `Transit`
   inside `BindBuffers`/`BindTextures`) is part of the L1 and L2 phase times. Isolating it
   cheaply would need laps inside `vk_runtime`. Do that only if L1's or L2's hit time looks big
   enough to split.
2. **L3 compares with the last graphics record only** (one entry), like the session-38 memo, and
   is not keyed by `Info*`. Draws from another pass in between break it, as they would break
   the memo.
3. **A special binding does not count against a hit.** The flatbuf copy and the clip planes run
   anyway; L1's key holds only the binding type. A memo that also skipped the flatbuf copy would
   need the flat user-data words in its key.
4. **The L1 key ignores the binding flags** (`is_written`, `is_formatted`). They are fixed per
   `Info*`, so the comparison against the same `Info*` already covers them.
5. **A hit can still get a different result.** The probe compares guest words only, so a hit can
   still resolve differently: a buffer re-created in the cache, or an image re-registered.
   Clean excludes only uploads. The real generations (buffer-cache generation, find state) are
   for verify mode (`SOTC_DRAW_MEMO_VERIFY=1`) in W5 and W6.
6. **Slot keys use the `Info*`.** A slot is keyed by `Info*` and binding index. A pipeline
   recompiled with a new `Info*` starts a new slot, and old slots stay in the map, which grows
   with the number of shaders.
7. **For integration (I1).** `CLAUDE.md` asks for a row for every local patch to `third_party/shadps4`
   in `third_party/shadps4/README.md`. Add one for `sotc_memo_probe.{h,cpp}` and the hooks when
   the diff is applied. W1 doesn't touch shared files, so it doesn't add the row itself.
