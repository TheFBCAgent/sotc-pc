# shadow-pc

A native PC port of **Shadow of the Colossus (PS4, 2018)**, bbport-style. The PS4 CPU is x86-64, so the
game's own code runs directly on the host. There is no emulation and no recompilation.

- An offline converter links the game into a **boot image**.
- A small C **runtime** loads it and provides the PS4 system libraries through HLE.
- The GPU is shadPS4's `video_core` (vendored in `third_party/shadps4`) behind a C interface (`gpu/`, "sotcgpu").

Docs:
- `README.md`: the user-facing guide (setup, scripts, status, known issues).
- `docs/CHANGELOG.md`: every fix so far, one line each, with its result and off switch.
- `docs/m3/bringup.md`: M3 Vulkan bring-up, findings and debugging techniques.
- `docs/RECON.md`: results (M2 reached the main loop).
- `docs/MILESTONE2.md`, `docs/MILESTONE3.md`: earlier plans. `docs/RESEARCH.md`: background.

Read when needed:
- `docs/ENV.md`: the runtime and GPU environment variables (`SOTC_*`), what each does and what it measured.
- `docs/next/gpu-pipe.md`: the two-stage GPU pipeline (session 42 design, built from session 43 on):
  stage ownership, the rules that keep the invariants below across threads, drain points.
- `docs/next/draw-prep.md`: draw preparation off stage B (sessions 59-60). It covers what bbport
  built, B's cost per record, and the chosen shape D: a third stage C that records the Vulkan
  commands B resolved, plus its rules, invariants and build order.
- `docs/DEBUGGING.md`: crash reports, debuggers, profilers (perf, Tracy, sample.py, Nsight Systems via
  `scripts/nsys.sh`), GPU debug tools and stats, and the findings behind them, session by session.
- `docs/PGO.md`: LTO/PGO builds and regenerating the profile.

## Hard rules
- **Never commit game data, firmware or keys.**
  - `.gitignore` covers `*.bin *.prx *.sprx *.pkg *.psarc CUSA*/`.
  - `build/` and `reports/` are local only, because they hold game-derived output. So are PGO
    profiles.
- **Use only your own dump.** Its folder comes from `SOTC_GAME_DIR` (firmware: `SOTC_FW_DIR`).
- `../shadps4` (commit `14ada48`, vendored in `third_party/shadps4`) is a GPL-2.0 reference. This repo is GPL-2.0-or-later, so porting code is fine; credit it in a comment.
- Commit only when asked.

## Rules that prevent bugs
Invariants a code change must not break (background in `docs/DEBUGGING.md` and `docs/ENV.md`):
- Every `gpu_modified_ranges.Add`/`Subtract` also `Mark`s/`Unmark`s `SotcWrittenPages`, or `IsRangeGpuWritten` answers "no" wrongly.
- Anything that changes which image a request matches goes through `RegisterImage`/`UnregisterImage` (or bumps `SotcFindCache`).
- Every `FindImage` result that is not a stored perfect match bumps `sotc_uncached_finds`, or the
  texture-set memo (session 40) keeps an overlap resolve.
- Anything that takes bytes out of `gpu_modified_ranges` (`Subtract`/`Unmark`) or changes the hot
  lines bumps `sotc_note_gen`, or the written-binding memo (session 48) skips a needed
  `SotcNoteGpuWrite`.
- Anything that marks a metadata surface cleared goes through `ClearMeta`/`TouchMeta` (they bump
  `sotc_meta_clears`), or the render-state memo (session 38) keeps a stale "no clear" state.
