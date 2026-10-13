# Draw preparation off stage B: design notes

Started in session 59 (2026-10-13, Windows), the design session for multi-threaded draw
preparation (plan in [../NEXT.md](../NEXT.md)). This file collects the inputs: what bbport built
(agenda item 1) and where B's time goes per record (item 2). The shape, rules and build order
(items 3-5) come next.

## 1. What bbport built

Source: `docs/parallel_gpu.md` in `deadinside28/bloodborne_pc` (`master`; the same file is in
`bratdr/bloodborne_pc`), plus `gpu/shadps4/video_core/renderer_vulkan/vk_draw_prep.{h,cpp}`,
`vk_draw_pipe.h` and the prepared-draw paths in `vk_pipeline_cache.cpp`, `vk_rasterizer.cpp` and
`liverpool.cpp`.

- **Built before its two-stage pipe**, on the single GPU thread. The +11.5% is a single-thread
  number, and most of it came from **pipeline selection**, which is stage A's work in both
  designs. After the pipe only the prepared sharps and vertex inputs reach the recording stage
  (`packet.prepared`, used as `bind_prepared`). The doc never measures draw prep again with the
  pipe.
- **Threads:** one scanner (`bb:DrawScan`) and N workers (`bb:DrawPrepN`, half the hardware
  threads, 1..8), all `SCHED_IDLE`. The first design (4 workers, each replaying the whole
  stream) gave nothing on 8 threads and was replaced.
- **Input and lookahead:** at `sceGnmSubmit` the game's thread copies each graphics DCB and
  allocates one `PreparedDraw` per direct draw. The scanner replays register writes and stores
  each buffer's start/end checksum and a register delta (32-word blocks). A worker claims the
  nearest scanned buffer the GPU thread hasn't reached and stops once the GPU thread passes it.
  If the scanner lags more than 64 buffers, it is rebased from the GPU thread's registers. The
  unit of work is a whole guest command buffer (~65-80 per frame, 10-200 draws each).
- **Per draw a worker computes:** the pipeline key (`RefreshGraphicsKey` on its own register copy
  and its own `Shader::Info` copies), every T#/S#/V# through `GetSharp` plus the texture
  description hash, and the vertex inputs (attribute/binding descriptions, merged stream
  ranges). Variable-size data goes into a per-submission arena of chunks that never move.
  "Workers never create or compile anything."
- **Stays on the consuming thread:** every cache operation (`FindImage`/`FindView`,
  `ObtainBuffer`, uploads, barriers), descriptor writes, render targets, dynamic state and all
  Vulkan recording. **No cache was made thread-safe and no command buffer was recorded in
  parallel**: those were planned steps 2 and 3, never built.
- **Speculative and verified on the consumer:** a prepared draw is used only if its register
  checksum equals the GPU thread's running checksum, each stage's program hash matches, and
  `pgm_base` plus the flat user data are equal word for word (`memcmp`; the flat data covers
  everything the specialization read from guest memory). Otherwise the normal path runs.
  No generations. Compute-queue SH writes perturb the checksum, so those draws miss.
  Tessellation and indirect draws/dispatches are never prepared.
- **Results** (single thread): pipeline prep 64.1 -> 71.5 fps (+11.5%, 97-98% of direct draws
  used it, each worker ~10% of a core); sharps on the workers +3%; with vertex inputs +5.5%
  (both against prep without them). Per draw on the GPU thread 7.7 -> 6.5 us; after the pipe
  their stage B costs ~2.8 us per packet (ours ~5.8 us per record).
- **Dropped:** a per-draw fork/join texture helper (no gain: the helper's task cost ~6,200 cycles
  against ~4,600 inline, and the GPU thread still waited ~2,500 per join; "moving work to
  another core per draw costs about what it saves, because the data is shared. Scaling needs
  splits where each thread owns its data for long stretches").
- **Pitfalls:** a worker read a V# the guest was still writing (`SurfaceFormat` assert; now
  `TrySurfaceFormat`, the draw left to the GPU thread); reads of guest memory that may already
  be reused (fault recovery, which leaks partial allocations); a per-draw clock read cost 8% of
  the GPU thread; shared `Shader::Info` holds per-draw state (per-worker copies needed).

What this means for us: copying bbport's workers as built would mostly unload stage A, which is
not our limit (A ~25 ms/flip vs B ~42 on Windows). Their sharps and vertex inputs could be
computed by A itself, which already has the flat user data. Taking real work off B needs what
bbport never built: cache-hit lookups off B, or parallel recording.

## 2. Where B's time goes per record (Windows, session 59)

