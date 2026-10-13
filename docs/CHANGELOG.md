# Changelog

One line per fix, oldest first: what it did, what it measured and, for speed changes, whether it
is on by default and which variable turns it off. The full write-up of each session (runs, logs,
numbers) is in [NEXT.md](NEXT.md) (sessions 37 on) and [history/](history/) (sessions 3-36); the
variables are in [ENV.md](ENV.md); every patch to shadPS4's code has a row in
[third_party/shadps4/README.md](../third_party/shadps4/README.md).

Speed numbers are flips per second (fps) or GPU-thread CPU milliseconds per flip, in the temple
or at the altar, from alternating fix/baseline runs. Machines: Windows = Ryzen 5 3600 + RTX 2060,
Linux = Ryzen 5 5600 + RX 9070 XT (RADV). "No gain" means the difference was inside the run-to-run
noise.

## Milestones 1-2 (2026-10-06)

- **M1:** the boot-image converter (`tools/sotcconv`) and the native runtime: the game's own
  x86-64 code runs on the PC, PS4 system libraries through HLE.
- **M2:** the game reaches its main loop on a null GPU (threads, files, services, event queues,
  fibers, dialogs, save data).

## Milestone 3: Vulkan bring-up and correctness (2026-10-07 .. 10-11)

| Session | Fix |
|---|---|
| bring-up | shadPS4's `video_core` behind `sotcgpu`; device loss, flip order and flip rate fixed. |
| bring-up | The intro renders (GPU readbacks), stream-buffer fix, FPS in the window title. |
| bring-up | Generic USB pad mappings, keyboard keys, `sotc-padprobe`. |
| 3 | Gameplay reached. |
| 4 | Silent crash after "Continue": fiber stacks pinned against GPU page protection. |
| 5 | Black boot fixed (SRT walker: null pointers read zeros); VRAM working-set diagnostics. |
| 6 | VRAM exhaustion: emergency image GC, dedicated image allocations. |
| 6b | Crash C: the fault path hashes guest memory through `CopySparseMemory`, never in place. |
| 7 | Black debris, rocks and grass: tiled GPU-written images kept in the image GC. |
| 8 | Blocky depth of field: runtime-indexed `ReadConst`s served from an SRT window. |
| 9 | Horizontal stripes on shaded surfaces: wave64 `ReadLane`s in branches lowered right. |
| 11 | Posterized, blown-out sky: a wave32 `ReadLane` past the workgroup reads the reduction identity; LDS subgroup barriers. |
| 27b | Linux build (PIC, POSIX reservations, case-insensitive paths). Also fixed on Windows: file reads and writes ran twice. |
| 28 | Wander and Agro flickering through colors on AMD: the session 11 lane rule also on wave64 GPUs. |
| 29 | Menu crash: the PM4 parser read a packet's length after the packet had signalled the guest. |
| 39 | Steam Deck runs (distrobox build); integrated-GPU memory budget fixed (no speed change). |
| 41 | The brightness setting works (`sceVideoOutAdjustColor` gamma); PS4 Pro mode boots (`SOTC_NEO=1`). |
| 52 | Crash in the lifted-restriction fault retry fixed. |

## Audio (2026-10-11)

| Step | Fix |
|---|---|
| 1 | Host audio output through SDL3. |
| 2 | The user's own firmware `libSceNgs2` linked from `SOTC_FW_DIR`; Ajm HLE. |
| 3 | ATRAC9 decoding through LibAtrac9: voices, effects and music play. |

## Speed, one GPU thread (sessions 10-40)

Gameplay started at ~6-7 fps on Windows.

