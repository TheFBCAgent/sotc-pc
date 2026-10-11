# Environment variables

Moved from `CLAUDE.md` ("Commands"). Set them for `sotc-runtime`, or pass them with `smoke.py --env` / `run.bat --env`.

Converter environment variables (`scripts/convert.sh`, `scripts\convert.ps1`):
- `SOTC_GAME_DIR` (required): the game folder with `eboot.bin`.
- `SOTC_SHADPS4` (default `../shadps4`): the shadPS4 checkout that names the imports.
- `SOTC_FW_DIR` (optional): a folder with the user's own decrypted firmware `libSceNgs2.sprx`
  (e.g. `D:\`). The scripts pass it as `sotcconv.py --fw-dir`, which links Sony's Ngs2 mixer into
  the boot image (its Ajm imports go to `runtime/src/hle/ajm.c`; `docs/next/audio.md`). Unset or
  without the file: nothing extra is linked, the converter prints one `firmware:` line saying so,
  and the runtime's null Ngs2 runs (the game is silent). The module is read in place, never
  copied; the boot image that contains it is game/firmware-derived (never commit it).

Runtime environment variables:
- `SOTC_GAME_DIR` (required): mounted as `/app0`.
- `SOTC_USER_DIR` (default `user`; smoke uses `build/user`): writable mounts.
- `SOTC_LOG=0..4`
- `SOTC_STRICT=1`: abort at the first unimplemented import.
- `SOTC_NEO=1`: report PS4 Pro hardware.
- `SOTC_USER_NAME` (default "Wander"), `SOTC_LANGUAGE` (default 1, en-US), `SOTC_ENTER_BUTTON`: UserService and SystemService values.
- Window and input:
  - `SOTC_HEADLESS=1`: no window.
  - `SOTC_WINDOW=WxH`: window size.
  - `SOTC_PAD_SCRIPT="25:cross,31.5:down"`: timed button presses for unattended runs.
- Audio (`runtime/src/audio_host.c`, `docs/next/audio.md`; needs the window, so headless is silent):
  - `SOTC_AUDIO=0`: no host audio; AudioOut paces on the clock as before.
  - `SOTC_AUDIO_VOLUME=0..200` (percent, default 100).
  - `SOTC_AUDIO_LATENCY_MS=10..500` (default 60; at least two device buffers): audio queued per
    port before `sceAudioOutOutput` blocks.
- GPU (`build-vk`):
  - `SOTC_GPU=null|vulkan`.
  - `SOTC_SHOTS=N`: screenshot interval in seconds.
  - `SOTC_VK_VALIDATION=1`.
  - `SOTC_GPU_LOG`: shadPS4 log filter.
  - `SOTC_GPU_SKIP=<hash,...>`: drop one shader's draws.
  - `SOTC_GPU_READBACKS=0|1|2`: off, relaxed, precise. Default precise: the game computes its camera constants on the GPU.
  - `SOTC_GPU_PIPELINE_CACHE=0` (on by default since session 21): shadPS4's on-disk pipeline cache in
    `$SOTC_USER_DIR/gpu_cache/pipelines-v<kPipelineCacheVersion>/CUSA08809/` (game-derived, never
    commit). Pipelines are saved as they compile; the next start preloads them (`Preloaded N
    pipelines in X ms (stages, pipelines on N threads)`: ~1.1 s for 2325). Session 26: the
    stages load serially (~0.9 s), the pipeline constructors (`vkCreate*Pipelines`) run on
    `SOTC_PRELOAD_THREADS` workers (default hardware threads - 1; `=1` serial). With the NVIDIA
    driver's own shader cache cold (session 25 hit it once, ~48 s serial, cause unknown; force it
    with `--env "__GL_SHADER_DISK_CACHE_PATH=<empty dir>"`): 18 s on 11 threads. The first run of a new cache version still compiles on the fly
    (~1000 compiles in the intro). Session 27: the gameplay load (~100 s with run.bat's pad
    script) froze 4.7-4.9 s with the driver cache warm (2.4 s with our cache; compute shader +
    pipeline ~1.3 s of it, graphics ctors 0.46 s), and **37 s** with the driver cache cold too
    (~80 ms per pipeline ctor, about half compute); `SOTC_ASYNC_PIPELINES` below. Bump
    `kPipelineCacheVersion` (`gpu/src/sotcgpu.cpp`) when a patch changes the generated SPIR-V.
  - `SOTC_ASYNC_PIPELINES=N` (session 27; default hardware threads - 2 since session 31, was 4;
    `=0` off): a graphics pipeline
    missing from the cache is built on a worker and its draws are skipped until it is ready
    (`PipelineCache::SotcQueueAsync`/`SotcCollectAsync`, `vk_pipeline_cache.cpp`). Not for
    pipelines where a stage writes memory (built on the GPU thread: skipping them made the cold
    boots' SRT race hit 3 of 4). Gameplay-load freeze, our cache off: 4.7 -> 4.1 s
    (driver cache warm), 37 -> 25 s (cold; what is left is compute pipelines). Stats line
    `async pipelines:` (queued, ready, pending, draws skipped, built on the GPU thread; compute
    jobs count as queued/ready too).
  - `SOTC_CS_PREFETCH=0` (session 31; on while the workers above exist): compute pipelines
    ahead of their dispatches. On a compute miss the GPU thread scans the rest of the command
    buffer (`amdgpu/sotc_dispatch_scan.h`, test `gpu/tests/test_dispatch_scan.cpp`), compiles
    the new programs with their future dispatch's registers (`SotcPrefetchCompute`; an ASSERT
    in that compile throws and drops the program, `common/sotc_assert_guard.h`) and queues the
    pipeline constructors on the workers; a dispatch never skips, it builds its own job or
    waits (`SotcWaitCompute`). The cold gameplay load is one compute-queue submit of ~550
    dispatches, ~270 new programs. Both caches cold: largest freeze 24.4 -> 6.8 s (10 workers).
    Logs `Compiling compute pipeline X ahead` and waits >= 50 ms.
  - `SOTC_UNBOUNDED_CAP_MB`: default 128.
  - `SOTC_IGPU_RESERVE_MB` (session 39, integrated GPUs only): memory left to the system when the
    device budget is computed. Default a third of the heap budgets, at most 8 GB (shadPS4: a flat 8 GB,
    which put the Steam Deck at the 2 GB floor and the image GC in critical mode all run). The log says
    `Integrated GPU: N MiB available, budget M MiB`. `=8192` gives back shadPS4's rule.
  - `SOTC_TIMING=1`: wait accounting.
  - `SOTC_GC_FREE_STALE=1`: let the image GC free tiled GPU-written images without a download
    (off by default: it blackened textures the game renders once, session 7).
  - Readback drains (session 10; see `docs/DEBUGGING.md`, "GPU drains"), each on by default:
    - `SOTC_SIDE_READBACK=0`: download data from finished batches with `Scheduler::Finish` again.
    - `SOTC_HOT_FLUSH=0`: no early submit after dispatches that write hot readback lines.
      The hot list (256 lines, never forgets one) is full of load lines in gameplay; session 33
      tried aging it (LRU by last reader hit, idle lines dropped after 240 flips; per 256 B and
      per 4 KiB): flushes 50 -> 170 (350)/s and side-copy fences up, in-batch drains 29 -> 20
      (12)/s, gtwait unchanged or worse, **fps -4%** (10.27 vs 10.68 baseline). A flush only turns
      a `Finish` into a side-copy wait on the same batch. Kept opt-in: `SOTC_HOT_AGING=1`
      (`buffer_cache/sotc_hot_lines.h`, test `gpu/tests/test_hot_lines.cpp`).
    - `SOTC_SRT_GPU_PATCH=1` (opt-in): patch GPU-written walker constants on the GPU (no gain measured;
      session 19: ~1 ms/frame, because most GPU-written walker tables also hold descriptors).
    - `SOTC_PREFETCH_READBACK=1` (opt-in, session 13): at the end of each batch, copy the GPU-written
      pieces of hot readback pages into a host ring and serve later downloads from it. Side-copy fences
      fell to ~0, but the GPU-thread wait moved into `Scheduler::Wait` and GPU time per frame rose (no gain).
  - `SOTC_TRIM_WINDOW=0` (session 15, on by default): don't trim fault downloads. With it on, a fault
    download whose own data is older than the batch being recorded is shrunk from its 512 KiB window to
    the adjacent 64 KiB pages that are older too (`SotcWriteTicks::TrimWindow`), so in-batch neighbours
    no longer force a `Scheduler::Finish`. Halved the `Finish` waits per frame (~4 to ~2).
  - `SOTC_FINE_TICKS=0` (session 17, on by default): write ticks per 64 KiB again. With it on, bindings
    up to 1 MiB are tracked per 4 KiB (`SotcWriteTicks::SetFinePages`) and the trim works per 4 KiB.
    `Finish` per frame ~4 to ~2 (the drains were neighbours in the same 64 KiB), but side-copy fence
    waits grew by about as much: gtwait and fps unchanged.
  - `SOTC_TEX_DOWNSCALE=0|1` (session 16; **on by default when the VRAM budget is under 8 GiB**, so on
    the RTX 2060, off on a 16 GB card; the log says `SOTC_TEX_DOWNSCALE on/off (budget N MiB)`):
    sampled-only textures (no storage/target/video-out use, not depth or 3D, >1 mip, top level >= 512 px,
    no texel fetch or size/LOD query in the shader) are created one mip level smaller
    (`texture_cache/sotc_tex_downscale.h`; host level k = guest mip k+1). Any other use frees the small
    image and re-creates it full size. Saves 1-1.8 GB in gameplay (heap0 peaks ~4.0 GB outdoors vs a
    ~5.1 GB budget); the `[vram] images:` line ends with `downscale on/off: eligible ...`.
  - `SOTC_ASYNC_IMAGE_FREE=1` (opt-in, session 14): free image memory (`vmaDestroyImage`) on a worker
    thread instead of the GPU thread (`texture_cache/sotc_async_free.h`). `vkFreeMemory` left the GPU
    thread, but no fps gain was measurable, and one run with it on hit a blue screen (cause unknown).
  - `SOTC_RS_MEMO=0` (session 38; default on): turns off the render-state memo (a draw that continues
    the open render pass with the same targets reuses the previous `RenderState`). Logs `render-state
    memo: H hits, M misses` every ~1M draws. Altar: 81.7% hits, -0.9% GPU-thread CPU (noise level).
  - `SOTC_TEX_MEMO=0` (session 40; default on): turns off the texture-set memo (a stage bound again
    with the same T# words, no image registered or unregistered since, reuses its `FindImage` results).
    Logs `texture-set memo: H hits, M misses` every ~1M binds. Temple: 86% hits, -2.4% GPU-thread CPU.
  - `SOTC_CPU_COPY` (default **on** since session 24; `=0` off, `=2` check): SotC's copy kernel, cs
    `0x50784801` (`dst[i] = src[i mod ctl[1]]`), also runs on the CPU at record time
    (`Rasterizer::SotcCpuCopy`, `renderer_vulkan/sotc_cpu_copy.h`) when its source and control words
    aren't GPU-written, so the SRT walker reads its descriptor tables without waiting. Session 22:
    the CPU writes only pages a download already wrote (`buffer_cache/sotc_resident_pages.h`); the
    other pages of the destination stay GPU-written (a copy "in part"), and older GPU data under a
    CPU-written piece is dropped (`SotcForgetGpuWrites`). No size cap, so the 4 MiB buffer at
    `0x1042300000` is covered. `=2` (check) keeps the marking, copies only fully resident
    destinations and compares every download with the CPU result (session 21: 1.5 G dwords, 0 differ).
    **fps +12%** (session 24: 10.83 vs 9.65 fps at 110-240 s, 3 alternating `run.bat` runs each;
    session 23: +14%). Memory within ~0.15 GB of copy off (session 23). Cold-cache boots
    (sessions 24-25): black after `cs 0xa402ddac` compiles in 2 of 35 with the copy on and 2 of 15
    off, so that race is pre-existing. One Oodle LZ crash in a copy-on cold boot (session 24, log
    lost), not seen in 30 more boots (session 25).
  - `SOTC_STAGING_RING=0` (session 23, on by default): shadPS4's upload staging ring as it was.
    With it on (`renderer_vulkan/sotc_staging_ring.h`, test `gpu/tests/test_staging_ring.cpp`):
    `StagingBufferPool` ticks per flip (`Rasterizer::SotcGcTick`; its tick ran only from
    `OnSubmit`, which stops in gameplay, so nothing was ever freed), tries the current 16 MiB
    block and then the lowest index up, frees blocks unused for 60 frames (large buffers: 120),
    and past 32 blocks waits for the oldest block instead of growing. Before, one loading batch
    (~1 GB of uploads, 1.9 GB with `SOTC_CPU_COPY`) grew the ring to 89-142 blocks (1.4-2.3 GB of
    host memory) for the rest of the run; gameplay needs 1 block (a batch uploads 5-9 MiB). Peak
    private 6.8-7.1 -> 5.2-5.3 GB (copy on: 7.8-8.1 -> 5.4-5.7 GB), fps unchanged; ~4-7 capped
    waits per run, all in loads. The `[vram] staging:` line (every ~10 s) gives per memory type
    blocks, new/trimmed, requests, the largest batch, capped waits, why the ring grew (blocks
    waiting for the batch being recorded vs. a submitted one) and large buffers.
  - `SOTC_CPU_COPY_LOG=1` (debug, session 22): logs every copy-kernel dispatch (`cpu-copy-log:`
    outcome, tick, sharps, control words; big copies also the working set before) and, when the
    SRT walker is about to read a table at an address that isn't GPU memory, the walker's table
    trail (`srt-trail:`, shader, earlier tables and their first dwords) for 8 s from the first
    copy; `=N` starts the 8 s at N s. ~20k lines in 8 s.
