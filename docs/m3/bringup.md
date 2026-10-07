# M3 Vulkan bring-up: status and plan

Goal: the M3 exit criteria in `docs/MILESTONE3.md` on the Vulkan GPU. The intro logos without flicker, the
title screen, still running at 120 s, no errors. Then wrap up M3.

## Where we are (2026-10-07)
Commits: `d1199db` "M3 integration (WIP)", then "M3 bring-up: device loss, flip order, flip rate".

| Area | State |
|---|---|
| Null GPU | Meets M3: 60 flips/s during the logos, ~30/s from the title stage on. |
| Device lost / TDR | **Fixed.** See "Device loss" below. |
| Flip ordering | **Fixed.** `SubmitAndFlip` now arms the EOP flip before submitting the DCB. |
| Flip rate | **Fixed.** `sotcgpu_set_flip_rate`, called from `sceVideoOutSetFlipRate`. |
| Logo flicker | **Fixed.** `sotcgpu_map_memory` now calls `RegisterMemory`, so the page manager tracks CPU writes. |
| Intro scene | **Renders** (moon, cliffs, Wander and Agro, lit). It needed GPU readbacks; see "Steps A–C" below. |
| Title throughput | **Improved.** 4 flips/s at first; 12–20 flips/s after the buffer-cache patch. The limit now is shadPS4's per-draw cost: ~20k indirect draws per frame. |
| Title screen | **Not confirmed.** A 125 s run is still in the intro cinematic. |

### Device loss (fixed)
- The game maps one direct-memory pool of more than 3 GiB and sub-allocates from it.
- It binds thousands of "unbounded" vsharps (`num_records = ~0`), many based near the start of that pool.
- shadPS4 clamps those to the end of the mapping, so the buffer cache made 2.6–3.7 GiB of its sparse arena
  resident. On the 6 GB RTX 2060 that over-commits VRAM: 300 ms frames, then a TDR. One run instead failed
  with `ErrorOutOfDeviceMemory` (one 2.9 GiB allocation).
- `hle_memory_clamp_range` (`runtime/src/hle/memory.c`) now bounds a request of 1 GiB or more by its own
  mapping and by `SOTC_UNBOUNDED_CAP_MB` (default 128; 0 turns the cap off). Resident memory fell to ~0.9 GiB.
  A 120 s run had no device loss and no TDR events.
