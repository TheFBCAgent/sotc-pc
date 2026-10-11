# Debugging notes

Moved from `CLAUDE.md`. Tools, techniques and findings, in the order they were added.

## Debugging
- Crashes print `module+offset` for the faulting instruction and the stack words that point into guest code. Look offsets up in Ghidra, or in `build/game/boot.txt` for imports.
- Linux (session 27b): a fault outside guest code also prints `host code: lib.so+off (symbol)`
  and host stack words, the thread name, and, for code in no library (JIT: the SRT walker's
  `rwxp` buffer), its `/proc/self/maps` line. `llvm-symbolizer --obj=build-vk/sotc-runtime
  <offset>` (PIE: offset = ip - the base of its `r-xp` mapping at file offset 0).
- Linux, gdb 17 / lldb 22 (session 29): `gdb -x scripts/sotc.gdb --args build-vk/sotc-runtime
  build/game/boot.img` (env as smoke.py sets it). It passes the constant tracker SIGSEGVs on
  silently and stops at `fault_reported` (`main.c`, after the crash report, before `_Exit(3)`):
  `bt` shows the faulting frames above `<signal handler called>`, `info threads` names them.
  The game runs under it, slower (~11 flips/s at boot vs ~90). Old core dumps: `coredumpctl
  info -1` (stack), `coredumpctl gdb -1`.
- Linux, perf 7.1 (session 30): `perf record -g --call-graph dwarf,16384 -F 999 -t <tid> -o
  x.data -- sleep 18` on the `shadPS4:GpuComm` thread (`ps -L -o tid,comm -p $(pgrep -x
  sotc-runtime)`), then `perf report --no-children|--children --time <a>,<b> --stdio -g none`
  for one freeze (times from `perf script -F time | head -1` + offset). Flip gaps: the
  `videoout: flip N` lines (one per flip). On Linux `plat_query_prot` parses `/proc/self/maps`
  (~8 ms in gameplay, thousands of mappings): keep it off hot paths. The bridge never asks
  for arena pages (`ensure_known_ex`): every arena map goes through `gpu_bridge_guest_map/remap`
  and pages mapped before the GPU started enter the page table at init (`replay_map`).
  The `UpdatePageWatchers: ... not fully GPU mapped` warnings (~1500 per run) are SotC's
  streamed textures (a reserved range, its mip tail mapped first): harmless.
- Linux fps checks (session 31): a save now exists in `build/user/savedata`, so `run-vk.sh`'s pad
  script continues into the **temple at ~100 s** (before, it only reached the post-new-game
  cutscene). Flips per 10 s from the `videoout: flip` lines; the temple is very steady (+-0.1).
  Whole-process profile: `perf record -F 499 -g --call-graph dwarf,8192 -p <pid> -- sleep 10`
  at ~150 s; per thread `perf script -F tid,comm | sort | uniq -c`; the user frame under kernel
  samples (`[unknown]` = kernel, paranoid 2) names the syscall. Headless runs save no shots.
  Temple: GPU ~20% busy, the GPU thread ~80% on-CPU: **CPU-bound on the GPU thread**. Syscalls
  are dear on RADV: `vkGetSemaphoreCounterValue` is an ioctl (session 31 throttled it in
  `PopPendingOperations`, 17.0 -> 20.3 fps). The game's 4 `BPE JobWorker` threads spend ~55% in
  the kernel on guest mutex futexes (contention, not the critical path).