| Session | Fix | Result | Default (off switch) |
|---|---|---|---|
| 10 | GPU drains: the SRT walker reads unwritten bytes without waiting; data from finished batches copied on a side queue; hot-line flush. | Waits ~850/s -> ~15-30/s; 6-7 -> 7-10 fps | on (`SOTC_SIDE_READBACK=0`, `SOTC_HOT_FLUSH=0`) |
| 12 | GPU stats (`SOTC_GPU_STATS`): gameplay is CPU-bound on the GPU thread. | measurement | - |
| 13 | Readback prefetch. | no gain | opt-in (`SOTC_PREFETCH_READBACK=1`) |
| 14 | Async image free. | no gain | opt-in (`SOTC_ASYNC_IMAGE_FREE=1`) |
| 15 | Trimmed fault-download windows. | in-batch waits halved | on |
| 16 | Downscaled textures under 8 GiB of VRAM (the slow state was VRAM paging). | fewer slow runs | on under 8 GiB (`SOTC_TEX_DOWNSCALE=0`) |
| 17 | Write ticks per 4 KiB. | waits halved, fps unchanged | on (`SOTC_FINE_TICKS=0`) |
| 18-20 | Tracy build; ~10 us per draw spread over many small pieces. | measurement | `-DSOTC_TRACY=ON` |
| 21 | On-disk pipeline cache on by default. | faster boots | on (`SOTC_GPU_PIPELINE_CACHE=0`) |
| 21-24 | CPU copy of the game's copy kernel (no GPU round trip for the walker). | ~10.8 fps, kept on after 30 cold boots | on (`SOTC_CPU_COPY=0`) |
| 23 | Upload staging ring sized to what is in flight. | ~1.4-2.3 GB less memory | on (`SOTC_STAGING_RING=0`) |
| 26 | Pipeline preload on worker threads. | cold driver cache: boot preload 18 s on 11 threads | on (`SOTC_PRELOAD_THREADS`) |
| 27 | Async graphics pipelines on a cache miss. | shorter load freezes | on (`SOTC_ASYNC_PIPELINES=0`) |
| 30 | No host protection queries for arena pages (Linux: `/proc/self/maps` at ~8 ms each). | the Linux streaming freezes gone | on |
| 31 | Compute pipelines compiled ahead of their dispatches. | gameplay-load stall 3.0 -> 1.7 s (Windows) | on (`SOTC_CS_PREFETCH=0`) |
| 31 | Timeline-semaphore queries only when needed (Linux). | 17.0 -> 20.3 fps | on |
| 32 | Streamed ranges as an append-only log. | -1.7 ms/flip (Linux) | on |
| 33 | Lock-free memory clamp from a per-thread snapshot. | no lock on ~1M calls/s | on |
| 33 | Hot lines that age out. | -4% fps | opt-in (`SOTC_HOT_AGING=1`) |
| 34 | Page prefilter for `IsRangeGpuWritten`. | -0.4 ms/flip (Linux) | on |
| 36 | `FindImage` lookup cache. | 21.4 -> 22.2 fps (Linux) | on |
| 37 | LTO + PGO build options. | Windows: -8.7% ms/flip, +8.4% fps | opt-in (`-DSOTC_LTO=ON`, `-DSOTC_PGO=`) |
| 38 | Render-state memo. | 82% hits, gain within noise | on (`SOTC_RS_MEMO=0`) |
| 40 | Texture-set memo. | -2.4% GPU-thread CPU (Linux) | on (`SOTC_TEX_MEMO=0`) |

## Speed, the two-stage GPU pipeline (sessions 42-53)

The GPU thread split in two: stage A reads the game's command stream, stage B records Vulkan.
Design and rules: [next/gpu-pipe.md](next/gpu-pipe.md).

| Session | Fix | Result | Default (off switch) |
|---|---|---|---|
| 42 | Design (no code). | - | - |
| 43 | Step 1: register deltas, packet ring, both halves on one thread. | +2.5% cost, 0 differences in verify mode | `SOTC_GPU_PIPE=sync` |
| 44 | Step 2: stage B on its own thread. | 22.5 vs 22.0 fps (Linux) | `SOTC_GPU_PIPE=1` |
| 45 | Step 3a: fences, submit-done, idle and flips run on B. | 24.7 vs 21.7 fps (Linux) | |
| 46 | Step 3b: commands and A's faults run on B. | fewer drains | |
| 47 | Step 4: the CPU copy computed on A, committed on B. | | |
| 48 | Step 5a: written-binding memo. | +5.5% fps (Linux) | on (`SOTC_WRITE_MEMO=0`) |
| 49 | Wait timers on B; fast CPU copy. | fast copy: fewer fps | opt-in (`SOTC_COPY_FAST=1`) |
| 50 | Asynchronous readback. | +5.4% fps (Linux, ~29 fps) | on with the pipe (`SOTC_ASYNC_READBACK=0`) |
| 51 | Wait accounting for stage A and the guest. | measurement | - |
| 52 | Hot-flush budget (no more random flush storms that cost 10-35%). | 29.3 vs 29.0 fps (Linux) | on (`SOTC_HOT_BUDGET=0`) |
| 53 | The pipe on Windows, **on by default**. | ~13 -> ~19 fps (Windows, +47%) | on (`SOTC_GPU_PIPE=0`) |

## Speed, Windows stage B (sessions 54 on)

On Windows stage B is the limit (~42 ms/flip of B's CPU at ~7,200 draws and dispatches per flip).

| Session | Fix | Result | Default (off switch) |
|---|---|---|---|
| 54 | Profile of B on NVIDIA (driver ~22%, waits ~10%, memmove ~10%, buffer tracking ~8%). | measurement | - |
| 55 | Descriptor-set cache. | 99.9% hits, no gain on NVIDIA | opt-in (`SOTC_DESC_CACHE=1`) |
| 56 | Resident-run memo in `EnsureResident`. | 1.6% -> 0.8% of B, no fps change | on (`SOTC_RESIDENT_MEMO=0`) |
| 57 | Nsight Systems capture of B; pipeline-bind memo. | 71% of binds skipped, B -1 ms/flip | on (`SOTC_BIND_MEMO=0`) |
| 58 | Queue submits counted by caller. | ~43/flip, ~4% of B: not the cost | - |
| 59 | Draw-prep design inputs: bbport's workers read; B's time per record measured (`docs/next/draw-prep.md`). | ~58% of record time is cache hits we could move, ~42% Vulkan recording | - |

Next: multi-threaded draw preparation (see [NEXT.md](NEXT.md)).
