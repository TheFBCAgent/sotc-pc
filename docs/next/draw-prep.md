# Draw preparation off stage B: design notes

Started in session 59 (2026-10-13, Windows), the design session for multi-threaded draw
preparation (plan in [../NEXT.md](../NEXT.md)). This file collects the inputs: what bbport built
(agenda item 1) and where B's time goes per record (item 2). Session 60 chose the shape, its
rules and the build order (items 3-5): sections 3-9. Session 61 ran step 1, the probe: section 10.

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

## 3. What a worker could prepare ahead (session 60, from the code)

Shape A (the session 59 plan: workers fill a "prepared" block per record ahead of B, and B checks
it against cache generations) only pays for work whose result B can still trust later. Here is
the record path, piece by piece:

| piece | result depends on | can be prepared ahead? |
|---|---|---|
| T#/S#/V# sharps (`GetSharp`), the clamp, vertex inputs and merged vertex ranges | the packet's flat user data, mappings | yes (pure). But session 48 measured the clamp on A as noise (B -0.5 ms/flip) |
| `ObtainBuffer`'s arena and offset (`GetArena`) | the address only | yes, but it costs nothing |
| `ObtainBuffer`'s stream check, `EnsureResident`, `SynchronizeMemory`/`ForEachUploadRange`, uploads | the memory tracker's CPU/GPU-modified pages | **no.** Guest threads mark pages CPU-modified themselves, in their write faults (`GuestFaultSignalHandler` -> `InvalidateMemory` -> `InvalidateRegion`, ~40k per second in the temple, session 51). A worker's "nothing to upload" goes stale whenever the guest writes between the worker and B. Only B's own check, at B's position, is correct, and that check is most of the cost |
| written-binding notes (`SotcNoteGpuWrite`, `InvalidateMemoryFromGPU`), `IsBufferAccessed`, `Transit`, `binding.is_bound`/`force_general`, `SotcTouchFound` | B's bookkeeping, in stream order | no: they are the bookkeeping |
| `FindImage` hits | texture-cache state (`SotcFindState` has a generation) | yes, with a generation check. The texture-set memo (session 40) already does this on B for repeated sets |
| `FindTexture` (view lookup), `GetSampler` | the image's view list, the sampler map | yes, but they are hash lookups that B could memoize itself |
| render-target lookups (`PrepareRenderState`, `BeginRendering`) | texture-cache state | the render-state memo (session 38) already skips repeats on B |

So **most of B's "own CPU work" (~22 ms/flip) is ordered bookkeeping or tracker checks, and both
must run at B's position.** The parts that are pure or cacheable already have memos on B, or
measured as noise when moved to A. Shape A's real ceiling is a few ms/flip, not ~22, and every
result needs a validity check. bbport hit the same wall: its workers never touched a cache.

The **Vulkan recording** does not depend on cache state at all. It is ~16 ms/flip inside the
records (binddraw, push, barr, dynstate, vkbegin), ~42% of the record time, plus ~3 ms outside
them (flushes, flips). Once the bookkeeping has chosen the handles, offsets, layouts and
barriers, writing them into a command buffer needs nothing from the caches.

## 4. The shape: split stage B by ownership (shape D)

The choice is neither (A) prepare-ahead nor (B) parallel recording from the session 59 agenda. It
is the step that comes before (B): a third pipe stage.