Probe: temporary `rdtsc` laps through the four record functions, `BindBuffers`/`BindTextures`
timed inside `BindResources`, and counters for the cache-changing events
([draw-prep-probe.diff](draw-prep-probe.diff), applies on `b9bbc7e`; `git apply` it to measure
again). Run `s59-probe`: altar, Pro mode, `flipcpu.py`, 20.25 flips/s, A 24.5 + B 42.1 ms/flip
(B 41.7 without the probe, `s58-submits`), no hot-flush storm.

Per 10 s (~202 flips, ~1.46M records, ~7,200 per flip). The records themselves take
**~7.8 s of B's ~8.5 s of CPU (~92%)**; the rest is packet reading, commands and flips.

| record kind | per 10 s | us each | largest phases (us) |
|---|---|---|---|
| indirect draw | ~718k | 4.0 | tex 0.82, buf 0.79, binddraw 0.65, rstate 0.59, push 0.43 |
| dispatch | ~471k | 5.9 | buf 2.03, binddraw 1.67, cpucopy 0.66, barr 0.62, tex 0.45 |
| draw | ~193k | 7.6 | tex 1.74, binddraw 1.40, buf 0.78, push 0.71, vkbegin 0.70, barr 0.67 |
| indirect dispatch | ~73k | 9.0 | buf 3.58, binddraw 3.10, barr 0.76 |

By phase, all kinds together (ms per 10 s, of ~7,800):

| phase | ms | what it is |
|---|---|---|
| buf (`BindBuffers`) | 1,936 | `ObtainBuffer` (residency, tracker sync, uploads, stream copies, write notes), descriptors |
| binddraw | 1,751 | bind pipeline, draw/dispatch, hot-flush submits (~2,200 per 10 s), `ResetBindings` |
| tex (`BindTextures`) | 1,157 | `GetSharp`, `FindImage`/`FindView` (hits), samplers, descriptors |
| push | 590 | `pipeline->BindResources`: push descriptors and push constants (driver) |
| rstate (`PrepareRenderState`) | 501 | render-target and depth lookups (graphics only) |
| barr (`FlushBarriers`) | 492 | our barrier collection plus `vkCmdPipelineBarrier2` |
| cpucopy | 311 | committing the CPU copy A computed (dispatches) |
| bindrest | 271 | the rest of `BindResources` (user data, DMA sync) |
| dynstate | 254 | `UpdateDynamicState` (driver calls) |
| beginrt | 185 | `Rasterizer::BeginRendering` (render-target lookups) |
| vkbegin | 164 | `Scheduler::BeginRendering`/`EndRendering` |
| vtx | 143 | vertex/index buffers, `setVertexInputEXT` |
| indbufs | ~50 | indirect argument/count buffers |

**Cache-changing events** per 10 s:
- ~27k buffer uploads in ~20k records.
- ~138k stream copies in ~99k records.
- ~413 new images and ~209 image uploads.
- ~1.13M written-binding notes in ~487k records (a third of all records, mostly dispatches).

**92.7% of records change nothing in the caches** apart from their written-binding notes. Only
7.3% have an upload, a stream copy, a new image or an image upload.

Reading:
- **Roughly 58% of record time is our own CPU work** (buf, tex, rstate, beginrt, bindrest, vtx,
  cpucopy: ~4.5 s per 10 s, ~22 ms/flip), and **~42% records Vulkan commands** (binddraw, push,
  barr, dynstate, vkbegin: ~3.25 s, ~16 ms/flip). The driver's share fits session 54's ~22% of
  B in `nvoglv64` (some of binddraw and barr is our code).
- **The CPU part is mostly cache hits.** Moving it off B (shape A in the plan) could take up to
  ~22 ms/flip off B, but its results (Buffer pointers and offsets, image views, render-target
  state) depend on cache state that B changes. A worker's result needs a validity check, as
  bbport's prepared draws had, here against the caches' generations, and the 7.3% of records
  that change the caches stay on B. The written-binding notes are bookkeeping that must keep
  their order: they stay on B (the session 48 memo already skips 74%).
- **Vulkan recording is a hard floor for shape A**: ~16 ms/flip, plus ~3 ms outside the
  records. With all of the CPU part gone, B would be ~20 ms/flip, below A's ~25: A becomes the
  limit at about 40 flips/s. Going past that needs A's own work split, or parallel recording
  (shape B), which also spreads the driver's time.
- **Per-draw fork/join is ruled out** by bbport's result. Work has to be handed over in long runs
  (whole packets or command buffers ahead of B), never per call.
