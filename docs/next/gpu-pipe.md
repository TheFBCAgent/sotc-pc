# Two-stage GPU pipeline: design (session 42, 2026-10-12)

Goal: split today's single GPU thread (`shadPS4:GpuCommandProcessor`, the critical path in
gameplay on every machine) into two threads that run one packet stream in order. Behind
`SOTC_GPU_PIPE` (default 0 = today's path, unchanged), so a threading bug (stale data, flicker)
is told apart from the open rendering bugs in one run.

Model: bbport's `vk_draw_pipe.h` (`docs/parallel_gpu.md` in `bratdr/bloodborne_pc`): +19% first
version, +32% with its constant ring. Its pitfalls are listed at the end; SotC adds three of its own
(the SRT walker's GPU-written tables, the CPU copy, `WaitRegMem` on fresh fences), measured below.

## Where the GPU thread's time goes (perf, session 42)
Temple, `build-vk` at `aa4cbd1`, 22 flips/s, `perf record --call-graph dwarf` on the GPU thread
for 10 s at 140 s (`s42-perf`, 7,929 samples). Inclusive shares of its on-CPU time:

| piece | share | stage |
|---|---|---|
| `Draw` / `DrawIndirect` / `DispatchDirect` / `DispatchIndirect` | 13.5 / 41.6 / 26.5 / 5.9 (87.5) | |
| - pipeline selection (`GetGraphicsPipeline` 8.6, `GetComputePipeline` 2.4; SRT walker + `ResolveSrtTable` ~4, `StageSpecialization` 5.7) | 11.0 | A |
| - `SotcCpuCopy` | 6.1 | A |
| - `BindResources` (`BindBuffers` 30.6 of which `ObtainBuffer` 19.5 and clamp 2.0; `BindTextures` 9.6) | 46.4 | B |
| - `PrepareRenderState` 2.3, `BeginRendering` 2.2, `ResetBindings` 4.0, `BindIndexBuffer` 2.1, `UpdateDynamicState` 1.6, `BindVertexBuffers` 0.6, `IsCompute*` 0.8, `PopPendingOperations` 0.8 | 14.4 | B |
| - the rest (descriptor push, `vkCmd*`) | ~9.6 | B |
| RADV (`libvulkan_radeon`, inside the rows above) | 14.9 | B |
| `ProcessCommands` (guest-fault downloads) | 2.2 | B |
| PM4 decode, register writes, fences, kernel | ~10 | A (decode), B (fences) |

So stage A (decode + registers + pipeline selection + CPU copy + building packets) is ~25% of
today's work and stage B ~75%: **ceiling ~1.3x the GPU thread's throughput** (temple ~22 ->
~28 flips/s), the size of bbport's gain. Balancing later moves work from B to A (below).

## What would drain (temporary probe, session 42, removed)
`s42-probe` (same scene, ~19.3 flips/s under the probe): every read stage A makes of guest
memory was checked against the writes of the last 32/128/512/2048 records (a lag stage B might
have). Per flip (~7,160 records):

| A-side read | per flip | newest overlapping write <= 32 / 128 / 512 / 2048 records back |
|---|---|---|
| SRT walker tables (`ResolveSrtTable`) | ~44,800 | GPU-written binding: **0 / 0 / 0 / 0**; CPU-copy destination: 146 / 575 / 1,355 / **3,390** |
| CPU copy control / source | ~265 / ~265 | none |
| `WaitRegMem` labels | ~296 | fence label (EOP/ReleaseMem): **221** / 227 / 239 / 252 |
| predication, `CondExec` | 0 | (`SOTC_PRED` is `run`; no `CondExec`) |

Packets per flip: EOP ~225, ReleaseMem ~144, EOS ~19 (none `GdsStore`), `SET_PREDICATION` ~22,
**no** `WriteData`, non-trivial `DmaData`, `DumpConstRam`, `MemSemaphore`, `IndirectBuffer`,
`CondExec`. Guest-fault downloads ~21 per flip and the GPU thread's own fault downloads ~5 per
flip (`s41-base-temple`, `SOTC_GPU_STATS=1`).

Consequences, each one a rule below:
1. The walker never read a table that in-flight GPU work writes. The GPU-written tables it
   downloads (~5 per flip) were written more than 2,048 records earlier.
2. The walker reads the CPU copy's destinations constantly, within a few records. **The CPU
   copy stays on stage A, in stream order**, or stage A would drain ~3,400 times a frame.
3. Most `WaitRegMem` wait for a fence a few packets earlier in the stream. If fences run on
   stage B, **a wait whose label a pending fence writes counts as met**, as in bbport. A drain
   there would happen ~250 times a frame.

## Threads and ownership
- **Stage A** = the existing GPU command thread (`shadPS4:GpuCommandProcessor`): the queues and
  their coroutines, PM4 decode, the register file, CE, predication, `WaitRegMem`/`Rewind`
  polling, pipeline selection (`GetGraphicsPipeline`/`GetComputePipeline`: SRT walker,
  `StageSpecialization`, async compiles, the session 31 dispatch scan), and the CPU copy's
  computation.
- **Stage B** = a new thread `sotc:GpuRecord`: the rasterizer's draw/dispatch bodies after
  pipeline selection, the buffer and texture caches, the scheduler and `Runtime` (barriers,
  uploads, downloads), the presenter and flips, the GC tick, the liverpool command queue
  (`SendCommand`: guest-fault downloads, video-out registration, flips), fences and IRQs.
- While B runs, A only touches B's state through packets or after a drain. A drain = A waits
  until B has run every packet handed over; B is then idle and A may call into B's state.

## The packet stream
SPSC ring (16 MiB, bbport's size), A writes and B reads in order; release on commit, acquire on
read; B sleeps on a futex/condvar when empty, A when full (no spinning: the Deck has 4 cores).
`SOTC_GPU_PIPE_DEPTH` caps packets in flight (start at 1024: rule 1 above holds up to 2,048).

Packets:
- **Draw / indexed / indirect / indirect-count**, **dispatch direct / indirect** (every kind:
  indirect draws are ~50% of the records, dispatches ~37%): the register blocks written since
  the previous packet (32-dword blocks; `Regs` is 0xD000 dwords, 208 KiB, so never whole), the
  CB/DB extent hints when changed (`last_cb_extent`/`last_db_extent`), the pipeline pointer, per
  stage the `Shader::Info*` plus a snapshot of its user data, flattened user data, `pgm_base`,
  `sotc_flat_patches` and fetch-shader pointer, the draw parameters; dispatches also carry the
  queue id and the `ComputeProgram` registers (`GetCsRegs`).
- **Fence**: EOP, EOS (with `GdsStore`: B finishes and reads GDS), ReleaseMem, and `WriteData`/
  `DmaData` if a game ever sends them: run on B in order. B writes the label and raises the IRQ.
- **CPU-copy commit**: the destination, the computed dwords, the plan (below).
- **Submit done**: per `ProcessCommands`/`submit_done` round: B runs `OnSubmit` + `Flush` and
  signals `GpuIdle` (bbport's heap corruption came from signalling idle before deferred fences).
- **Marker**: guest debug markers (`ScopeMarker*`, off by default).
- Predication: skipped packets are never handed over (decided on A as today).

B reads registers through `Rasterizer::Regs()`, `CbExtent(i)`, `DbExtent()`, `CsRegs()` (its
copy with the pipe on, Liverpool's with it off) and shader user data through accessors on
`Shader::Info` that return the snapshot installed for the current packet (thread-local), never
the shared members A keeps overwriting. That refactor is mechanical: 28 `liverpool->regs`, 5
`GetCsRegs` and 7 extent reads in `vk_rasterizer.cpp`; `GetSharp`/`ReadUdSharp`/`ReadUdReg`.

## Rules (how the CLAUDE.md invariants hold)
- **`gpu_modified_ranges` + `SotcWrittenPages`**: owned by B; every `Add`/`Subtract` (and
  `Mark`/`Unmark`) happens on B, the CPU copy's `SotcForgetGpuWrites` included (in its commit).
  A asks "GPU-written?" (walker tables ~860k/s, CPU-copy control and source) as: (1) A's own
  **pending-writes set** (the written ranges of packets B hasn't retired: a 4 KiB page counter
  table + a ring of ranges, A-only); (2) the bitmap, whose words become atomic (B `fetch_or`/
  `fetch_and`, A relaxed loads); (3) only when the bitmap says maybe, `gpu_modified_ranges`
  under a new mutex B also takes for `Add`/`Subtract` (~150k/s, uncontended). B marks before
  it retires a packet (release store of its sequence number), and A reads pending before the
  bitmap, so a write is never missed in between. A pending hit drains B first, then the normal
  path runs (rule 1: expected 0 per frame). A packet's written ranges are computed on A from the
  stage's buffer sharps and `is_written` with the clamp, and B takes A's clamped sizes instead of
  clamping again (`hle_memory_clamp_range` is already thread-safe, session 33).
- **SRT walker tables with no known extent** (`bytes == 0`, read at `ptr`): checked as the 4 KiB
  page(s) at `ptr` against pending writes.
- **CPU copy**: A computes it (plan, control and source read through `CopySparseMemory`, both
  checked as GPU-written above; a source inside a pending CPU-copy destination is read from that
  overlay) into a buffer owned by the commit packet. B commits in order: residency split
  (`SotcResident`), `TryWriteBacking`, `SotcForgetGpuWrites`, `SotcSetCpuMirroredWrite`. Until B
  has retired the commit, `ResolveSrtTable` on A returns a pointer into that buffer for a table
  inside the destination (same values as the GPU writes, checked by `SOTC_CPU_COPY=2` since
  session 21), so the walker sees stream order. A never writes guest memory that earlier packets
  still read. The image-source check (`IsMeta`, `FindImageFromRange`: texture-cache state, and
  `FindImageFromRange` frees downscaled images) moves to B's commit; a source that turns out to
  be an image there is counted and logged (`pipe: cpu copy source became an image`; 0 rejections
  in the temple today).
- **`SotcFindCache` / `RegisterImage`, `sotc_uncached_finds`, `ClearMeta`/`TouchMeta`
  (`sotc_meta_clears`)**, the render-state and texture-set memos: texture-cache and rasterizer
  state, B only. A calls no texture-cache function (the `FilterDraw` specials run on B).
- **Fault path**: B is the "GPU-side thread" whose faults are handled inline (`assume_locks`;
  `GuestFaultSignalHandler`, `sotcgpu_handle_fault` test B's thread id with the pipe on). A's
  own faults are queued to B like a guest thread's, and A waits for the result; the walker's
  emulated loads and table resolver keep testing A's id. Host code on A reads guest memory only
  through `CopySparseMemory`/resolver aliases, as today.
- **PM4 packet memory after a signal**: B never reads PM4 memory; A copies what B needs into
  the packet. Signals move to B, after A has stepped past the packet with the count it read
  first (session 29), so the rule keeps holding. A's per-submission `on_done` (the runtime
  advances ring read pointers) fires when A has consumed the buffers, as today.
- **Guest-visible order**: fences (and so the guest's view of "the GPU finished") are signalled
  by B after it recorded everything before them, as the single thread does now. A reads guest
  memory earlier than B does, but always after the submission that references it.
- **`WaitRegMem`**: A keeps label -> value of pending fence writes; a wait that value satisfies
  is met (B runs the fence before any later packet). With no pending write to the label, A polls
  memory as today (it waits for the CPU, not for B). A pending write whose value fails the test:
  drain, then poll.
- Untouched: `kPipelineCacheVersion` (no SPIR-V change), `plat_query_prot`, one GPU process.
  New `third_party/shadps4` patches get README rows as usual. `SotcDebug` counters are atomic
  already; the fault tables and other "GPU thread only" diagnostics belong to B.

## Drain points
| event | per flip (temple) | handling |
|---|---|---|
| A read hits a pending write (walker, CPU copy, labels) | 0 measured | drain, then today's path |
| A's own fault download (walker on an old GPU-written table) | ~5 | queued to B, A waits |
| `WaitRegMem` failing on a pending fence value | rare | drain, then poll |
| guest-fault download (`SendCommand<true>`) | ~21 | B between packets; no drain (valid at B's position) |
| flip, video-out registration | 1 | B command queue |
| unmap from the guest | rare | drain (B may hold packets that use the range) |
| shutdown, `SOTC_DUMP_RT`, screenshots | rare | B; drain where they read A state |

Each drain is counted by reason and call site (bbport found its one per-frame lockstep drain
that way); the stats line prints drains, A waiting, B busy.

## Verify mode
`SOTC_GPU_PIPE_VERIFY=N`: every Nth packet also carries the full register file; B reports words
where its delta-built copy differs. And per stage, B re-reads the flattened user data's sharps
from guest memory (guarded: pointers may be stale) and compares them with A's snapshot. Both
stay in the code (one branch when unset).

## Build order (one step per session; A/B with `flipcpu.sh --env SOTC_GPU_PIPE=1` vs `=0` on
the same exe, alternating; screenshots compared at the altar and the temple)
1. **Plumbing, no thread**: register deltas (`Liverpool` marks 32-dword blocks in every register
   write path, `ClearState` included), `Rasterizer::Regs()`/`CbExtent()`/`DbExtent()`/`CsRegs()`,
   the user-data snapshot accessors on `Shader::Info`, the packet format and ring, and a
   synchronous mode (`SOTC_GPU_PIPE=sync`: A builds each packet and B's code runs it on the same
   thread at once). Same output as today; tests for the delta replay and the ring.
   **Done (session 43)**, see "Step 1 as built" below.
2. **Stage B thread** for draws and dispatches; everything else drains, and the CPU-copy
   dispatch drains after its packet (correct but slow, like bbport's first version). Verify mode.
   `flipcpu.sh`/`flipcpu.py` then also sum `sotc:GpuRecord`'s CPU (they read only `GpuComm`);
   flips/s becomes the headline number. **Done (session 44)**, see "Step 2 as built" below.
3. **Fences, submit-done/idle, flips and the command queue on B**; pending fence values for
   `WaitRegMem`; A's faults queued to B. **3a done (session 45)**: fences, submit-done, idle,
   flip IRQ, pending fence values; see "Step 3a as built". **3b done (session 46)**: the
   command queue and A's faults on B; see "Step 3b as built".
4. **Pending-writes set** on A (walker, unknown-extent tables, CPU copy inputs) and the
   concurrent `IsRangeGpuWritten` (atomic bitmap, mutex on the set); **CPU copy on A** with the
   overlay. Then the measured A/B, and Windows confirmation.
5. Balance: B will be the limit (~75%). Move pure work from B to A: the stream copies of
   small read-only buffers into a ring A owns (bbport's constant ring, +11% there; our
   `OB stream copy`), the clamp, vertex/index range resolution. Each step measured.

60 fps in heavy views needs ~2.7x (session 41); two stages give ~1.3x. After them come
draw-preparation workers (bbport +11.5%), the session 40 buffer-side items and the
per-draw memos, all measured in Pro mode (`SOTC_NEO=1`, no 30 fps cap).

## bbport pitfalls to avoid
- Signalling `GpuIdle` before deferred fences: the guest freed live objects (heap corruption
  after minutes). Idle goes through B.
- Draining at every non-draw packet (their first version: ~80k/s no-op `DmaData`). Ours: the
  `0x3022C` `DmaData` is already skipped before the switch.
- A worker reading a V# the guest was still writing asserted in `SurfaceFormat`: A-side sharp
  reads must not assert on garbage (`TrySurfaceFormat`-style checks, or leave the draw to B).
- Per-draw fork/join helpers lost: shared data moves between cores. Stages must own their data
  for long stretches.

## Step 1 as built (session 43, 2026-10-12)
- `video_core/renderer_vulkan/sotc_gpu_pipe.h` (Vulkan-free, `gpu/tests/test_gpu_pipe.cpp`):
  `SotcDirtyBlocks` (1,664 blocks of 32 dwords, starts all dirty), `SotcEncodeRegDelta`/
  `SotcApplyRegDelta`, `SotcPacketWriter`/`Reader`, `SotcPacketRing` (16 MiB, record cap =
  `SOTC_GPU_PIPE_DEPTH`).
- `Liverpool::sotc_dirty` is marked in every register write path (`SotcMarkRegs`,
  `SotcMarkField`; `ClearState` marks all). `SOTC_GPU_PIPE_VERIFY=1` over a 200 s run to the
  altar (`s43-verify`, every packet compared): **0 differences**, so no write path is missed.
- Rasterizer: every B-side register read goes through `Regs()`/`CbExtent()`/`DbExtent()`/
  `CsRegs()` (28 + 7 + 5 sites); `FilterDraw` = `ClassifyDraw` (A) + `RunDrawFilter` (B); each
  draw/dispatch entry point = selection + `Record*`. `SotcPipeSubmit` (A) / `SotcPipeRun` (B).
- `Shader::Info::UserData()/FlatUd()/PgmBase()/FlatPatches()` return the thread's installed
  `UdSnapshot` for that stage (`SotcUdScope`, a thread-local span, scanned by `Info*`); every
  `GetSharp`, `ReadUdSharp`, `ReadUdReg`, the flat-buffer upload and its patches read through them.
- Order change vs off: B's `PopPendingOperations` now runs after A's pipeline selection for the
  same draw (before it, with the pipe off). Rendering unchanged; keep in mind for step 2.
- Cost (temple, `flipcpu.sh`, same exe, alternating): `sync` 36.70/36.44 vs off 35.43/35.95
  ms/flip (**+2.5%**), flips/s 21.62/21.73 vs 22.05/21.94. What is left of it (perf `s43-psync`
  vs `s43-pbase`): `SotcPipeSubmit` ~1.1%, `SotcPipeRun` ~1.0%, delta encode ~1.0%, the packet
  copy (writer, then ring) and the flat-buffer copies ~0.7%. Writing the packet straight into the
  ring would drop one copy; with the thread on, Submit and encode are on A, which has slack.
- Ring lesson: a seq_cst store per `Pop` (to pair with the sleeping producer's flag) waited for
  the store buffer B's recording had just filled, ~180 ns per packet (3.7% of the thread). The
  consumer now publishes its position every 32 packets (at most half the depth cap) and before
  it sleeps; `notify` only when the other side announced it sleeps. Sync mode never sleeps, and
  `SotcPipeSubmit` publishes before a packet over 4 MiB so the one thread can't block on itself.

## Step 2 as built (session 44, 2026-10-12)
- `SOTC_GPU_PIPE=1` starts `sotc:GpuRecord` (`Rasterizer::SotcPipeThread`): it loops on the
  ring, runs each packet (`SotcPipeRunPacket`, shared with sync mode), and when the ring is
  empty stores `retired` (seq_cst) and wakes a draining A before it sleeps. A `Stop` packet
  from `~Rasterizer` ends it.
- `SotcPipeDrain(reason)` (A only; a no-op on other threads, in sync mode and with the pipe off):
  waits until `retired == submitted`. Called before `OnFence` (so every fence, label and IRQ is
  signalled after B recorded what precedes it), `OnSubmit`, `Flush`, `Finish`, `ReadDataFromGds`,
  `FillBuffer`/`CopyBuffer`, the markers, `SotcGcTick`, each command-queue callback, the
  `PatchedFlip` IRQ, `GpuIdle`, and after a CPU-copy dispatch packet (rule 2).
- Faults: `GuestFaultSignalHandler` asks `SotcFaultOnGpuSide()`: on A it drains, then the fault
  is handled inline as before; on B inline; elsewhere queued to A's command queue (which drains).
  The SRT walker's emulated loads and table resolver still run on A only. The uffd path (off in
  sotcgpu) still compares with A's tid.
- The walker's `IsRangeGpuWritten` from A while B records: `SotcWrittenPages` is atomic (one
  writer, concurrent readers; `test_written_pages` has a concurrent reader), and every
  `gpu_modified_ranges` change takes `sotc_ranges_mutex`, which the interval search takes too.
  A may still see a write B hasn't recorded yet: rule 1 (0 measured within 2,048 records); the
  pending-writes set is step 4.
- Verify mode now carries Liverpool's whole register file in every Nth packet (A's view when
  it sent it). `s44-verify` (`=64`, 200 s to the altar): **0 differences**, no errors; the altar
  renders like pipe off.
- Drains per flip (temple, `pipe:` lines): fence ~390 (all EOP/ReleaseMem/EOS), CPU copy ~265,
  command ~33, fault ~6, flip 2; ~75% of them find B busy. A waits ~530 ms per second and B
  sleeps ~280 ms per second: the two stages mostly take turns.
- Cost (temple, `flipcpu.sh`, same exe, alternating): **pipe 22.67/22.27 vs off 22.12/21.93
  flips/s (+1.7%, at noise level)**; GPU-thread CPU 44.14/45.44 ms/flip (A 12.80/13.34 + B
  31.34/32.10) vs 35.52/35.27. B alone is ~89% of the single thread and A ~37%; the ~9 ms of
  extra total CPU is the hand-over (futex waits and wakes, packets, cold caches). Step 3 (fences
  on B, pending fence values for `WaitRegMem`) removes ~390 of the ~730 drains per flip, and
  step 4 the CPU-copy ones (~265).

## Step 3a as built (session 45, 2026-10-12)
- EOP, EOS and ReleaseMem moved into `Liverpool::SotcRunFence` (OnFence, label write, GDS
  store/copy, IRQ). With `SOTC_GPU_PIPE=1`, `Rasterizer::SotcPipeFence` copies the PM4 packet
  into a `Fence` packet and B runs `SotcRunFence` on the copy, so B never reads PM4 memory and
  the guest sees a fence only after B recorded what precedes it. Off and `sync`: inline as before.
- A notes each fence's label write in `SotcPendingFences` (`sotc_gpu_pipe.h`, test
  `TestPendingFences`): known for `Data32Low`/`Data64` EOP/ReleaseMem and EOS `SignalFence`,
  unknown for clocks, EOS `GdsStore` and ReleaseMem `GdsMemStore`. B stores `fence_retired`
  (release) after each fence packet; A forgets writes up to it.
- `WaitRegMem` (gfx and ASC) asks `SotcPipeWaitMet` before polling and after each yield (another
  queue may queue the fence meanwhile): met when the newest pending write covering the dword
  passes `PM4CmdWaitRegMem::TestValue`; a failing, unknown or partial one drains (`wait-fence`)
  and memory is polled. In the temple: ~5,300 waits/s met, 0 `wait-fence` drains.
- `SotcPipeDefer` sends submit-done (`OnSubmit` + `Flush`), `GpuIdle` and the `PatchedFlip`
  `GfxFlip` IRQ as `Event` packets: `SubmitFlipInternal` (presenter, GC tick) now runs on B.
  `WriteData` still drains (0 per flip).
- `s45-verify` (`=64`, 200 s to the altar): **0 differences**, altar renders right.
- Drains per flip: ~730 -> ~300 (cpu-copy ~265, command ~28, fault ~5-7); fence gone.
  **Fault drains got dearer**: A now runs up to the depth cap ahead, so each of A's own faults
  waits for B to finish everything queued (~1.1-2.5 s of waiting per 10 s in the temple vs.
  ~0.3 s before). Step 3b (A's faults queued to B, handled between packets) removes that.
- Cost (temple, `flipcpu.sh`, same exe, alternating): **pipe 24.62/24.81 vs off 20.89/22.56
  flips/s** (+9.8% over the better baseline; session 44's pipe was 22.67/22.27); GPU-thread
  CPU 41.46/38.54 ms/flip (A 11.99/10.81 + B 29.47/27.73) vs 36.21/34.83.

## Step 3b as built (session 46, 2026-10-12)
- `Liverpool::SendCommand` -> `QueueCommand`: with `SOTC_GPU_PIPE=1` the command goes to B's
  queue (`Rasterizer::SotcPipeCommand`), else to Liverpool's as before. B runs queued commands
  between packets and when it wakes (`SotcPipeRunCommands`). The ring's consumer now sleeps on
  a `wakes` counter that `Push` and `Kick` bump (`WaitFront(other_work)`: returns an empty span
  for side work), so a command wakes an idle B; test `TestRingKick` (packets and kicks from two
  threads, no lost wake-up).
- Ownership: B holds `SotcPipe::state_mutex` while awake and releases it before it stores
  `retired` and sleeps. `SotcPipeDrain` waits as before, then takes the mutex and returns a
  `SotcPipeOwner` guard (nested guards count `own_depth`; a drain while owning with packets
  queued releases the mutex while it waits). Every drain site that touches B's state keeps the
  guard for its scope (`Flush`, `Finish`, `OnSubmit`, `OnFence`, `SotcGcTick`, DMA, GDS,
  markers); the CPU-copy and `wait-fence` drains only wait (`SotcPipeWait`).
- Faults: `SotcFaultOnGpuSide` is true on A only while it holds a guard (B is blocked then, so a
  queued fault would never run); otherwise A's faults go through `SendCommand<true>` to B like a
  guest thread's and A waits for that one command. Rule: never `SendCommand<true>` from B or
  from A holding a guard. The `command` and `fault` drains are gone.
- `s46-verify` (`=64`, 200 s to the altar): **0 differences**, no errors, altar renders right.
- Temple (110-195 s): ~900 commands/s on B (~37 per flip), ~120-165 A faults/s queued, A waits
  **~16-21 ms per second** for them (session 45's fault drains: ~110-250). What remains is the
  CPU-copy drain: ~6,300/s, **~300 ms of A waiting per second**, B idle only ~34 ms/s.
- Cost (`flipcpu.sh`, same exe, alternating): **pipe 24.06/23.51 vs off 22.08/22.11 flips/s
  (+7.6%)**; GPU-thread CPU 43.12/45.12 ms/flip (A 11.15/12.13 + B 31.98/32.98) vs 35.54/35.28.
  No gain over session 45's pipe (24.62/24.81, measured against a noisier baseline): the fault
  waits were not on the critical path. The stages still take turns at every CPU copy (~265 per
  flip): step 4 removes that.