- Host code on the fault path never reads guest memory in place: read through `Memory::CopySparseMemory` (`read_sparse`).
- The PM4 parser never touches packet memory after the packet signalled the guest (EOP, EOS, WRITE_DATA).
- Two-stage GPU pipeline (session 43, `docs/next/gpu-pipe.md`): every write to Liverpool's
  `regs` also marks `sotc_dirty` (`SotcMarkRegs`/`SotcMarkField`), or stage B's register copy
  goes stale (`SOTC_GPU_PIPE_VERIFY=1` finds it). Recording code reads registers only through
  `Rasterizer::Regs()`/`CbExtent()`/`DbExtent()`/`CsRegs()`, never `liverpool->regs`, and a
  stage's user data only through `Info::UserData()`/`FlatUd()`/`PgmBase()`/`FlatPatches()`
  (`GetSharp` does), never the members.
  With `SOTC_GPU_PIPE=1` (session 44; the default since session 53) stage B runs on `sotc:GpuRecord`: a call from Liverpool's
  thread into B's state (caches, scheduler, presenter) holds the guard
  `Rasterizer::SotcPipeDrain` returns for as long as it touches that state (session 46: B runs
  commands whenever it is idle, and the guard's `state_mutex` is what stops it; `SotcPipeWait`
  only waits); fault-path "GPU thread" tests use `SotcFaultOnGpuSide()` (A while it holds a
  guard, or B; A's other faults are queued to B); `gpu_modified_ranges` changes only under
  `sotc_ranges_mutex`. Never call `SendCommand<true>` from B or from A holding a guard
  (B would never run it).
  Session 45: fences, submit-done, idle and the flip IRQ are packets B runs (A copies the PM4
  packet; B never reads PM4 memory); a new A-side reader of a fence label checks
  `SotcPendingFences` (via `SotcPipeWaitMet`) before reading memory.
  Session 47: A computes the CPU copy and B commits it; a new A-side reader of guest memory
  (walker paths, CPU-copy inputs) goes through `SotcPipeOverlay`/`SotcPipeMergeRead` (pending
  overlays and GPU writes), and `SotcPendingRanges::Retire` runs only at the start of
  `SotcPipeSubmit` (a walker may hold an overlay pointer). A new written binding kind on B
  needs a matching `SotcPipeNoteWrites` entry on A.
  Session 50 (`SOTC_ASYNC_READBACK`): a download may be in flight on B (its pieces still
  GPU-written and protected until B's finish). New B code that changes guest bytes or their
  GPU tracking (`gpu_modified_ranges`, tracker GPU/CPU marks, `TryWriteBacking`) calls
  `BufferCache::SotcReadbackSettle` on the range first.
- `SOTC_DESC_CACHE=1` (session 55, opt-in) caches descriptor sets by handle: new code that
  destroys an image view, sampler or buffer bound by the rasterizer calls
  `SotcDescCache::Dead` before the handle dies (the `ImageView`/`Sampler`/`UniqueBuffer`
  destructors do), or a recycled handle binds a stale set.
- `BufferCache::resident_ranges` only grows (session 56): `EnsureResident`'s memo
  (`sotc_resident_memo`) relies on it. Code that ever unbinds arena memory clears the memo.
- Pipeline binds on a `Scheduler`'s command buffer go through `Scheduler::BindPipeline` (session
  57 memo, `SOTC_BIND_MEMO`); code that binds raw on one (the presenter's FSR/post-process
  passes, ImGui) calls `SotcForgetPipelines()` after it, or the next bind is skipped wrongly.
- Bump `kPipelineCacheVersion` (`gpu/src/sotcgpu.cpp`) when a patch changes the generated SPIR-V.
- Every local patch to `third_party/shadps4` gets a row in `third_party/shadps4/README.md`.
- Keep `plat_query_prot` off hot paths on Linux (it parses `/proc/self/maps`, ~8 ms in gameplay).
- Only one process uses the GPU at a time (Vulkan game runs and `test_sotcgpu_*`).
- Compare frame rates with alternating fix and baseline runs in the same session (`scripts/flipcpu.sh`;
  Windows `python scripts/flipcpu.py <label> --exe ...`, altar 150-225 s), never on one fps number.
  Measure with the window: headless runs skip `SOTC_PAD_SCRIPT`. Measure in PS4 Pro mode: both
  scripts pass `SOTC_NEO=1` by default (`NEO=0` / `--neo 0` for base mode).
  Stage B's GPU waits are in the `pipe: B waits:` line (session 49): a B-side CPU saving shows
  in flips/s only while B is not waiting. Stage A's and the guest's are in `pipe: A waits:`
  (session 51). Check `hot flushes` on the B line in every measured log: a random hot-flush storm
  (session 51, ~1 run in 3) costs 10-35% flips/s; rerun such a run (normal: ~200-340/s).
  Session 52's per-line budget (`SOTC_HOT_BUDGET`) caps them; `(denied M)` shows it working.
- Windows, measuring another build (session 37): run its exe from `build-vk\` under another name
  (exes in new build directories can't create files outside their own tree: no screenshots, no
  `build/user`), and warm a new exe name with one short run first (NVIDIA's shader cache is per exe).
- A fresh build directory is RelWithDebInfo unless told otherwise; check `CMAKE_BUILD_TYPE` in its
  `CMakeCache.txt` before measuring (clang-cl's `project()` defaults to Debug, ~8x slower).
- A fiber can resume on another host thread: a host `thread_local` read across `sotc_fiber_swap`
  must not be inlined into the fiber code (LTO cached `t_current` in `fiber.c`; it builds `-fno-lto`).

## Commands (Windows: Git Bash or PowerShell)
```sh
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl     # once (clang on Linux)
cmake --build build --parallel 8                              # runtime + one test exe per runtime/tests/test_*.c
ctest --test-dir build                                        # C tests
cmake -S . -B build-vk -G Ninja -DCMAKE_C_COMPILER=clang-cl -DSOTC_VULKAN=ON   # Vulkan GPU build (sotcgpu)
cmake --build build-vk --parallel 8; ctest --test-dir build-vk            # test_sotcgpu_headless causes a TDR (known)
cmake -S . -B build-tracy -G Ninja -DCMAKE_C_COMPILER=clang-cl -DSOTC_VULKAN=ON -DSOTC_TRACY=ON  # Tracy profiling
       # build (see docs/DEBUGGING.md, "Tracy"). A Debug build runs the game ~7x slower.
cmake -S . -B build-vk-lto ... -DSOTC_VULKAN=ON -DSOTC_LTO=ON   # thin LTO over everything, lld
       # (Windows: add -DCMAKE_LINKER=lld-link). PGO and its profile: docs/PGO.md. Profiles are
       # game-derived behaviour: never commit them. build-vk stays the iteration build.
python -m unittest discover -s tools/elfscan                  # Python tests
python -m unittest discover -s tools/sotcconv
export SOTC_GAME_DIR='<your dump>/uroot'
scripts/convert.sh   # or scripts\convert.ps1 -> build/game/boot.img + boot.txt (all imports)
python scripts/smoke.py --seconds 60   # run the game; prints the unimplemented imports in call order,
                                       # stubs hit, errors and frames. Full log in build/game/smoke.log
python scripts/smoke.py --exe build-vk/sotc-runtime.exe --gpu vulkan --seconds 120 --shots 10
       # --shots N: a screenshot every N s into build/game/shots/ (a black 1080p frame is 82420 bytes);
       # each run's shots are also copied to build/game/shots/<time>-<label>/ (named like its log)
       # --threads: per-thread CPU time; --timing: wait accounting. smoke kills the game at --seconds.
scripts\run.bat --label dof [--seconds 300] [--env "SOTC_GPU_SKIP=a,b"]   # Windows: the Vulkan run above with
       # the new-game pad script, --seconds 600 --shots 10; clears leftover debug variables.
       # Quote --env values: PowerShell splits lists at commas and reads hashes like 689583e5 as
       # numbers. (smoke.py --env NAME=VALUE is repeatable.)
python scripts/crashcatch.py --seconds 60 [--dump r14:0x80]   # Windows: run under a tiny debugger; catches
                                       # faults the runtime can't report (e.g. a blown stack)
```
Linux: the same commands with `-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++` and
`build-vk/sotc-runtime` (no `.exe`). Submodules: `git submodule update --init --depth 1`, then the
same inside `third_party/sirit` and `third_party/zydis`. `scripts/convert.sh` needs the shadPS4
checkout (`../shadps4` or `SOTC_SHADPS4`, commit 14ada48) for import names: without it every
import stays a bare NID, nothing resolves and the game dies at once.
`scripts/play.sh` / `scripts\play.bat`: play runs (no pad script, no shots, no time limit, base
mode; `--pro` for Pro). Logs: `build/game/logs/<time>-play.log`. When a setting becomes
recommended for playing, add it there (and to the README).
`scripts/run-vk.sh` is `run.bat` for Linux: same defaults, pad script and cleared variables, the
window on (`--env SOTC_HEADLESS=1` runs without a window but also without the pad script, so it
stays at the title: not for gameplay measurements).
Steam Deck (SteamOS; read-only root, no compiler on the host): the toolchain lives in an Arch
distrobox, `distrobox create --name sotc-dev --image docker.io/library/archlinux:latest`, then
inside it `sudo pacman -S clang lld llvm cmake ninja python-capstone pkgconf base-devel
vulkan-icd-loader vulkan-radeon libx11 libxext libxrandr libxcursor libxi libxss wayland
wayland-protocols libxkbcommon libdecor alsa-lib libpulse pipewire`. Run every Linux command above
as `distrobox enter sotc-dev -- <cmd>` (the GPU, `$HOME` and `/run/media` are visible inside it).
Requirements: clang 19+ (clang-cl on Windows; MSVC can't compile `sysv_abi`), CMake 3.24+, Ninja,
Python 3.10+, `pip install capstone`.

## Map
- `tools/elfscan/elfscan.py`: parses PS4 ELF/fSELF files (imports by NID, relocations); `load_aerolib`/`load_shadps4` name NIDs.
- `tools/sotcconv/sotcconv.py`: eboot + `sce_module/{libc,libSceFios2}.prx` → `boot.img`.
  - The format is in `runtime/include/bootimg.h`; keep the two in sync.
  - `codescan.py` decodes the exact patch sites (fs TLS reads, SSE4a) inside `.eh_frame` functions only.
- `runtime/src/main.c`: startup order is
  1. `log_init`
  2. `plat_tls_init` and the fault handler
  3. `hle_init`, then the kernel, memory, thread and file inits
  4. `loader_load`
  5. `tls_init`
  6. `hle_run_main`: the guest main thread runs the libc and Fios2 inits, then jumps to the eboot entry
- `runtime/src/loader.c`: maps the image (below 1 TiB), resolves imports through `hle_lookup`, applies relocations and patches, sets protections.
  - Unresolved functions get a trap stub that logs `unimplemented lib:name`.
- `runtime/src/tls.c`: guest TCB/DTV (shadPS4 layout) and `__tls_get_addr`. `sse4a.c` emulates SSE4a; it's only needed on Intel/Linux.
- `runtime/src/platform_{win,posix}_{mem,thread,fault}.c`: memory reserve/commit, the guest TLS register, threads, sync, time, faults.
- `runtime/src/window.c`: the SDL3 window and main-thread event loop. Also gamepad and keyboard input (→ `hle/pad.c`) and the FPS shown in the window title.
- `runtime/src/gpu_bridge.c`: runtime ↔ sotcgpu.
  - Display gamma (the game's brightness setting): `sceVideoOutAdjustColor` → `sotcgpu_set_gamma`
    → the presenter's post-process pass (session 41).
  - It mirrors guest page protections and routes page faults to the GPU's tracker.
  - It also handles submits, EOP/flip events and map notifications.
- `gpu/`: sotcgpu, the C interface over shadPS4's video_core.
  - Interface: `gpu/include/sotcgpu.h`. Implementation: `gpu/src/sotcgpu.cpp` (presenter, flips, screenshots, settings).
  - Two-stage pipe: `renderer_vulkan/sotc_pipe.cpp` plus `sotc_rasterizer_internal.h`.
    CPU copy: `sotc_cpu_copy.cpp`. These were split out of `vk_rasterizer.cpp` in session 60.
  - `gpu/shim/` replaces shadPS4's `core/`.
  - Local patches to `third_party/shadps4` **must** be listed in `third_party/shadps4/README.md`.
- `runtime/src/hle/`, one file per area:
  - `kernel.c` (misc, time), `memory.c` (dmem/fmem arena at `0x1000000000`), `thread.c` (pthreads, sync, guest main thread), `file.c` (fds and the mount table), `sysmodule.c`
  - M2: `evflag.c`, `equeue.c`, `kernel_misc.c`, `fiber.c`, services (`userservice.c`, `systemservice.c`, `np.c`, `misc_services.c`, `pad.c`), `dialogs.c`, `savedata.c`, `net.c`, `http.c`, `audio.c` (+ `../audio_host.c`, SDL3 output), `ngs2.c` (null; replaced by your own firmware module when `SOTC_FW_DIR` is set at conversion), `ajm.c` (ATRAC9 through LibAtrac9, `third_party/LibAtrac9`, MIT), `gnmdriver.c`, `pm4.c` (null GPU), `videoout.c`
  - shared: `hle.h` (macros, error codes), `kobj.h` (integer handles), `equeue.h` (event-queue API)

## HLE conventions
- Each PS4 library lives in its own file and registers itself:
  `HLE_LIBRARY(var, "libSceX", HLE(fn1), HLE(fn2), HLE_AS("name", impl),)`.
  New files are globbed, so there's no need to edit `hle.c` or `CMakeLists.txt`.
- Implementations are `static SYSV <ret> ps4_<SonyName>(...)`. `SYSV` is required: the guest uses the SysV ABI.
- `sce*` functions return `SCE_OK` or `SCE_KERNEL_ERROR(PS4_E*)`; posix functions return -1 and set `*hle_errno()`, or return an errno value (pthreads).
- Handles:
  - Pointer-sized PS4 handles (mutex, cond, event flag, equeue) are host pointers stored in the guest's variable. Lazily create the object when the value is NULL (static initializers).
  - Integer handles use `kobj_alloc/get/free`.
- Intentional no-ops use `HLE_STUB_OK(name)`. It logs `stub name`; unresolved imports log `unimplemented`.
- Semantics: read `../shadps4/src/core/libraries/<lib>/`. Exact import names are in `build/game/boot.txt`.
- Host code runs on guest thread stacks, so avoid huge stack frames. Never call guest code while holding a runtime lock.
- Tests: `runtime/tests/test_<area>.c` with its own `main()`. Reach static `ps4_*` functions with `hle_lookup()`.

## Known limits
- Memory: the game process plateaus at ~4.7-5.0 GB private in gameplay (5.25 GB of PS4 direct
  memory is reserved, resident once touched, plus heap and driver buffers), peak ~5.2-5.7 GB at the
  gameplay load (working set peak ~5.9-6.2 GB). A Tracy capture needs several GB more (client
  queue + `tracy-capture` keeps the trace in RAM): capture 10 s, close other apps. Claude Code
  kills background jobs when memory runs critically low.
- Developed on Windows 10 (Ryzen 5 3600, SSE4a runs natively; RTX 2060: the AjncSystem wave32
  shader fixes matter on NVIDIA) and Linux (Ryzen 5 5600, RX 9070 XT, RADV/Mesa, subgroup size 64).
- PS4 Pro mode (`SOTC_NEO=1`, session 41): title and intro render; gameplay and its 60 fps
  Performance mode untested. A JIT crash report (fault in the SRT walker's generated code) now prints
  the code bytes, registers and host return addresses (`main.c`).
- Linux: `test_sotcgpu_headless` is not run there (an amdgpu reset can take the desktop down).
  POSIX reservations are `PROT_NONE` mappings of an empty memfd (`sotc-reserved`) so
  `plat_query_prot` reports them unmapped like Windows' `MEM_RESERVE`. Guest paths are matched
  case-insensitively (`fix_case` in `file.c`; the game opens `pakn.psarc` for `pakN.psarc`).
  Without `SOTC_FW_DIR` the game runs silent.
- AMD runs shaders wave64, so the recompiler's 32-wide LDS lowering (`LowerWave64BallotPass`) is
  skipped there, but the session-11 rule for a `ReadLane` past a workgroup of <= 32 threads (reads
  the reduction identity) runs on both (session 28; a 33..63 thread workgroup only logs
  `ReadLane(s) past the N-thread workgroup left as is`). Before it, Wander and Agro flickered
  through many colors every frame on AMD.
- Direct memory is aliased through a pagefile-backed section and arena placeholders (Windows 10 1803+). Views need 64 KiB alignment; an unaligned direct map falls back to an unaliased copy (warns once).
- Splitting a mapping briefly unmaps it: the fault handler retries such accesses, and host I/O into guest memory must go through a bounce buffer (see `file.c`).
- The null GPU (`pm4.c`) runs submissions asynchronously on one thread per queue; GnmDriver uses `pm4_submit`.
- SSE4a on Windows with an Intel CPU isn't supported yet; it needs pre-patched trampolines.

## Personal context
`CLAUDE.local.md` (gitignored) holds per-developer context: dump paths, machines, workflow, the
current plan. It is not part of the repo.
