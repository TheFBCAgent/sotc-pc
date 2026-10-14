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
- `SOTC_GUEST_ARGS="+name=value ..."` (session 69): extra arguments for the eboot's `main`, split
  at spaces. SotC appends them to its command line (`[MSG-Init] CmdLine:`) and reads `+name=value`
  console variables from it, e.g. `+progSamplerAnisoLevel_Cinematic=16` (AjncSystem's "sharp
  textures" mode sets the five `*AnisoLevel*` variables this way; no visible change on Linux).
- `SOTC_CALLER_TRACE=1` (session 69, debug): once a second, logs `caller: <fn> image+0x<offset>
  N/s (tid T)` for the guest call sites of sleeps, event-queue and event-flag waits, vblank and
  flip waits and frame submits (`runtime/src/hle/caller_trace.c`), plus `caller: flip mode` changes.
  Image offsets are eboot addresses (base 0 in `boot.txt`); GoldHEN/AjncSystem patch addresses are
  these + 0x400000.
- `SOTC_NEO=1`: report PS4 Pro hardware (`sceKernelIsNeoMode`, the Neo GnmDriver init sequences,
  alt-tile images). Session 41: the title and intro render (the display buffer is an alt-tile image
  in Neo mode, `third_party/shadps4/README.md`); gameplay and the Graphics Mode option untested.
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
  - `SOTC_ABSOLUTE_INSTANCE_ID=1` (session 69): restores upstream's absolute instance ID in vertex
    shaders without a base-instance user SGPR (the fix for flashing particles turned off).
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
      Session 51 (pipe on, Pro mode): the list fills during the gameplay load (~44 s), mostly with
      different lines each run (`hot line N at ...` log lines, one per insert). In 3 of 8 runs
      gameplay compute kept writing some of them: ~3,100 or ~25,000 hot flushes/s instead of
      ~170, 26.3 / 19.3 flips/s instead of ~29.2 (`hot flushes N` in the `pipe: B waits:` line).
      Session 52 (pipe on, Pro mode, alternating, 3 runs each, no storm in any): default 28.78,
      `=0` 28.24, `SOTC_HOT_AGING=1` 28.22 flips/s (aging flushes ~400/s vs ~200-340).
    - `SOTC_HOT_BUDGET=N` (session 52, default 8; `0` = no limit, the old behaviour): each hot
      line may request at most N flushes per flip, all lines together 4N. Normal runs flush ~6-12
      times per flip in all. Denied requests: `hot flushes N (denied M)` on the `pipe: B waits:`
      line. Alternating, 4 pairs: 29.28 vs 28.97 flips/s (`=0`); a fifth `=0` run stormed
      (~2,090 flushes/s, 26.26 flips/s). Storms with `=0` (or before session 52): 1 of 8 runs this
      session, 3 of 8 in session 51; with the budget: 0 of 5 (one run denied ~70/s at
      29.35 flips/s: a line written more than 8 times per flip all run long).
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
  - `SOTC_DESC_CACHE=1` (session 55; default off): cached descriptor sets instead of descriptor
    pushes. Pipelines with at most `maxDescriptorSetStorageBuffersDynamic` storage buffers get
    STORAGE_BUFFER_DYNAMIC buffers; a set is keyed by the layout and the writes without buffer
    offsets, and bound with the offsets as dynamic offsets (`sotc_desc_cache.h`). Logs `desc-cache:`
    every 10 s (binds, misses, sets evicted by destroyed views/samplers/buffers/layouts). Temple,
    RTX 2060: 99.9% hits, but stage B's CPU per flip unchanged (43.9 vs 43.3 ms on/off, 4+4 runs).
    Untried on RADV, where the pushes were ~21% of B.
  - `SOTC_TEX_MEMO=0` (session 40; default on): turns off the texture-set memo (a stage bound again
    with the same T# words, no image registered or unregistered since, reuses its `FindImage` results).
    Logs `texture-set memo: H hits, M misses` every ~1M binds. Temple: 86% hits, -2.4% GPU-thread CPU.
  - `SOTC_WRITE_MEMO=0` (session 48; default on): turns off the written-binding memo (a written
    buffer binding that repeats a range already noted at the same scheduler tick, with no download,
    CPU-copy forget or hot-line change since, skips `SotcNoteGpuWrite`'s bookkeeping: the
    `gpu_modified_ranges` add, page marks, write ticks and hot-line scan). Logs `written-binding
    memo: H hits, M misses` every ~1M written bindings. Temple: 74% hits; with `SOTC_GPU_PIPE=1`
    27.65/27.52 vs 26.24/26.07 flips/s off (+5.5%), stage B 25.8 vs 27.8 ms/flip.
  - `SOTC_RESIDENT_MEMO=0` (session 56; default on): turns off `EnsureResident`'s memo of block
    runs known to be resident (256 entries keyed by the request's 16-block granule; arena memory is
    never unbound, so an entry never goes stale). Logs `resident-run memo: H hits, M misses` every
    ~1M binds. Temple (Windows, Pro): ~92% hits; `EnsureResident` fell from 1.6% to 0.8% of stage
    B's samples, too small to show in flips/s or B ms/flip (on 42.4-42.8 vs off 41.4-43.5 in the
    steady runs).
  - `SOTC_BIND_MEMO=0` (session 57; default on): turns off `Scheduler::BindPipeline`'s memo, which
    skips `vkCmdBindPipeline` when the same pipeline is already bound at that bind point on the
    current command buffer. Logs `pipeline-bind memo: S of 4194304 binds skipped`. Temple (Windows,
    Pro): ~71% skipped; B ms/flip on 41.85 / 42.95 / 42.04 vs off 43.04 / 42.83 / 43.13 (-1 ms
    median, at the noise floor). Two memo-on runs during an overlapped (invalid) batch died at
    ~93 s in the new-game load with `UnregisterImage: ... already unregistered image`; none in 6
    clean runs. If that assertion shows up again, retry with `SOTC_BIND_MEMO=0`.
  - `SOTC_COPY_FAST=1` (session 49; default off): the CPU copy (`SotcCpuCopyCompute`) skips the
    zero-fill of its word vectors (`SotcWords`) and repeats the period with block `memcpy`s
    (`SotcCpuCopy::Run`; off: `RunSimple`, the per-dword loop). Temple, pipe on, three
    alternating pairs: A -1.2..-2.2 and B -0.4..-1.5 ms/flip, yet **fewer flips/s** (26.84/26.85/
    27.21 vs 28.01/27.80/27.71; A's queued faults waited longer): B is not CPU-bound alone (see
    the `pipe: B waits:` line under `SOTC_GPU_PIPE`). Kept off.
  - `SOTC_ASYNC_READBACK=0` (session 50; default on, only with `SOTC_GPU_PIPE=1`): stage B
    waits for queued faults' downloads again. On: B records the download copy into one of 8
    host slots (2 MiB each), submits it (a side copy for data in a submitted batch; for data in
    the batch being recorded it flushes the batch) and goes on. The `sotc:Readback` thread waits
    for the GPU and posts the finish to B as a command: B writes guest memory, takes the pieces
    out of `gpu_modified_ranges` and unprotects their pages, then wakes the faulting thread.
    Until then the pages stay protected and GPU-written, and B *settles* a readback (waits for it
    and finishes it) before it changes those bytes or their tracking (another download, a written
    binding, a CPU-copy commit, a fence label, an image download). Line every 10 s: `pipe: N
    async readbacks (in batch, side), settled S (W waited, ms; by site: ...), slots full, too
    big`. Temple: ~850 readbacks/s, ~60 settles/s (almost all `download`: a fault on a page whose
    readback is in flight), B's semwait and side-fence waits ~0. Pro mode, alternating:
    **28.91/29.56 vs 27.51/27.93 flips/s off (+5.4%)**, B now idle ~17% of the time.
    Session 53 (Windows, NVIDIA's second graphics queue): 17.49/19.07 vs 15.59/15.79 off (+15%).
  - `SOTC_GPU_PIPE` (session 43; **default `1` since session 53**, `0` off): the two-stage GPU pipeline
    (`docs/next/gpu-pipe.md`). Each draw/dispatch becomes a packet (register delta, extents,
    compute registers, pipeline, per-stage user-data snapshot) for the recording half (stage B).
    `sync` (build step 1): B runs each packet at once, on the same thread; same picture as off,
    +2.5% GPU-thread CPU (36.57 vs 35.69 ms/flip, temple). `1` (session 44, build step 2): B runs
    on its own thread, `sotc:GpuRecord`; every other call from Liverpool's thread into B's state
    (fences, DMA, submit/flush, commands, flips, the CPU-copy dispatch, its own faults) waits for B
    to finish first (a drain). Session 45 (step 3a): fences (EOP, EOS, ReleaseMem), submit-done,
    GPU idle and the flip IRQ run on B in stream order without a drain, and a `WaitRegMem` on a
    label a pending fence writes is met from its value (temple: 24.6/24.8 vs 20.9/22.6 flips/s
    off). Session 46 (step 3b): commands (guest-fault downloads, flips without GPU work,
    video-out registration) and Liverpool's own faults run on B between packets, without a
    drain. Session 47 (step 4): Liverpool's thread computes the CPU copy (`SOTC_CPU_COPY`) and B
    commits it; A reads the destination from its overlay until then, and an A read of a range a
    pending packet writes on the GPU waits for B (`pending`). No drains are left in the temple:
    26.1/26.3 vs 21.5/21.7 flips/s off. A second line every 10 s: `pipe: A copies N (MB), L left
    to B, R overlay reads, P pending ranges (B big); verify: W walks, D differ`. Logs `pipe: 10.0 s: N packets (F fences, E events), D drains (W waited, T ms), B idle
    I ms, M waits met by pending fences, C commands on B, Q A faults queued (T ms); <reason>
    calls/waited/ms ...` every 10 s. Session 49: a third line, `pipe: B waits: submit Nx T ms,
    refresh, semwait, acquire, present, present-fence, side-fence ...`: stage B's wall time in
    Vulkan calls that enter the kernel (`sotc_wait_timer.h`; queue submit, semaphore counter
    query, `waitSemaphores` under `Scheduler::Wait`, swapchain acquire/present, present fences,
    the download side-copy fence). Temple, Pro mode: semwait ~1.59 s and side-fence ~0.68 s per
    10 s (~23% of B), submit ~0.07 s. Session 51: `... hot flushes N` ends that line (the
    dispatches that submitted at once for a hot line), and a fourth line, `pipe: A waits: idle,
    vo-label, regmem-spin, ring-full, guest-read, guest-write, read-to-b, read-async, read-on-b`:
    stage A's wall time waiting for guest submits (`submit_cv`), in `WaitVoLabel`, in passes
    where every queue's task sat in a wait loop (`sotc_in_wait`), and asleep on a full ring;
    the guest threads' GPU page faults by kind; and a queued download's wait for B to start it,
    then for its data (left in flight, or done on B). Drains, A's queued faults and pending reads
    are in the first line. The `async readbacks` line ends with `start to GPU done` / `to finish`
    per readback. Temple, Pro mode, per 10 s: A idle ~3.4 s, ring-full ~1.6 s; guest reads ~840
    (~380 us each), writes ~40k (~9.4 us); readbacks ~275 us to GPU done + ~31 us to finish.
    Other values log a warning and run with it off.
    Session 53 (Windows, RTX 2060, Pro, altar 150-225 s, alternating): **on 34.4/19.8/19.2 vs
    off 12.9/12.9/13.1 flips/s** (the 34.4 run did a lighter workload: ~3x fewer render-state
    memo misses; the typical gain is ~+47%); B ~43 ms/flip, idle ~7%: on Windows B is the limit.
    `=64` verify run: 0 register differences, 0 of ~397k walks differ. Made the default.
    `SOTC_GPU_PIPE_DEPTH=N` caps packets in flight (default 1024; no effect in sync).
    `SOTC_GPU_PIPE_VERIFY=N`: every Nth packet also carries Liverpool's whole register file
    (208 KiB); B compares its delta-built copy with it and logs `pipe verify:` lines on a
    difference. Session 43 (sync, `=1`, 200 s): 0 differences; session 44 (`SOTC_GPU_PIPE=1`,
    `=64`, 200 s to the altar): 0; sessions 45 and 46 (same): 0. Session 47: B also re-runs
    each stage's SRT walker for those packets and compares the flattened user data with A's
    (`pipe verify: ... walker:` lines); `=64`, 200 s: 0 of ~317k walks differ.
  - `SOTC_GPU_CMD` (session 62, default `0`; `sync`): draw-prep shape D step 2a
    (`docs/next/draw-prep.md`, section 11). The `Record*` path encodes its Vulkan commands into
    a ring (`sotc_cmd_record.h`), and in sync mode B decodes them right after each record. A
    `pipe: cmd:` line every 10 s gives scopes, commands and the direct accesses that broke a
    scope, by site. Altar, Pro, alternating: B +3.6 ms/flip (~+8%), the same frames.
    Session 64: `1` starts stage C (`sotc:GpuCmd`), which records the commands; B drains it
    before every submit and direct command-buffer access (`docs/next/draw-prep.md`, section 13).
    The `pipe: cmd:` line also counts the drains that waited and B's time in them. Altar, Pro,
    alternating: ~+7% flips/s (19.8 vs 18.4), B -5 ms/flip, the same frames; C spins (~35
    ms/flip of CPU). Opt-in until step 3b.
    Session 65: `SOTC_GPU_CMD_SPIN=N` (us, default 50) is how long C spins for the next record
    before it sleeps; `SOTC_GPU_CMD_WAKE=N` (default 1) wakes a sleeping C only every N records
    (drains always wake it). The `pipe: cmd:` line counts the wakes and C's sleeps. Shorter spins
    or batched wakes lower C's CPU but cost B in wake-ups (section 14 of `draw-prep.md`): keep the
    defaults.
    Session 64 also added a stall watchdog (with `SOTC_GPU_PIPE=1`, always on): when B runs no
    packet for 3 s it logs `stall:` lines (what A, B and C wait on, the GPU's timeline value).
  - `SOTC_INDIRECT_PROBE=1` (session 66, debug only): copies every indirect draw's and dispatch's
    arguments to a host buffer on the GPU and logs a `probe: indirect:` line every 10 s: how many
    drew nothing (zero count), by vertex shader, whether the record at the same address one flip
    earlier was empty too, and the `ZPASS_DONE` dumps per flip (`docs/next/draw-prep.md`,
    section 15). It ends the render pass before each probed draw: frames stay the same, speed
    does not. Altar: ~3,530 indirect draws/flip, 12% empty; no occlusion queries. A
    `probe: direct:` line counts the empty direct draws (none) and dispatches (0.3%).
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