- **Stage A** (Liverpool's thread): unchanged.
- **Stage B** (`sotc:GpuRecord`): unchanged, **except that it no longer records Vulkan commands**.
  It keeps every cache and all bookkeeping:
  - the buffer and texture caches, the memory tracker, `gpu_modified_ranges`, write ticks, hot
    lines and readback slots;
  - staging and stream-buffer allocation;
  - `Runtime`'s access and layout tracking, so it still decides every barrier;
  - the render-state and texture-set memos;
  - the command queue, fences, deferred operations (`PopPendingOperations`) and the GC.

  Wherever it records today, it **appends a command** to a second ring instead. A command holds
  resolved values only: Vulkan handles, offsets, sizes and layouts, the descriptor writes (by
  value), push-constant bytes, dynamic-state values, and the draw or dispatch arguments.
- **Stage C** (new thread `sotc:GpuCmd`): owns the command buffers and the queue side of the
  `Scheduler`. That covers begin/end, submits, and the per-command-buffer state: dynamic-state
  tracking, the bind memo (`Scheduler::BindPipeline`), `SOTC_DESC_CACHE`, push descriptors, and
  rendering begin/end. C reads the commands in order and records them. It never reads a cache,
  guest memory or registers.

Why this shape:
- **Each thread owns its data for long stretches** (bbport's lesson): B owns the caches and C owns
  the command buffers. No cache has to become thread-safe, and there is no per-draw join.
- **Nothing is speculative.** There are no generation checks, no misses and no verify-and-redo
  path: a command is final when B appends it.
- **The CLAUDE.md invariants stay single-threaded on B** (section 6). Ranges, written pages,
  `sotc_note_gen`, `sotc_meta_clears`, the find cache and readback settle all keep their thread.
- **It is the road to (B).** Once commands are resolved values, several C threads could record
  segments into secondary command buffers without touching any cache. That is not built first.

Ceiling, from session 59's Windows numbers (rough estimates):
- **B ~26-28 ms/flip.** B loses ~16 ms/flip of recording and gains the encoding (~7,200 records
  x ~0.3 us, ~2 ms).
- **C ~21 ms/flip:** ~16 of recording, ~3 outside the records and ~2 of decoding.
- **A ~25 ms/flip.**

All three end up near 25 ms/flip: **~36-40 flips/s, against ~20 today.** The limit is then
whichever of A and B is slower, and A's own work is the next item. On Linux (B ~26 ms/flip,
RADV ~21% of B) the gain is smaller in ms but the same in kind.

Costs and risks:
- **Encoding.** Every `vkCmd*` that B makes today becomes a command. The recording sites are:
  - `Runtime` (`vk_runtime.cpp`: barriers and copies, ~12 sites);
  - the rasterizer (~12);
  - the scheduler (rendering begin/end, flush);
  - `tile_manager` and `blit_helper` (2 each);
  - the presenter (7).

  That is ~40 sites, not the whole of video_core.
- **Lag.** C runs behind B. Anything on B that needs the GPU to have *executed* something (a
  download, `Scheduler::Wait`) now also needs C to have *submitted* it (section 5).
- **Lifetimes.** A handle in a command that C has not recorded yet must stay alive (section 5).

## 5. Rules for shape D

1. **Ticks belong to B.** B numbers batches as it does today (`Scheduler::CurrentTick`).
   - A flush on B (a hot flush, an in-batch readback, submit-done, a flip) appends a
     `Submit(tick, semaphores)` command and advances B's tick at once. C submits when it gets
     there.
   - `Scheduler::Wait(tick)` and `IsFree(tick)` on B first wait until C has *submitted* that
     tick (C stores a release counter after each submit), then wait on the timeline semaphore as
     today.
   - A wait on the batch being built (today: flush, then wait) appends the flush first.
2. **Commands carry values, never pointers into B's state.** These are all copied into the
   command:
   - descriptor writes (`buffer_infos`/`image_infos` are reused by the next record);
   - push constants and vertex input descriptions;
   - barriers (`vk::BufferMemoryBarrier2`/`ImageMemoryBarrier2` arrays) and copy regions.

   B writes staging and stream-buffer bytes into mapped memory before it appends the copy.
   Their reuse is tick-based, and rule 1 keeps it correct.
3. **No Vulkan object that B appended dies before C records it.** Today images, views and blit
   views die through `Scheduler::DeferOperation`, after the GPU finished the last tick that used
   them. C submitted that tick, so C has recorded everything before it. Every immediate destroy
   on B (a `vk::Unique*` reset or destructor, `SotcDescCache::Dead`) of a handle that can be in a
   command must do one of two things: be deferred the same way, or be appended as a `Destroy`
   command that C runs in order. The verify mode (section 7) checks this.
4. **The side queue stays on B.** Async readback's side copies (~30 per flip) have their own
   command pool and buffers, which C never touches. Their submit waits for the main-queue tick
   that wrote the data, so it first waits for C to have submitted that tick (rule 1).
5. **Rare work that needs both the caches and the command buffer runs on B while C is drained**,
   as the pipe's drain guard does for A. B waits until C has recorded everything appended, then
   records directly into C's current command buffer under C's mutex, and C continues after it.
   - Candidates: the presenter's flip passes (1 per flip; a command later), image creation and
     recreation copies (~413 new images per 10 s), `SOTC_DUMP_RT`, screenshots, ImGui.
   - Each such drain is counted by reason, as the pipe's are. A frequent one becomes a command.
6. **C's lag is capped** (commands in flight; start at ~1,000 records). A guest read fault on
   data in the batch being built now waits for C to submit it, so the lag adds to the fault's
   latency (~380 us today). Step 3 measures it.
7. **C's per-command-buffer state moves with the recording**: the bind memo, the dynamic-state
   tracker, the rendering state (`Scheduler::BeginRendering`/`EndRendering`, which today also
   compares render-target info) and the descriptor cache. B keeps the decisions that need the
   caches: which images are targets, and their layouts.

## 6. The CLAUDE.md invariants under shape D

| invariant | thread(s) today | with shape D |
|---|---|---|
| `gpu_modified_ranges` + `SotcWrittenPages`, `sotc_ranges_mutex` | B (A reads) | unchanged |
| `RegisterImage`/`SotcFindCache`, `sotc_uncached_finds` | B | unchanged |
| `sotc_note_gen` (written-binding memo) | B | unchanged |
| `ClearMeta`/`TouchMeta`, `sotc_meta_clears` | B | unchanged |
| `SotcReadbackSettle` before changing guest bytes or tracking | B | unchanged. A readback's copy and flush become commands; its finish still runs on B |
| `SotcDescCache::Dead` before a handle dies | B | moves to C with the cache; rule 3 orders the destroy after C used the handle |
| `resident_ranges` only grows, `sotc_resident_memo` | B | unchanged (residency is memory allocation and binding on B, not command recording; step 1 checks this) |
| `Scheduler::BindPipeline` memo, `SotcForgetPipelines()` after raw binds | B | moves to C. The presenter's raw binds are rule 5 drains and run on C's buffer, so they still forget |
| never `SendCommand<true>` from B, or from A holding a guard | A, B | unchanged, and never from C either |
| no PM4 reads after a signal; register reads only through `Regs()` | A, B | unchanged: C reads neither |

## 7. Verify mode

`SOTC_GPU_CMD_VERIFY=1` catches rule 3, the one bug class this shape adds:
- B registers every handle it appends (buffer, image, view, sampler, pipeline) with the tick and
  command sequence it was appended at.
- Every destroy on B checks that C's recorded sequence has passed the handle's last use.
  Otherwise it logs `cmd: <kind> destroyed before C recorded it (B at N, C at M)`.

Sync mode catches rule 2 (pointers into reused storage): B appends a command and C records it at
once, on B's thread. A dangling pointer behaves the same in both modes, but a command that copied
too little differs from the threaded run. Compare screenshots at the altar, as for the pipe.

## 8. The refactor first (agenda item 4)

Yes, and it is running now (session 60, workstream `refactor`): the pipe, CPU-copy and verify
code move out of `vk_rasterizer.cpp` into `sotc_pipe.cpp` and `sotc_cpu_copy.cpp`. Shape D
changes `Runtime`, the `Scheduler`'s recording side and every recording site in the rasterizer.
With a 3,000-line file of pipe code in between, each step's diff would be hard to review.

The command ring and the encoder/decoder get their own files from the start:
- `sotc_cmd_ring.h`: Vulkan-free like `sotc_gpu_pipe.h`, with a unit test;
- `sotc_cmd_record.cpp`.

## 9. Build order

Each step sits behind `SOTC_GPU_CMD` (`0` = today's path, `sync`, `1` = the thread). Each is
measured with `flipcpu.py` (Pro mode, alternating, the same exe with the switch on and off;
B's and C's ms/flip and flips/s) and confirmed on Linux before the next step starts.

1. **Probe** (temporary, like session 59's). Before building, find out the encoding surface and
   the drain count:
   - count the commands per record, by kind;
   - count the rule 5 events per flip (presenter, image-creation copies, other direct recording);
   - time B's recording calls by site;
   - list which destroys on B are immediate (rule 3).
2. **The command ring and sync mode**, no thread yet. `Runtime`, the rasterizer's recording sites
   and the scheduler's rendering and flush calls append commands. With `sync`, C's decoder runs
   on B's thread right after each record.
   - Expected: the same output (compare screenshots).
   - Measure the encode/decode cost (the pipe's step 1 cost +2.5%).
   - Tests for the ring and the encoders.
3. **The C thread**, with rules 1 (ticks, waits, submits), 3 (destroys), 4 (side queue),
   5 (counted drains) and 6 (lag cap), plus the verify mode. Measure against `SOTC_GPU_CMD=0`:
   flips/s, B and C ms/flip, the drain counts, and the guest's read-fault latency
   (`pipe: A waits:`).
4. **Reassess**, depending on which stage is the limit:
   - **A:** A's own work (pipeline selection, the CPU copy's compute; NEXT.md item 7).
   - **C:** the redundant dynamic-state sets (session 57's plan 0.2), then segments recorded on
     several threads (shape B).
   - **B:** memos for `FindTexture` and `GetSampler`, or prepared sharps on A (section 3, small).

Dropped from the session 59 order:
- Step 3 (A prepares the sharps): it would move work onto A, which becomes a limit with shape D.
- Step 4 (workers for cache hits): its buffer half can't be trusted ahead of B (section 3).

Both stay in step 4 above as small options.

## 10. Step 1: the probe (session 61)

Probe: [draw-prep-cmd-probe.diff](draw-prep-cmd-probe.diff) (applies on `0d5f1e9`;
`SOTC_CMD_PROBE=1`). It wraps the `vkCmd*`, submit and destroy entries of vulkan-hpp's dynamic
dispatcher (and VMA's destroys), so no recording site changes. Every call is counted and timed
by thread, by context (the record kind, or a site scope: presenter, image refresh, overlap
copies, tiling, blits, deferred ops) and by function. Commands on B outside every scope, and
destroys outside deferred ops, keep their call stacks (`llvm-symbolizer --relative-address`).
Run `s61-probe1`: altar, Pro mode, `flipcpu.py`, 19.40 flips/s, A 24.5 + B 43.6 ms/flip
(probe off, `s61-base1`: 19.11 flips/s, B 43.7 ms/flip: the probe costs nothing measurable). Numbers per flip, 150-225 s.

**Commands per record:**

| record kind | per flip | commands each | mostly |
|---|---|---|---|
| draw | ~940 | 12.4 | bind pipeline, begin/end rendering, barrier, push descriptors, push constants, vertex input, index buffer, ~5 dynamic-state sets, draw |
| indirect draw | ~3,515 | 5.25 | push descriptors, push constants, vertex input, index buffer, draw (bind pipeline in 1 of 5) |
| dispatch | ~2,316 | 3.83 | push descriptors, push constants, dispatch, barrier in 2 of 3 |
| indirect dispatch | ~361 | 4.5 | the same plus bind pipeline |

**~41,000 commands per flip on B**, not ~7,200: the encoder must be cheap per command (~40 ns
each is ~1.6 ms/flip), and push descriptors (~7,100/flip, writes by value) are the largest payload.

**B's time inside Vulkan calls: ~12.4 ms/flip** (session 59's "recording" phases, ~16 ms/flip,
also hold our own code around the calls):
- push descriptors 2.6 (0.36 us each), bind pipeline 1.6 (~2,090/flip after the bind memo, 0.78
  us), dispatch 1.6 (0.69 us), barriers 1.6 (~2,370/flip, 0.66 us), begin rendering 0.7 (1.5 us);
- queue submits 1.6 (~41/flip, ~38 us each) plus begin/end command buffer 0.4;
- `vkFreeMemory` 0.45 (~2/flip, ~220 us each, from deferred image destroys; session 14's opt-in
  `SOTC_ASYNC_IMAGE_FREE` moves them);
- the many small calls (draws, push constants, vertex input, index buffer, dynamic state; ~25,000
  per flip) ~1.5 ms together, ~40-110 ns each including the probe's wrapper.

**Revised ceiling.** C's work is ~12 ms/flip of driver calls plus decoding, well under A. B
loses those ~12 ms and some of the struct building around them, and pays the encoding (~41k
commands): **B ~30-33 ms/flip** instead of section 4's 26-28, so B stays the limit at
**~30-33 flips/s** (against ~20 today) until B's own CPU work shrinks.

**Rule 5 (work that needs the caches and the command buffer), per flip on B:**
- overlap copies (`ResolveOverlap`/`ExpandImage`): ~4 scopes with commands (image-to-buffer
  and buffer-to-image copies, barriers);
- image uploads (`RefreshImage`, with `DetileImage`'s dispatch): ~1;
- the presenter's `PrepareFrame`: 1 (barriers, rendering, one draw). `Present` itself runs on
  the presenter thread with its own command buffers (~10 commands, a blit and a submit).
- blits (`BlitHelper`): 0 at the altar.

All of them are copies, barriers, clears or a dispatch: **they can be ordinary commands**, so
no drains are needed in gameplay except `PrepareFrame` (or make it a command too).
Commands on B outside every record and scope, from the stacks:
- the async readback's side-queue copies (`SotcRbStart`: barriers, copy, own submit, ~30/flip;
  rule 4 keeps them on B);
- `FlushStreamedRanges` -> `SynchronizeMemory` copies from `OnSubmit` (about as many as the side copies);
- the flush callback's barriers (`Rasterizer` constructor lambda, from `Scheduler::Flush`);
- `EliminateFastClear` (a clear between packets, ~0.5/flip);
- `EnsureResident`'s copy (rare).

**Rule 3 (immediate destroys on B):** none of a handle a pending command can use. Every destroy
on B is either a deferred operation or gated on `scheduler.IsFree(tick)`:
- `StagingBufferPool::TrimRing`/`TrimLarge` (from `SotcFlipTick`, ~1/s: `UniqueBuffer::Destroy`);
- `DescriptorHeap::Commit` (`vkResetDescriptorPool`, ~0.4/s).

So **rule 1 must make `IsFree(tick)` answer "no" for a tick C has not submitted** (not wait).
Then these stay correct with no change. The presenter thread's screenshot code destroys its own
objects. `SOTC_DESC_CACHE` (off) and the emergency image GC did not run here; step 3's verify
mode still checks them.

## 11. Step 2a: the command ring and sync mode (session 62)

Built behind `SOTC_GPU_CMD` (`0` default; `sync`, or `1` until step 3 exists, decodes on B):
- `sotc_cmd_ring.h` (Vulkan-free, test `gpu/tests/test_cmd_ring.cpp`): a single-producer
  single-consumer byte ring. Records are written in place (`Reserve` returns room for the
  header and payload) and published once per record, not per command. `Reserve` returns
  nullptr when the ring is full: sync mode decodes then (`OnFull`); step 3's thread will wait.
- `sotc_cmd_record.h/.cpp`: `SotcCmdBuffer` (from `Scheduler::Cmd()`) has `vk::CommandBuffer`'s
  method names for every call the record path makes, so a site only changes where it gets its
  command buffer. While a scope is open it appends a command holding values only (rule 2:
  descriptor writes and their infos, push constants, barrier arrays, vertex input descriptions,
  `RenderState` for begin rendering). Otherwise it records directly, so with `SOTC_GPU_CMD=0`
  the cost is one predictable branch per call.
- Converted: the four `Record*` functions (`SotcCmdScope` opens and closes the scope),
  `BindVertexBuffers`, `BindIndexBuffer`, `UpdateDynamicState` (`DynamicState::Commit` takes
  the encoder), `Pipeline::BindResources` and `SotcBindCached` (push constants, push
  descriptors, descriptor-set binds), `Runtime::FlushBarriers`, and the scheduler's
  `BeginRendering`/`EndRendering`/`BindPipeline`.
- **The decisions stay on B:** the bind memo, the dynamic-state dirty bits and the open-pass
  check (`is_rendering`/`render_state`) are still updated where B encodes, so C only records.
  This differs from rule 7, which moved them to C. It is simpler, every command is final, and B
  already knows when a command buffer ends (rule 1: flushes are B's).
- **Ordering without converting every site (the "break"):** `Scheduler::CommandBuffer()` and
  `SubmitExecution` check the scope. A direct access inside an open scope decodes the pending
  commands first and closes the scope, so the rest of that record records directly. A site
  that holds a raw command buffer across a call to an encoded function (`Runtime`'s copies
  call `FlushBarriers`, then record) therefore stays in order. The breaks are counted by site
  in a `pipe: cmd:` line every 10 s. They are step 2b's list, and in step 3 each one is a
  drain.

**Altar, Pro mode, `flipcpu.py`, alternating, same exe (`s62-*`):**

| run | flips/s | A ms/flip | B ms/flip |
|---|---|---|---|
| sync1 | 18.49 | 28.6 | 47.3 |
| base1 (`=0`) | 19.83 | 24.1 | 42.3 |
| sync2 | 19.24 | 25.4 | 45.2 |
| base2 (`=0`) | 18.96 | 26.9 | 43.5 |
| sync3 | 18.83 | 28.0 | 46.9 |

- **Sync mode costs B ~3.6 ms/flip** (means 46.5 vs 42.9, ~+8%; the pipe's step 1
  cost +2.5%), ~-0.5 flips/s. That is encode plus decode for ~39,000 commands per flip,
  ~90 ns per command, above section 10's 40 ns budget. Step 3 moves the decode to C, so B
  keeps only the encode. Its share is not measured separately yet: step 3 shows it as B's
  ms/flip minus what it loses.
- **The output is the same:** the altar screenshots of base2 and sync3 show the same frame,
  with no missing or corrupted pass. No errors in the logs, and no hot-flush storms
  (~110-200 hot flushes/s).
- `pipe: cmd:` line: ~130,000 scopes per 10 s (~7,000 records per flip), **5.6 commands per
  record, 139 bytes each** (push descriptors dominate), no full-ring drains, and **1.5% of
  records break**. By site:
  - `vk_runtime.cpp` `Runtime::CopyBuffer` ~1,850/10 s (buffer uploads and stream copies
    inside `ObtainBuffer`);
  - the hot flush's submit after a dispatch ~650 (its dispatch is already encoded; the break
    only decodes it before the submit);
  - `Runtime::CopyImageToBuffer`-type copies ~365, `TileManager` detiling ~185, the indirect
    dispatch's hot flush ~180.

**Step 2b** converts these sites: `Runtime`'s copies (buffer, image, clear), tiling, the
hot-flush submit as a `Submit` command (rule 1), then the remaining scheduler users. That
leaves breaks only where the caches and the command buffer meet (rule 5).

## 12. Step 2b: the break sites converted (session 63)

- New commands (value-only regions, rule 2): `copyBuffer`, `fillBuffer`, `updateBuffer`
  (<= 64 bytes, `Runtime::InlineData`'s 4), `copyBufferToImage`, `copyImageToBuffer`,
  `copyImage`, `resolveImage`, `clearColorImage`.
- `Runtime`'s copies, fills, inline data, uploads/downloads, image copies, resolves and clears,
  and `TileManager`'s tiling and detiling dispatches record through `Scheduler::Cmd()`.
- **A submit inside a scope no longer breaks it.** `SubmitExecution` calls
  `SotcCmdState::FlushSubmit` (decode what the record appended, shut the scope), submits as
  before, and reopens the scope on the next command buffer, so the rest of the record stays
  encoded. The `pipe: cmd:` line counts these as in-scope submits. Step 3 turns this point
  into a `Submit(tick)` command (rule 1); submits still run on B.
- Not converted (rare, all outside the altar's records): `BlitHelper` (MSAA copies and
  depth reinterpretation), the fault manager's dispatch, the readback prefetch at batch end
  (it runs in the submit callback, with the scope shut), debug markers and the presenter.

**Altar, Pro mode, `flipcpu.py`, alternating, same exe (`s63-*`):**

| run | flips/s | A ms/flip | B ms/flip |
|---|---|---|---|
| sync1 | 17.67 | 28.8 | 46.4 |
| base1 (`=0`) | hung at the title at ~85 s (see below) | | |
| sync2 | (pad script drifted: another scene, ~5,400 records/flip; not used) | | |
| base2 (`=0`) | 17.97 | 28.5 | 43.1 |
| sync3 | 19.40 | 26.5 | 45.1 |
| base3 (`=0`) | 19.59 | 26.6 | 41.8 |
| sync4 | 19.12 | 27.0 | 45.2 |
| base4 (`=0`) | 20.76 | 23.7 | 40.8 |

- **Breaks: 0 of ~1.3 million scopes per 10 s** (session 62: 1.5%), no full-ring drains,
  ~1,060-1,360 in-scope submits per 10 s (= the hot flushes). 5.7 commands per record,
  138 bytes each.
- **Sync mode still costs B ~3.7 ms/flip** (means 45.6 vs 41.9; session 62: +3.6), flips/s
  18.7 vs 19.4. Converting the 1.5% of broken records changed nothing measurable: the cost is
  the encode+decode of every command, and step 3 moves the decode to C.
- The output is the same: sync3's and base2's altar screenshots show the same frame (only
  the animation phase differs). No errors in the logs, no hot-flush storms (1,070-1,830 per
  10 s).
- **base1 hung** with `SOTC_GPU_CMD=0` at the title screen ("Press Any Button"), ~85 s in:
  `videoout: flip queue full`, then no more flips or `pipe:` lines, GPU tick frozen. With
  `=0` this session's code takes the same path as before (no scope ever opens; `Cmd()`
  records into the same primary command buffer), so it is an existing intermittent hang, not
  step 2b. Not reproduced in the other three `=0` runs. If it returns, catch it live
  (`scripts/crashcatch.py` or a debugger attached) and get A's and B's stacks.

**Step 3 next:** the C thread. Everything B records inside a record is now a command, and the
only points where B and the command buffer meet inside a record are the in-scope submits.

## 13. Step 3a: the C thread, with drains at submits (session 64)

`SOTC_GPU_CMD=1` now starts stage C (`sotc:GpuCmd`) at the first record's scope. `sync` stays
as in sections 11-12. Step 3 is split in two:
- **3a (this session):** C records the commands into the primary command buffer. B keeps
  everything else: the command pool, the session's begin and end, every submit, ticks and
  deferred operations.
- **3b (next):** rule 1 proper, which moves submits to C (section 5).

How 3a keeps Vulkan's external synchronization:
- **A drain is the only meeting point.** Before a submit (`FlushSubmit`), before a direct
  `Scheduler::CommandBuffer()` access (`Break`) and before `UploadCommandBuffer`, B publishes
  and waits until C has recorded everything (`SotcCmdState::Drain`). Only then does B touch the
  command buffer or the pool.
- **In thread mode the scope stays open between records.** `Close` publishes and wakes C. It does
  not shut the scope, so commands appended outside a record (barriers, copies, overlap and
  upload work between packets) are encoded too, and B never records directly while C may be
  recording. Only a drain shuts the scope, until the next record opens it. While it is shut, C
  is idle by construction: nothing can be appended.
- **Rule 1 holds trivially.** Every tick B submits was fully recorded first, so `Wait(tick)`,
  `IsFree(tick)` and deferred destroys need no change. **Rule 3 holds** for the same reason: C's
  lag never crosses a submit, and section 10 found no immediate destroys between submits.
- **Rule 6 (lag cap):** the submits cap it (~41 per flip plus hot flushes, ~170 records
  apart). The 4 MiB ring never filled (0 full-ring waits).
- **The ring's waits** (`SotcCmdRing`, tested in `test_cmd_ring`):
  - C spins 50 us for the next record, then sleeps on `head` (`Sleep`), and B wakes it only
    if it sleeps (`PublishAndWake`).
  - B spins 200 us in a drain, then sleeps on `tail` (`WaitDrained`).
  - `tail` and C's private fields now sit on separate cache lines: B reads `tail` on every
    `Reserve`.
- Verify mode (section 7) is not built yet. 3a's drains make rule 3 hold by construction, so it
  matters from 3b on.

**Altar, Pro mode, `flipcpu.py` (now with a C column), alternating, same exe (`s64-*`):**

| run | flips/s | A | B | C (ms/flip) |
|---|---|---|---|---|
| c1 (`=1`) | 21.53 | 24.4 | 36.9 | 33.2 |
| base1 (`=0`) | 18.75 | 27.1 | 43.7 | |
| c2 | 18.45 | 28.9 | 41.8 | 36.3 |
| base2 | 18.97 | 27.3 | 42.9 | |
| c3 | hung at the title ~66 s in (below) | | | |
| c4 | 19.32 | 29.3 | 40.7 | 34.9 |
| base3 | 18.55 | 27.6 | 44.9 | |
| c5 | died 2.4 s in (below) | | | |
| base4 | 17.49 | 28.3 | 47.8 | |

- **`=1` means: 19.8 flips/s, B 39.8 ms/flip. `=0`: 18.4 flips/s, B 44.8.** That is ~+7% flips/s
  and B ~5 ms/flip lighter, with a wide spread between runs. Section 10 predicted B losing
  ~10 ms (Vulkan calls minus the submits it keeps). B also still pays the encode (~2 ms), and A
  rises with `=1` (~28-29 vs ~27).
- **C's CPU is ~33-36 ms/flip, mostly its 50 us spin** (it records ~12 ms/flip of Vulkan calls).
  On a 6-core Ryzen the spinning competes with A, B and the guest threads, which may explain
  c2's and c4's smaller gains. Tune it first in 3b (shorter spin, or wake C once per N records).
- **Drains:** ~1 per flip outside submits (the presenter's `PrepareFrame`,
  `vk_presenter.cpp:698`, rule 5, as section 10 predicted), plus the ~220 in-scope submits per
  10 s. Those that found C still recording cost B ~2 ms per 10 s in all. There were no hot-flush
  storms.
- **The output is the same:** the altar screenshots of c1 and c2 match the baselines.
- **c3 hung** at the title ~66 s in: `flip queue full`, then no more flips or `pipe:` lines. That
  is the symptom of session 63's base1 hang, which ran with `SOTC_GPU_CMD=0`, so it is probably
  the existing hang, but that is not proven. This session added a **stall watchdog**
  (`Rasterizer::SotcStallWatch`, thread `sotc:StallWatch`, with `SOTC_GPU_PIPE=1`). When B runs
  no packet for 3 s (and every 10 s after), it logs a `stall:` line with:
  - what B and A (or a guest fault) are blocked on (`SotcWaitTimer::Where` markers: the wait
    timer's slots, B idle, B waiting for A's guard, A's pipe drain, B's drain of C);
  - B's tick, the known and the real GPU timeline value;
  - C's ring state.
  It did not fire in the four runs after it was added. Use it on the next hang (lldb on the
  Windows machine fails to start: a missing shared library).
- **c5 died** 2.4 s in, at the start of the main loop, with nothing in its log. Under
  `scripts/crashcatch.py`, the game dies at startup ~50% of the time **with both `=1` and `=0`**
  (4 of 6 and 3 of 6), almost always at the same guest site: a write at guest image
  `+0x579d4c` (once `+0x6832e3`, once `+0x39ad37a`) into direct memory around `0x10205xxxxx`
  that is reserved but not committed (`state 0x2000`). This is an existing startup race (a
  mapping not yet committed, or a split mapping that is briefly unmapped; `CLAUDE.md` "Known
  limits"), not stage C. The debugger's slower exception path makes it frequent. Not
  investigated yet.

## 14. Step 3b item 1: C's spin (session 65)
`SOTC_GPU_CMD_SPIN` (us, default 50) sets how long C spins for the next record before it
sleeps; `SOTC_GPU_CMD_WAKE` (default 1) wakes a sleeping C only every N scope ends (a spinning C
sees every publish; drains always wake it). The `pipe: cmd:` line now counts the scope ends that
woke C and the times C slept. Altar, Pro, `SOTC_GPU_CMD=1`, alternating, two clean runs each
(one 5 us run had a hot-flush storm, 760 denied per 10 s, and is left out; the startup crash of
section 13 killed 5 of 15 runs at 2.4 s):

| Setting | flips/s | B ms/flip | C ms/flip | C woken / slept per 10 s |
|---|---|---|---|---|
| spin 50 (default) | 21.1, 22.4 | 37.8, 36.7 | 32.3, 32.9 | ~8.7k / ~7.4k |
| spin 20 | 20.0, 22.6 | 39.7, 36.9 | 31.6, 31.3 | ~21k / ~17k |
| spin 5 | 20.9 | 38.6 | 30.5 | ~144k / ~142k |
| spin 0 | 20.8, 19.9 | 39.2, 41.2 | 27.4, 28.4 | ~1.2M / ~1.2M |
| spin 5, wake every 4 | 19.5, 20.9 | 39.4, 38.1 | 26.9, 26.1 | ~73k / ~79k |

- **The spin is not where C's CPU goes.** Without any spin C still uses ~27-28 ms/flip,
  because it then sleeps and wakes ~120,000 times a second (~1.5M scopes per 10 s, ~80% of
  them wake it). Its real recording work is more than the ~12 ms/flip of section 13's Vulkan
  time.
- **Shorter spins cost B** (+1.5 to +3.5 ms/flip): every scope end that finds C asleep pays a
  `WakeByAddress`. B is the limit, so flips/s follow B. Batching the wakes (every 4) took C to
  ~26 ms/flip but did not give B its time back: still ~73k wakes per 10 s (9x the default's),
  and B's drain waits rose only slightly (30-36 vs ~21 ms per 10 s).
- **Decision: keep 50 us and wake every scope** (the defaults). The two variables stay as
  tuning knobs. The idea that C's spinning cost c2/c4 their gain on the 6-core Ryzen is not
  supported: lower C CPU did not raise flips/s.

## 15. The indirect-draw probe (session 66)
Question (user, session 65): do many of B's indirect draws draw nothing? Their arguments come
from the game's compute shaders, so a GPU-culled object could be a draw with count 0 that B
records in full (~5 us). `SOTC_INDIRECT_PROBE=1` (debug only, `sotc_indirect_probe.{h,cpp}`)
copies each indirect draw's and dispatch's argument words (and the count, for the count forms)
into a host-cached slab with encoded `copyBuffer`s between two global barriers, and reads the
slab once its tick is free (never waiting). A probed draw ends its render pass first (copies
can't run inside one), so frames stay the same but GPU timing doesn't: not for measuring speed.
It also counts the `ZPASS_DONE` dumps (occlusion queries) Liverpool sees. The
`probe: indirect:` line, every 10 s; one run, Pro mode, pad script:

| Scene | indirect draws/flip | empty | indirect dispatches/flip | empty |
|---|---|---|---|---|
| title | 3-6 | 100% | 6-10 | 0% |
| intro, outdoors (41-61 s) | 1,600-1,800 | 4-4.5% | 40 | 0% |
| loads/cutscenes (71-101 s) | 90-610 | 18-54% | 43-94 | 9-17% |
| altar (110-330 s, steady) | ~3,530 | **12.2%** (~430/flip) | ~360 | 9.7% (~35/flip) |

- **Few.** At the altar ~430 records per flip draw nothing: at ~5 us each, at most ~2.2
  ms/flip of B (~5%), not the ~9 ms hoped for. Every empty draw has a zero vertex/index or
  instance count in a single-draw packet: the game uses neither the multi nor the count forms
  (`multi 0`, `count 0: 0`).
- **No occlusion queries:** 0 `ZPASS_DONE` dumps in the whole run, so nothing is culled on the
  CPU from faked "always visible" results.
- The empties concentrate in a few vertex shaders: `a2a164eb` and `f5366412` (~11.5k of ~92k
  and ~11.3k of ~90k per 10 s; a pair, likely the same objects in two passes), `eb9444e2`
  (~70% empty), `54929ee8` (~68%), `ea52859e` and `2a6e88d3` (always empty).
- **Last flip as a hint:** of the records compared with the one at the same argument address a
  flip earlier, ~27-31k per 10 s stayed empty, 1-4.5k turned empty and as many turned live. A
  skip on "empty last flip" would draw 3-14% of those wrongly as nothing, so it needs a check;
  ~35% of the empties share their address with another record in the same flip and have no
  such history.
- **Direct calls (66b, user):** the probe also counts direct draws with a zero index/vertex or
  instance count and direct dispatches with a zero group count (B knows these at record time,
  so a skip would be exact), on a `probe: direct:` line. Altar: ~940 direct draws/flip, **none
  empty** in the whole run; ~2,300 direct dispatches/flip, 0.3% empty (~7/flip). Together with
  the indirect ones that is ~7,130 records/flip, matching session 61's ~7,200. Nothing there.
- **Decision:** not worth a fix now; back to step 3b item 2 (submits on C). A cheap record path
  for the always-empty shaders (`ea52859e`, `2a6e88d3`, the title's) is a possible later trim
  (~0.3 ms/flip).


## 16. Step 3b item 2, the plan: submits on C (written after session 66)
Today (3a) every submit is a drain: B waits for C to record everything, then ends the command
buffer, submits and begins the next one itself (~41 submits per flip plus hot flushes; ~2
ms/flip of B in `submit`/begin/end, section 10). Item 2 moves that work to C. **Expected: B -2
ms/flip or so (~5%)**; the drains themselves cost B little (section 13: ~2 ms per 10 s).

What the code does at a submit today (`Scheduler::SubmitExecution`), and where each piece goes:

| piece | today | item 2 |
|---|---|---|
| `on_submit`: `Runtime::FlushBarriers` | B, records | B, encoded (already) |
| `on_submit`: `SotcPrefetchAtBatchEnd` | B, `CommandBuffer()` (a drain) | opt-in (`SOTC_PREFETCH_READBACK`): leave it a counted drain, or encode it via `Cmd()` |
| `on_submit`: `SubmitPendingArenaBinds` (`bindSparse` on the graphics queue, adds a wait to `info`) | B | **stays on B** under `submit_mutex`. Its binds then reach the queue before C submits the batches still in the ring: harmless, because arena residency only grows (session 56) |
| `work_semaphore.NextTick()` | B | B (ticks belong to B, rule 1) |
| `EndSession` (`EndRendering`, `primary.end()`), the batch's command-buffer list, `GpuTimer` buffers, `queue.submit` under `submit_mutex`, `BeginSession` (`command_pool.Commit`, `begin`, `sotc_cmd.raw`) | B | **C**, at a new `Submit` command (payload: `SubmitInfo` by value, the signal value, the call site for the `submits:` line) |
| `dynamic_state.Invalidate`, the bind memo reset, `PopPendingOperations` | B | B, at the point it appends `Submit` (they are B's view of "a new command buffer") |
| `UploadCommandBuffer` | no caller left outside the scheduler | delete it (and its `upload` half of a session) |
| Tracy GPU zones (`TRACY_GPU_ENABLED`) | B | C, or off with `SOTC_GPU_CMD=1` |

New rule (the one that can hang the GPU): **a GPU-side wait on the draw scheduler's timeline
must not reach a queue before C submitted the signal.** Host waits are fine as they are
(`Semaphore::Wait` loops on `vkWaitSemaphores`, wait-before-signal is legal), and `IsFree`
needs no change (an unsubmitted tick can't be signalled). But the present thread's submit waits
on `frame->ready_tick` on the **same** graphics queue (`vk_presenter.cpp`, `Present`): queued
before C's signal, it would block the queue behind itself. So:
- `Semaphore` gains `submitted` (atomic; stored after every `queue.submit` in all modes) and
  `WaitSubmitted(tick)` (atomic wait, counted in `pipe: B waits` / the present thread).
- Callers that put a draw-timeline value into another submit call it first: the present path
  (`ready_tick`), the flip path, the side-queue readback (`write_tick`, rule 4; a different
  queue, so legal anyway, but keep it simple).
- `SubmitExecution` asserts in debug builds that every wait on a known `Semaphore` is submitted.

Other rules:
- **Every `Submit` publishes and wakes C at once** (no `SOTC_GPU_CMD_WAKE` batching), since B
  may wait on that tick right after (`Finish`, `Wait`, a fault on the batch being built).
- **`Drain` waits for the `Submit`s too**, so `CommandBuffer()` after a drain (the presenter's
  `PrepareFrame`, 1/flip) sees C's current primary buffer. Only the drain hands it over.
- **The pool is C's** (`command_pool.Commit` tags by `CurrentTick()`, an atomic of B's: a C-side
  commit tags one tick high at worst, which only delays reuse).
- **C's lag now crosses ticks.** The 4 MiB ring caps it (never full so far); add the number of
  ticks C is behind to the `cmd:` line and to the `stall:` line (B's tick, C's submitted tick,
  the GPU's).
- **Rule 3** (section 5): section 10 found no immediate destroy of an appended handle; verify
  mode below checks it from now on.

Verify mode (item 3, same session if it fits): `SOTC_GPU_CMD_VERIFY=1`. B records, per appended
handle (buffer, image, view, sampler, pipeline), the ring sequence of its last use; the
destructors that already call `SotcDescCache::Dead` (`ImageView`, `Sampler`, `UniqueBuffer`)
also check that C's decoded sequence passed it, else log `cmd: <kind> destroyed before C
recorded it (B at N, C at M)`. Off by default; a hash map on B only.

Build order and checks:
1. `Semaphore::submitted`/`WaitSubmitted` and the three callers, in every mode (no behaviour
   change with `SOTC_GPU_CMD=0`; run the altar once).
2. The `Submit` command and C's end/submit/begin; delete `UploadCommandBuffer`. Altar
   screenshots against `=0` and against 3a.
3. Verify mode, one altar run with it on: no `destroyed before C recorded it`.
4. Measure: `flipcpu.py`, alternating 3a (this build with item 2 off, e.g. a
   `SOTC_GPU_CMD_SUBMIT=0` switch) vs item 2, B and C columns, `pipe: cmd:` drains (should drop
   to ~1/flip) and `pipe: A waits` (C's lag may delay fences). Budget reruns: the startup crash
   (section 13) kills ~1 run in 3.
5. Then item 4: the default, and Linux.
