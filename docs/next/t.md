# T: texture-cache GC, so VRAM stays bounded

## The problem
- At 580 s, `[vram]` showed heap0 at 6,576 MiB against a 5,103 MiB budget.
  - Images: 1,392 live, 3,782 MiB. The buffer arena: 1.5 GiB.
  - The game then hits `ErrorOutOfDeviceMemory` or renders wrong.
- Images are what grows. shadPS4's `TextureCache::GarbageCollectImages` had four problems, and together
  they mean that on this card it effectively **never frees anything** once a render target reaches the old
  end of the LRU.
  1. **Tiled GPU-written images were never freed.** `if (tiled && download) return false;` ("can't handle
     non-linear image downloads"). `SafeToDownload()` is `GpuModified && !Dirty`, and every render and
     depth target is `GpuModified` (`FindRenderTarget` / `FindDepthTarget` set it on every bind). SotC's
     targets are tiled, so none of them is ever reclaimed.
  2. **Kept images used up the deletion budget.** `clean_up` decremented `num_deletions` (10/20/40) before it
     decided to skip. The LRU is walked oldest first, so once the 10 oldest images were unfreeable targets
     (for example the boot and logo targets), each pass spent its whole budget on them, and nothing behind
     them was ever looked at: no old texture was freed either.
  3. **The thresholds don't fit a card under 8 GB.** `mem_threshold = min(budget, 8 GB)` = budget, so
     `trigger_gc_memory = (budget - mem_threshold) / 2 = 0`, and the GC ran every tick. The "aggressive"
     pass waited for *older* images (160 ticks, against 80 under pressure and 16 normally).
  4. **`Common::LRUCache::ForEachItemBelow` ignored `return true`.** `RETURNS_BOOL` compared the
     `std::invoke_result<...>` struct (not `_t`) with `bool`, so it was always false. The walk never stopped
     early: harmless, but it hid problem 2.
- When the GC runs: `Rasterizer::OnSubmit`, from Liverpool's `Process()` after `SubmitDone()`. That's
  `sotcgpu_submit_done`, which is `sceGnmSubmitDone`, about once per frame. So a GC *tick* is about one
  frame (8–20 per second at the title). `gc_tick` and the LRU are fed. `FindImage` and `GetImage` call
  `TouchImage`, and `RegisterImage` inserts. `image.tick` is the `gc_tick` of the last use.

## Readback path and CPU visibility (why freeing is safe for CPU-read data)
- `SOTC_GPU_READBACKS` (default Precise) is a **buffer-cache** mechanism.
  - GPU-written buffer ranges are tracked by the memory tracker.
  - A CPU access to such a page faults, and the buffer cache downloads the range from the arena.
  - The camera constants (`docs/m3/bringup.md` A) are written through buffers and read back this way.
  - The texture cache isn't involved.
- Image contents reach guest memory only through `TextureCache::DownloadImageMemory`, which has two
  callers:
  - `ProcessDownloadImages`: only images queued with `readback_linear_images`, which sotcgpu leaves
    **off** (`gpu/shim/core/emulator_settings.h`).
  - The GC itself: linear GPU-written images under pressure, unchanged.
- So the CPU never sees a tiled image's GPU-written contents, before or after this change. Freeing such
  an image without a download leaves the guest bytes, and every CPU read of them, exactly as they were.
- What is lost is the **GPU-side** copy of the contents. After a free:
  - A re-created image is `Dirty` and `RefreshImage` uploads it from `ObtainBufferForImage`: the arena,
    which mirrors guest memory plus any GPU *buffer* writes. The freed range is well defined. It reads as
    the guest bytes, which is what shadPS4 shows for any freed image.
  - A texel-buffer read of that address (`SynchronizeMemoryFromImage`) no longer finds the image, and
    reads the arena bytes.
  - This shows up only if the game samples a target long after its last use, without rendering it
    again first. That's why these images need a long age.
- An image a GPU buffer write has hit since (`GpuDirty`), or one the CPU wrote (`CpuDirty`), isn't
  `SafeToDownload`. Its newest data is in guest memory or the arena, so freeing it was always exact and
  stays on the normal path.

## Policy (option a + c)
`third_party/shadps4/src/video_core/texture_cache/sotc_gc_policy.h` is Vulkan-free and header-only.
`GarbageCollectImages` uses it, and it is a listed patch.

| Level | Usage (fraction of `GetTotalMemoryBudget()`) | Re-uploadable age | Tiled GPU-written age | Frees/tick | Linear GPU-written |
|---|---|---|---|---|---|
| idle | < 50 % | n/a | n/a | 0 | n/a |
| normal | ≥ 50 % | 120 | 1800 | 8 | kept |
| pressured | ≥ 70 % | 30 | 300 | 32 | download, then free |
| critical | ≥ 85 % | 8 | 60 | 128 | download, then free |

- Ages are GC ticks (about frames). At 8–20 fps, 1800 ticks is 1.5–4 min, 300 is 15–40 s and 60 is 3–8 s.
- Usage is `Instance::GetDeviceMemoryUsage()` (VK_EXT_memory_budget). Without the extension, it is
  `total_used_memory` (the images' guest sizes), and the budget is the heap size.
- One pass per tick: walk the LRU from the oldest entry to `gc_tick - min_age`, and decide each image
  (`Decide`). Only frees count against the per-tick cap. Visits are capped at 4096.
- **Why (a) + (c), not (b):**
  - `TileManager::TileImage` exists. It is the tiler `SynchronizeMemoryFromImage` uses, so a tiled
    download into the arena is possible.
  - But it would allocate a device-local staging copy of every image it frees, at the moment memory is
    short. It needs arena residency for the range, adds a compute dispatch per free, and doesn't handle
    MSAA or depth-stencil cleanly.
  - (a) + (c) bounds memory with no new GPU work and no CPU-visible change. If stale targets show up on the
    GPU run, (b) is the follow-up, limited to the `FreeStale` case.

## Diagnostics
- The `[vram]` line gains `image gc <level>: freed N (stale S, downloaded D), skipped K`. These are
  totals since start:
  - `stale`: tiled GPU-written images freed without a download.
  - `skipped`: visits where an old image was kept, which means a GPU-written one not yet old enough.
- It's followed by `[vram] images: ...`, which the GPU thread rebuilds about every 10 s by walking the LRU
  (`TextureCache::SotcPublishImageBreakdown`):
  - live count and VMA MiB;
  - by time since last use: < 60, 60–600, > 600 ticks;
  - targets (RT or DS), texture-only, tiled GPU-written;
  - images that share a base address with another live image (aliases: other formats, tile modes or
    sizes of the same memory), and at how many addresses;
  - total image views;
  - the 4 largest images (address, size, layers, mips, format, tile mode, usage, MiB, age).
- **Why 1,392 images were live:** this can't be measured without a GPU run, so the breakdown is there to
  answer it. From the code, the expected answer is "mostly long-unused". Problem 2 stops the GC at the
  first unfreeable targets, so every texture and target ever created stays until shutdown. Check the
  `> 600 ticks` bucket in a run with the old GC (main) against one with this change. Aliases (problem 1's
  targets re-created per format or tile mode at one address) show up as `shared address`.

## Tests
- `gpu/tests/test_gc_policy.cpp` (`test_gc_policy`, 143 checks, in `build-vk`) covers:
  - thresholds, levels and per-level parameters (monotonic, and GPU-written ages always longer);
  - `Decide` for each image kind;
  - a simulated LRU: 100 young targets at the head no longer stall the frees behind them; textures all go
    over a few ticks; targets go once they reach their age;
  - the visit cap, that young images are never visited, and no underflow early in the run;
  - the `lru_cache.h` `return true` fix.
- `build`: 18/18. `build-vk -E sotcgpu`: 21/21.

## What the main session must check on a GPU run
1. **Memory stays bounded.** Run past the old failure point, for example
   `smoke.py --exe build-vk/sotc-runtime.exe --gpu vulkan --seconds 700 --shots 30`. Then check:
   - in `[vram]`, heap0 usage stays under its budget;
   - `images N live` plateaus instead of climbing;
   - `freed` grows;
   - the level is mostly normal or pressured, rarely critical.
2. **Read the `[vram] images:` lines** early (60 s) and late (500 s+). Note the age buckets, the
   `shared address` count and the top sizes. They answer the "why 1,392" question for real.
3. **No stale targets.** Compare screenshots with a main run at the same times. Look for a surface that
   shows an older frame, black or garbage for a moment, then recovers. Likely places: the title after the
   intro, menus opened and closed after a long time, and returning to an area.
   - `stale` > 0 with no visible problem is the expected result.
   - If something is stale, raise `min_age_gpu_tiled` in `Configure`, or implement (b) for `FreeStale`.
4. **No new crashes.** A freed image still in use would show up as an assert or a bad `ImageId` in
   `vk_rasterizer.cpp` or `texture_cache.cpp`. Before this change, shadPS4 already freed old images
   through the same `FreeImage` path.
5. **Frame rate unchanged.** The title's fps should match main. `SOTC_GPU_STATS=1` guest→GPU MiB shouldn't
   rise much. A rise would mean textures are being re-uploaded too often, and `min_age` is too short at
   the level the run sits in.