- Linux GPU-thread profiles (session 32): `perf script -i x.data --tid <GpuComm tid> -F
  tid,ip,sym,dso > chains.txt`, then `python scripts/perf_chains.py chains.txt self|incl|callers|
  children|from ...` (percent of that thread's samples; inlined frames count as frames).
  **Flips/s is quantized in the temple**: flip intervals are ~33 or ~67 ms (about half each),
  so a CPU saving shows only when frames cross 33 ms. Compare GPU-thread CPU per flip too:
  `utime+stime` of `/proc/<pid>/task/<tid>/stat` at 110 and 195 s / flips in between (~38-40
  ms per flip). `scripts/flipcpu.sh <label>` (session 33) does a run and prints both. Hot leaves sit one instruction late (skid): 188 samples on a `call` after
  `vzeroupper` were the call's neighbours, not the call. The baseline drifts between sessions
  (session 33: 39.0/39.5 ms per flip vs session 32's 38.3/38.5 for the same code), so measure
  the baseline in the same session.
- Session 33: `hle_memory_clamp_range` (`is_gpu_mapping` from `BindTextures`/`BindBuffers`,
  ~1.1M calls/s in the temple over ~10 regions per draw) answers from a per-thread sorted
  snapshot of the mapped spans without g_lock, valid while `g_regions_gen` is unchanged; a
  thread builds it after 256 locked walks at one generation (test `test_memory_clamp`).
- Session 34: `BufferCache::IsRangeGpuWritten` (~1M calls/s in the temple, nearly all from the
  SRT walker's `ResolveSrtTable`, ~0.1% true) checks `SotcWrittenPages` (4 KiB page bitmap,
  `buffer_cache/sotc_written_pages.h`, test `test_written_pages`) before the icl search. Any
  new `gpu_modified_ranges.Add`/`Subtract` must also `Mark`/`Unmark` it, or a "no" goes wrong.
  Baseline runs for `flipcpu.sh`: copy the old binary to its own directory under the same name
  (`build-vk-base/sotc-runtime`, pass `--exe`): the script finds the game by `pgrep -x
  sotc-runtime`, and a renamed binary is left running and overlaps the next run.
- Session 35: `hle_memory_read_sparse` (the stream copy of `ObtainBuffer`) is ~169k calls/s
  in the temple, ~4 KiB each, nearly all one `view` piece; the copy is ~3/4 of its time, the
  lock + `span_at` ~1% of the GPU thread, so no lock-free path was built (numbers in NEXT.md).
  Measure such splits with a temporary `rdtsc` counter: its own atomics inflate the
  function's perf self time.
- Session 36: `TextureCache::FindImage` (~670k calls/s in the temple, half render targets from
  `PrepareRenderState`, half `BindTextures`) asks `SotcFindCache` first
  (`texture_cache/sotc_find_cache.h`, test `test_find_cache`): perfect matches keyed by every
  request field, emptied by a generation bump in `RegisterImage`/`UnregisterImage`. 92% hits;
  misses are the per-tick GC's bumps and overlap resolves (never cached). Anything new that
  changes which image a request matches must go through Register/Unregister, or bump it.
  `ComputeImageSize` (2%) runs in the `ImageDesc` constructor, before the cache.
- `--shots N` hitches the game ~250-300 ms per shot (session 36): a stutter every N s in a run
  with shots is the screenshot, not the game.
- If the process dies with 0xC0000005 and no report:
  - Null GPU: use `scripts/crashcatch.py`. It prints registers, the page state at the fault and optional memory dumps.
  - Vulkan: crashcatch is unusable. GPU page tracking faults constantly, and under a debugger the game sits at 0 FPS on
    the first logo. Instead, read the Windows event log:
    `Get-WinEvent -FilterHashtable @{LogName='Application'} | ? Message -match 'sotc-runtime'` gives the exception code
    and the fault offset. Then use `llvm-symbolizer --obj=build-vk/sotc-runtime.exe --relative-address <off>`, or
    `llvm-objdump -d --start-address=0x7000000<off>` (the exe's ImageBase is `0x700000000000`).
  - A silent death usually means a fault while `rsp` was on an inaccessible page: Windows can't push the exception
    frame, so no handler runs. Example: a fiber stack the GPU tracker had protected. Fiber stacks are now pinned
    (`gpu_bridge_pin_stack`).
  - `nested fault <code> <rip> <rsp>` means host code faulted inside the fault handler, which runs on a per-thread
    emergency stack. Host code on the fault path (sotcgpu `InvalidateMemory` → texture cache) must never read
    guest memory in place, because pages may be read-protected or briefly unmapped. Read through
    `Memory::CopySparseMemory` (the runtime's `read_sparse`). Example: crash C, the hash in `MarkAsMaybeDirty`
    (`sotc_guest_hash.h`, test `gpu/tests/test_guest_hash.cpp`).
- `SOTC_LOG=3` traces memory maps, thread creation and file opens.
- GPU (see `docs/m3/bringup.md`, "Steps A–C"):
  - Check the scheme in this order: burst shots, then a render-target dump of one frame, then the shader IR
    dumps. Tools (details in `docs/next/x.md`; all free when unset):
    - `SOTC_DUMP_RT=<dir>` (with `--shots N`): at the next screenshot tick, records the color/depth targets
      one frame binds and writes each as PNG plus `<dir>/index.txt` (address, format, size, draws, min/max,
      `UNIFORM` = untouched). One-shot; `SOTC_DUMP_RT_ALL=1` dumps on every tick;
      `SOTC_DUMP_RT_SKIP=N` ignores the first N ticks (to dump in gameplay).
    - `SOTC_GPU_DUMP=1`: shadPS4 `dump_shaders` (IR text and SPIR-V in `build/user/gpu_cache/shader/dumps`).
    - `SOTC_GPU_STATS=1`: logs per second draws, indirect draws, dispatches, blocking scheduler waits,
      guest→GPU copy MiB, GPU read/write faults and `walker aliased/emulated`, then a `sched-waits by site:`
      line (who waited, and buffer downloads by when the GPU wrote the data: in batch / submitted / done,
      side copies, hot flushes). Every 10 s: `fault pages` (top faulting and draining pages: thread, r/w,
      PM4 packet, the host reader, the written bindings covering the page, which downloaded bytes changed)
      and, once per shader and table, `srt gpu-written table:` (what GPU-written walker leaves feed).
      Stats cost time (a mutex per fault, a compare per download): compare fps with it off.
      After `|` (session 12): `gpu busy X ms/s (main, side; timed/untimed/in flight)` from Vulkan timestamps
      around every submission on both queues (union, overlap once; read without waiting, so it trails by
      up to 1 s; `gpu/src/gpu_timer.cpp`, math in `sotc_gpu_busy.h`), `submits`, `flips`, and `gpu-thread
      wait` (blocking `Scheduler::Wait` + side-copy fences on the GPU thread; `side-copy fences` part).
      Per-10 s table: `python scripts/gpustats_table.py build/game/logs/<run>.log [start_s]`.
      Session 13 lines: `download recency` (downloads whose 64 KiB pages were also downloaded in the last
      2 frames, by write tick; distinct pages and MiB per second) and, with `SOTC_PREFETCH_READBACK=1`,
      `readback prefetch` (hits, hits after a wait, misses by reason, pages and MiB copied).
      Session 15: the 10 s report's `in-batch drains` line and table (each a `Scheduler::Finish`):
      page, reader (GPU-thread packet + host rip, or guest rip), newest covering written binding,
      whether its line was already hot and whether its own 64 KiB was written in the batch.
      Session 17: `reuse <reader>` lines (per reader: lines/pages, how many frames since the same
      line/page last drained, whether its own line was written in the batch, and the in-batch writers
      on its 64 KiB as compute shader hash + binding size) and `reuse feeders` (per writer: drains fed
      vs dispatches). Sum runs with `python scripts/reuse_table.py <logs...> [--start 110]`. A guest
      rip `0x0` is a page the fault table dropped.
      Session 19: `gpu-thread fault #N (X ms/s, faults, drains, packet) page ...` ranks the GPU
      thread's own fault pages by time spent (the table keeps a quarter for them), with the reader
      and, for the SRT walker (rip "no module"), `walker: shader table at +bytes, <outcome>` (what
      `ResolveSrtTable` did: aliased, not GPU-written, extent unknown, patch off, not patchable,
      patched). `srt gpu-written table` lines classify descriptors with `SharpDwords`.
      Session 21: the `written by` bindings name their writer (`cs <hash>`), and with
      `SOTC_CPU_COPY` set a `cpu copy:` line counts copies done on the CPU and left to the GPU.
    - Readback waits are latency, not copy cost (session 13): a download waits for the batch that wrote
      the data however it is copied. In gameplay ~2/3 of the GPU-thread wait is `Finish` for data in the
      batch being recorded (~20/s, ~16 ms each); side-copy fences are the other ~1/3.
  - **Tracy** (session 18, `build-tracy`, client v0.14.1 in `third_party/tracy`; the tools in `D:\tracy`
    are 0.14.1 too, and the protocol must match). On demand: costs nothing measurable until a client
    connects (9.1-9.4 fps vs 9.2 for build-vk at the altar). Unattended capture during a run:
    `D:\tracy\tracy-capture.exe -o build\game\tracy\x.tracy -a 127.0.0.1 -s 60 -f` once the game is in
    gameplay (~150 s with run.bat's pad script; 60 s is ~300 MB and ~50 M zones). Then
    `D:\tracy\tracy-csvexport.exe -u x.tracy` (one row per zone; drop the `guest mutex wait` rows with
    awk first, they are ~40 M) and `python scripts/tracy_frames.py zones.csv`: per thread, self ms/frame
    per zone (frames = `SubmitFlip` zones) and where the GPU thread's waits sit. Thread ids in the export
    are Tracy's indices, not names (the GPU thread is the one with `PM4` zones).
    Zones: GPU side `PM4 gfx/compute queue`, `SubmitFlip` (+ `FrameMark`), shadPS4's Draw/Dispatch and
    `RefreshImage`, `Scheduler::Finish`/`Wait`, `SotcSideCopy fence`, `BufferCache::ReadMemory download`,
    `GPU page fault` (value = address; on whichever thread faulted), `Compile shader`, `Create ...
    pipeline`, `Image GC`; runtime side (`runtime/src/prof.h`, Tracy's C API) `guest mutex wait`
    (contended only), `guest cond/evflag/equeue wait` (text = object name), `guest join/sleep/vblank
    wait`. Thread names go to Tracy through `plat_thread_set_name`.
    Session 20: ~45 finer zones inside Draw/Dispatch (`SOTC_ZONE*` macros in shadPS4's `common/debug.h`,
    list in `third_party/shadps4/README.md`): SRT walker, `GetProgram`, `BindTextures`/`FindImage`,
    `BufferCache::ObtainBuffer` split into `OB ...` pieces, descriptor push, barriers, `vkCmd*`. They
    emit ~3.8M zones/s (~15% fps while a client is connected), so capture 15-20 s, not 60, and read
    them with `python scripts/tracy_frames.py x.tracy --per-draw` (`tracy-csvexport -e`: aggregated
    self time per zone; per frame, per draw/dispatch, per call). Per thread (session 32):
    `python scripts/tracy_threads.py x.tracy` (filtered `-u -f` exports of the coarse zones:
    faults and guest waits per thread, the GPU thread's zones nested, and the chain above every
    wait, e.g. a download straight under `PM4 gfx queue` is a guest fault's). A per-event export is too big,
    except for one zone: `tracy-csvexport -u -f "Compile shader" x.tracy` (session 27; the
    `Compile shader` value is the shader hash).
  - For time spent: `python scripts/sample.py sotc-runtime.exe 15 GpuCommandProcessor JobWorker` samples the
    named threads of a running game (suspend, RIP, stack scan, `llvm-symbolizer`) and prints top leaf and
    inclusive functions. Pass `--top 500` to group every leaf by subsystem; `--callers REGEX` lists the
    first 1-3 exe frames above leaves matching REGEX (session 15: the DDI fence wait is
    `Scheduler::Finish` → `Wait` → `Semaphore::Wait`, `ZwWaitForMultipleObjects` is `SotcSideCopy`'s fence); a leaf in a driver is named by its
    nearest export (`nvoglv64!vkGetInstanceProcAddr` is driver-internal code), so find its callers from the
    first exe frames on the stack (session 14). `NtGdiDdDDIWaitForSynchronizationObjectFromCpu` and
    `ZwWaitForMultipleObjects` are waits. `--self-test` checks it without the game. Rebuild only after
    the game exits: a failed relink (exe in use) leaves a PDB that mislabels inclusive frames. GPU load:
    `nvidia-smi --query-gpu=utilization.gpu,memory.used --format=csv -l 1` (utilization = time any kernel runs, not
    real load; Task Manager "3D" may miss Vulkan queues; per-engine counters: see NEXT.md, frame rate part 2).
  - Frame rate varies 3x between identical runs (gameplay at the altar: 2.5-10 fps, VRAM at the budget).
    Compare builds on several runs, or on counters (sched-waits, downloads), not on one fps number.
    Session 16: the GPU's slow state (GPU ms/frame = busy / flips ~70-160 vs ~35) is **VRAM over
    budget**. With heap0 (~5.1 GB) over the budget Windows grants (4.9-5.2 GB, depends on other apps),
    WDDM pages allocations; once a hot GPU-written one lands in system memory the GPU works over PCIe
    for the rest of the run. Signs: `nvidia-smi dmon -s pucmt -d 2 -o T` shows SMs 90%+, VRAM
    bandwidth (`mem`) ~4%, `txpci` 6-10 GB/s; paging copies run on the **System process (pid 4)**
    Copy engine (`\GPU Engine(pid_4_*engtype_copy)\Running Time`), not the game's; the game's queues
    are engine `graphics_1`. `SOTC_TEX_DOWNSCALE` (default on here) keeps heap0 1-1.6 GB under budget
    (no slow window in 4 runs, including temple → first colossus). Pass `SOTC_TEX_DOWNSCALE=0` to see
    the slow state again.
  - GPU drains (session 10): a CPU read of GPU-written memory faults (precise readbacks) and downloads it.
    - The SRT walker (JIT code, the GPU thread, every draw/dispatch) reads tables through the unprotected
      physical backing view when its page is GPU-blocked but its bytes aren't GPU-written (byte-precise
      `gpu_modified_ranges`; `SrtResolveTable` → `ResolveSrtTable`). A load that still faults on CPU bytes
      is emulated (`sotcgpu_emulate_load`, `gpu/src/sotc_srt_load.h`).
    - Downloads of data written by a submitted batch (`SotcWriteTicks`, per 64 KiB) are copied on a second
      graphics-family queue (`SotcSideCopy`); only data in the batch being recorded takes `Finish`. A
      dispatch writing a line that once needed that submits its batch right away (hot flush).
    - Most remaining downloads are real: SotC's per-view constants (a 0xb00 binding at `0x1042710110` a
      dispatch rewrites) and small structs guest threads read and write each frame.
    - The GPU thread's own faults (~20 ms/frame at the altar, session 19) are the SRT walker reading
      tables compute shaders rewrite each frame, and nearly all hold **GPU-written descriptors** the
      host must read to bind. Two once-per-frame 4-dword descriptors cost ~16 ms/frame; their values
      never changed in downloads (rewritten with the same values), see NEXT.md "Later".
  - VRAM: the `[vram] images:` line (every ~10 s) ends with `| working set:`: images used in the last 8
    ticks, distinct images touched per GC tick, guest MiB summed vs. unique, overlapping images, and
    images re-created soon after a GC free (thrash). In critical mode "unused <60 ticks" is always 100%:
    the GC frees anything older, so read the working-set fields instead.
  - VRAM exhaustion no longer asserts at once: `[vram] emergency gc (arena|image, ...)` frees images not
    used this tick and the allocation is retried (`failed retries` in the `[vram]` line; an assert follows
    only if the retry fails). Images have dedicated allocations, so a freed image returns its memory at
    once. `arena ranges X MiB, Y MiB (Z%) inside live images` in the `[vram]` line is guest memory
    resident both in the arena and in images (~11% in gameplay).
  - A black screen from the first logo with frames still flipping: look for `SrtWalkerSignalHandler:
    Patched SRT walker` lines at ~2 s (null SRT pointers). The walker's null guard (`PushPtr`) fixed it;
    if they come back, the fault is past the 64 KiB zero block or a non-null bad pointer.
  - RenderDoc stalls the game (sparse arena), so it's not usable here.
  - A GPU-thread fault reading a PM4 header at a guest-unmapped address (session 29, the menu
    crash): the parser must not touch packet memory after the packet signalled the guest. EOP,
    EOS and WRITE_DATA write labels synchronously on the GPU thread, and the guest may free the
    command buffer at once. `ProcessGraphics` steps with the count read up front; test
    `gpu/tests/test_cmd_lifetime.cpp` (the EOP handler revokes the DCB page).
  - Black objects that come and go with VRAM pressure: suspect the image GC first. Rebuilding a freed
    image from guest memory loses whatever the GPU drew into it (the `stale` count in the `[vram]` line).
  - SRT walker errors (`ComputeOffset ... Phi`) and `SRT walker: N failed offsets in shader 0x...`
    name the shader: put the hash into `SOTC_GPU_SKIP` to see what it draws.
    - A ReadConst indexed by a loop counter (Phi) is served from a 16 KiB window of its table that
      the walker copies into the flattened buffer (`srt.h`, `ReadConstDynamicWindowFlag`; logs
      `SRT walker: N runtime-indexed reads from a window`). The `ComputeOffset ... Phi` errors still
      print for those and are harmless. Without it they read user data s0: SotC's blocky depth of
      field (session 8).
    - `Sharp source uses a runtime index`: a descriptor fetched with a loop index, which a window
      can't serve (shaders `0x9d6f9bf4`, `0xfb974c0e`). Still unhandled.
  - `ReadLane instruction in non uniform control flow in shader 0x...` (lower_wave64_pass): a wave64
    ReadLane left unlowered on 32-wide subgroups. It reads past the subgroup, so the two halves of a
    64-thread group disagree. A branch on an already-lowered ReadLane/Ballot now counts as uniform
    (session 9: SotC's tiled lighting `0xfb974c0e`, 8x8 tiles, gave 4-row horizontal stripes); none
    are left in gameplay. If one comes back, skip its shader first.
  - Finding a compute shader by shape: scan the SPIR-V dumps (`*.spv`) for `LocalSize` > 32 with
    subgroup ops or Workgroup variables (session 9 narrowed ~450 compute shaders to 30 this way).
  - Wave64 assumptions on 32-wide NVIDIA subgroups (session 11, the sky): a `ReadLane 32` in a workgroup
    of <= 32 threads reads the identity of its reduction (`LowerReadLanesPastWorkgroup`; an unknown op
    is folded and logs `ReadLane(s) past the N-thread workgroup folded`), and LDS workgroups that aren't
    64 threads get subgroup memory barriers between LDS writes and reads (`SharedMemoryBarrierPass`).
    Both changed SotC's exposure. A wrong-brightness bug that drifts over time: suspect these first.
  - Pick shots by number, not time: the route isn't deterministic after the scripted presses (some runs
    start at the altar, some on the terrace outside; a drop in the `[vram] ... tick` rate marks the
    terrace). Intermittent horizontal stripes on shadowed stone (1 shot in ~60, sessions 10 and 11) are
    still open.
  - TDRs: `Get-WinEvent -FilterHashtable @{LogName='System';ProviderName='Display','nvlddmkm'}`.
  - Blue screens: `Get-WinEvent -FilterHashtable @{LogName='System';Id=1001}` gives the bugcheck. Windows
    deletes `MEMORY.DMP` when C: has < 25 GB free, so enable minidumps (admin:
    `wmic recoveros set DebugInfoType = 3`) to see which driver crashed. Session 14 had one (0x50, during a
    run with `SOTC_ASYNC_IMAGE_FREE=1` while `sample.py` was suspending the GPU thread; no dump kept).
- The image loads at `0x10000000` on Windows (`0x400000` is taken). Guest heap mappings live in the arena from `0x1000000000`.
