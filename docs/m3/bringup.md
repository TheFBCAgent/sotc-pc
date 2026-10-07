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
| Logo flicker | **Open.** One display buffer comes out black on alternate frames. Shapes flash in the top-right quarter. |
| Title screen | **Open.** After ~40 s the title scene loads at 3–6 flips/s and the screen stays black. |

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

## Plan

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
    `SOTC_VK_VALIDATION`, `SOTC_GPU_LOG`, `SOTC_TIMING`, `SOTC_GPU_SKIP`, `SOTC_UNBOUNDED_CAP_MB`.
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