- Cost: reads more than 128 MiB past such a descriptor's base return zero. The real fix is residency
  management (M4, AjncSystem #11).

### Flicker: what is known
- The game double-buffers: flips 0,1,0,1 at 60 Hz, format `0x88000000` (A2R10G10B10 sRGB), tile mode 10.
- Burst screenshots, which log the buffer index, show content on one buffer and black on the other. During
  the Japan Studio logo, buffer 1 always had the logo and buffer 0 was always black. Which buffer is black
  varies between runs. Some runs captured no content at all.
- The texture cache is **not** the problem. The game's render target and the presenter's lookup resolve to the
  same image for each buffer (ids 0 and 1, same format and tiling, flags GpuModified|Registered).
- So the game's draw into that buffer produces black. Suspects, most likely first:
  1. **Per-frame CPU data not seen by the GPU.** A 2-slot ring of constants (fade value, transforms) where the
     CPU's writes to one slot aren't detected by the buffer cache's page tracking. No protect failures are
     logged. Check `gpu_bridge_on_fault` → `sotcgpu_handle_fault` hit counts per frame. Also check aliasing:
     direct memory has two host views (the arena view and the physical backing), and a write through the
     backing view isn't tracked.
  2. **`IT_SET_PREDICATION` ignored.** About 2 packets per frame during the logos (`liverpool.cpp:422`). Draws
     that should be skipped run anyway, which could explain the shapes in the top-right quarter. A real
     implementation needs occlusion-query results (ZPASS_DONE). First test cheaply: skip predicated draws
     entirely and see whether the flicker or the shapes go away.
  3. `IT_GET_LOD_STATS` unimplemented (~750 per minute).

## Steps A–C (2026-10-07, second session)

### A: the intro showed only fog
- **A1, unbounded vsharps clamped into small mappings: not the cause.**
  - The 5–12 KiB "Clamped size" results come from 7 regions. The game maps 1 MiB, then unmaps the last
    0xF0000, so each region is a real 64 KiB mapping.
  - The next mapping always starts 1 MiB later. There's no adjacent mapping, so spanning across mappings
    (the planned `SOTC_UNBOUNDED_SPAN`) changes nothing; it was dropped.
  - Raising `SOTC_UNBOUNDED_CAP_MB` to 1024 didn't change the picture either.
- **A2, SRT walker `Phi` failures: real but secondary.**
  - 7 shaders (4 cs, 1 vs, 2 fs) index their resource tables with loop counters (`i = phi(0, i+1)`).
    shadPS4's CPU walker can't flatten those, so the reads return flat-buffer slot 0.
  - shadPS4's fix is DMA (`direct_memory_access`). Enabling it turned every frame black: the walker faulted
    on null tables (offsets 0x20–0x870), and its fault handler patches those loads to zero permanently.
  - Skipping the two big shaders (0xbaec2d3e, 0xe4603b32) changed nothing visible in the intro.
  - Open for M4.
- **Root cause: GPU-written constants read on the CPU.**
  - Scene geometry is ~20k `DrawIndexIndirect` per frame. The arguments are CPU-written and valid.
    `startInstance` packs a per-draw instance index in its low 16 bits.
  - A render-target dump showed every depth target and every G-buffer target untouched (cleared to zero).
    That includes the main depth (2,443 draws) and the shadow maps (3,925 and 15,648 draws).
  - The geometry vs (0x9f1dbd1b) reads its view-projection matrix from a per-view block (`T[2..3]` → dwords
    32–47). That block is GPU-modified: the game computes its camera constants on the GPU.
  - With readbacks disabled (shadPS4's default), the walker read stale guest memory, so every draw got a
    garbage matrix.
  - **Fix:** sotcgpu now defaults to `readbacks_mode = Precise`. `SOTC_GPU_READBACKS=0|1|2` overrides it.
    Relaxed shows geometry but unlit silhouettes; Precise shows the fully lit scene, and is slightly faster.

### B: throughput
- **Faults aren't the bottleneck.**
  - Write faults peak at ~69k/s during the logos but cost only ~120 ms/s in the handler.
  - At the title they're 5–10k/s and ~10–20 ms/s.
- **Stream-buffer stalls were.** A sampling profile of `GpuCommandProcessor` and counters found:
  - 1.3 GiB/s of guest→stream-buffer copies: 340k copies/s of ~4 KiB.
  - ~10 blocking `Scheduler::Wait` per second (`StreamBuffer::WaitPendingOperations`).
  - The cause is that `ObtainBuffer` streams every small read-only bind.
  - **Fix:** a listed patch (`third_party/shadps4/README.md`) streams only CPU-modified data and binds the
    rest in place. Copies fell to ~50 MiB/s, with no blocking waits. The title went from 4 to 12–20 flips/s.
- **What's left is per-draw CPU cost.**
  - ~20k indirect draws and ~2k dispatches per frame. Each pays full setup: descriptor push, barriers,
    texture-cache lookups, `hle_memory_clamp_range`.
  - M4 options: batch consecutive compatible indirect draws (multi-draw), cache bindings across draws, and
    make the clamp lookup lock-free.
- Job workers spend ~80% of a core each, mostly contending a guest mutex (critical-section spin).
  Secondary.

### C: frame rate in the window title
- Done by a worktree agent; see `docs/m3/c.md`. The title reads `Shadow of the Colossus — N fps (Vulkan)`,
  counted where flips complete (`log_flip` in `videoout.c`).

### Still open
- **Title screen:** confirm it renders. Earlier runs showed only the arrow and ✕/○ icons.
- **`IT_SET_PREDICATION`, `IT_GET_LOD_STATS`:** unimplemented.
- **Phi-indexed SRT reads:** see A2.
- **`test_sotcgpu_headless` causes a real TDR** (event 4101) on every run, although it passes. This
  predates this session: it happens with or without the readbacks default, the buffer-cache patch and
  `RegisterMemory`. Suspect teardown or the headless device path.
- **Debug technique that worked: render-target dump** (temporary code, removed).
  - Record the image ids that `BeginRendering` binds during one frame.
  - Then `runtime.DownloadImage` each into a HostCached buffer, `scheduler.Finish()`, convert per format and
    write a PNG.
  - RenderDoc wasn't usable: the capture layer slows the game to ~3 flips per minute (sparse arena).

## Plan (earlier; Steps 1–2 superseded by the section above)

### Step 1: flicker
1. Log the `SET_PREDICATION` packet fields: address, `pred_op` (bits 18:16), `predicate` (bit 8), `continue`
   (bit 31). Note which draws fall inside a predicated range. Temporary log in `liverpool.cpp`; remove it
   afterwards.
2. Experiment: `SOTC_PRED=skip|run`. Skip draws while predication is active. Burst screenshots
   (`smoke.py --shots 0.3`) during the logos, then ask the user to watch the window.
3. If predication isn't the cause, look at CPU→GPU coherency:
   - Find the logo draw (`SOTC_GPU_SKIP` on the fs/vs hashes compiled at 5–10 s until the logo disappears).
   - Dump its constant-buffer contents on both buffers' frames, or force a re-upload of its buffers every draw.
     If the flicker goes away, the page tracking misses writes.
4. Fix the root cause. Keep any shadPS4 change as a listed patch in `third_party/shadps4/README.md`.

### Step 2: title scene throughput and black title
1. `smoke.py --threads --timing` on Vulkan for 90 s. Is the GPU thread (`GpuCommandProcessor`) saturated, or
   are guest threads waiting?
2. Look for per-draw stalls: readbacks, `SynchronizeMemory`, fault storms (count faults per second), arena
   `EnsureResident` calls.
3. Check the title render: burst shots, plus a `SOTC_PAD_SCRIPT="45:cross,55:cross"` run in case it waits for
   input.
4. Re-check the SRT walker `Phi` failures (~270 per run) for title-scene shaders: unflattened resource tables
   read garbage descriptors.

### Step 3: M3 wrap-up (only once the exit criteria pass or the gaps are listed for M4)
- Fold `docs/m3/{gl,gw,gi,p,s}.md` and this file into `docs/RECON.md` as "Milestone 3 result".
- `docs/MILESTONE3.md`: the contract now has `rip` in `sotcgpu_handle_fault`/`gpu_bridge_on_fault`, plus
  `sotcgpu_set_flip_rate`.
- `CLAUDE.md`:
  - Commands: `build-vk` (`-DSOTC_VULKAN=ON`), and the smoke options `--exe`, `--gpu`, `--shots`.
  - Environment variables: `SOTC_GPU`, `SOTC_SHOTS`, `SOTC_WINDOW`, `SOTC_HEADLESS`, `SOTC_PAD_SCRIPT`,
    `SOTC_VK_VALIDATION`, `SOTC_GPU_LOG`, `SOTC_TIMING`, `SOTC_GPU_SKIP`, `SOTC_UNBOUNDED_CAP_MB`,
    `SOTC_GPU_READBACKS`.
  - The map: `gpu/`, `window.c`, `gpu_bridge.c`, the platform split, the trampolines.
- Anything still open goes on M4's list with its AjncSystem number (`docs/RESEARCH.md` §2.4).

## How to run
```sh
export SOTC_GAME_DIR='D:\CUSA08809\uroot'
python scripts/smoke.py --exe build-vk/sotc-runtime.exe --gpu vulkan --seconds 120 --shots 10
python scripts/smoke.py --exe build-vk/sotc-runtime.exe --gpu vulkan --seconds 30 --shots 0.3   # burst
python scripts/smoke.py --gpu null --seconds 60                                                  # regression
```
- Screenshots go to `build/game/shots/`. A black 1080p frame is exactly 82420 bytes, so `stat -c %s` spots
  content without opening the images.
- TDRs: `Get-WinEvent -FilterHashtable @{LogName='System';ProviderName='Display','nvlddmkm';StartTime=(Get-Date).AddMinutes(-5)}`.
- Only the user can see the window. Ask them to watch it when screenshots disagree.
- Run one GPU run at a time.
