# Milestone 3: the title screen renders

**Goal:** replace the null GPU with shadPS4's `video_core` and `shader_recompiler` (Vulkan), behind a C
interface (`sotcgpu`), presenting into an SDL3 window. Two more tracks run alongside: frame pacing, and
stack and fault safety. M3's page-protection faults make the second track urgent.

**Starting point** (M2, commit `ee94eba`):
- The game reaches `Starting main loop` on the null GPU.
- `smoke.py --seconds 120` reports `still running` with no errors and no unimplemented imports.
- Only 11–26 flips/s, against the game's 30.
- Results and known gaps are in `docs/RECON.md`, "Milestone 2 result".

**Exit criteria**
- `SOTC_GPU=vulkan python scripts/smoke.py --seconds 120 --shots 10` reports `still running`, with frames rising
  and no errors.
- The saved screenshots in `build/game/shots/` show the intro and the title screen. Check them by opening the
  PNGs.
- `SOTC_GPU=null` still meets every M2 exit criterion, at **≥ 29 flips/s**.
- `ctest --test-dir build` and both Python suites pass. GPU tests skip cleanly on a machine without Vulkan.
- The red-zone question (workstream S) is answered, with a fix or a measured decision.

Decisions already made by the user:
- **Dependencies:** vendored shadPS4 sources plus git submodules. No vcpkg, and nothing built out of `../shadps4`.
- **Tracks:** pacing (P) and safety (S) run alongside the GPU work. Ngs2 LLE and Linux wait.
- **Goal:** the title screen, not in-game control. Real pad input comes with the window anyway.

---

## Step 0: foundation (serial, main session, before any agent starts)

Same idea as M2's foundation: after this step, no two workstreams edit the same file. Commit it as
"M3 foundation".

1. **Split the platform layer by area.** This is a pure move with no behaviour change.
   - Windows: `runtime/src/platform_win.c` becomes:
     - `platform_win_mem.c`: memory, the arena and physical memory
     - `platform_win_thread.c`: TLS, threads, sync, time and sleep
     - `platform_win_fault.c`: VEH, faults and `plat_is_readable`
   - POSIX: the same split for `platform_posix.c`.
   - `platform.h` stays as it is.
2. **Vendor shadPS4.** Add `scripts/import_shadps4.py`, which copies these from `../shadps4` at commit `14ada48`
   into `third_party/shadps4/`:
   - `src/common`
   - `src/video_core`
   - `src/shader_recompiler`
   - `LICENSE`

   Write `third_party/shadps4/README.md` recording the commit and stating that local patches are allowed and
   must be listed there. M4's AjncSystem fixes will be patches on this copy.
3. **Add submodules** under `third_party/`, pinned to the exact commits `git -C ../shadps4 submodule status`
   reports:
   - `vulkan-headers`, `vma`, `glslang`, `sirit`
   - `robin-map`, `xxhash`, `fmt`, `magic_enum`, `half`
   - `ext-boost` (header subset), `sdl3`

   GL may add more later; it owns `.gitmodules` after step 0.
4. **Write the GPU contract.**
   - `gpu/include/sotcgpu.h`: exactly the text below.
   - `gpu/null/sotcgpu_null.c`: a no-op implementation. `sotcgpu_init` returns -1, so the runtime keeps the null
     PM4 executor.
   - CMake:
     - `option(SOTC_VULKAN "Build the Vulkan GPU (sotcgpu)" OFF)`
     - `add_subdirectory(gpu)`
     - the runtime links target `sotcgpu`, which is the null implementation while the option is OFF
     - SDL3 is built from the submodule and linked into the runtime
5. **Freeze `main.c`** behind two hooks, so the agents never need to edit it.
   - **Startup:** `main()` starts the guest main thread without blocking, then calls `host_main_loop()`. That
     function is declared in `runtime/src/window.h`. Its first version in `window.c` only joins the guest
     thread. `hle_run_main` gains a non-blocking variant; `thread.c` belongs to S, but this one change happens in
     step 0.
   - **Faults:** `on_fault` first calls `gpu_bridge_on_fault(addr, is_write)`, declared in
     `runtime/src/gpu_bridge.h`. Its first version in `gpu_bridge.c` returns false. `plat_fault` gains an
     `is_write` field.
6. Make sure `cmake --build build`, ctest and the M2 smoke test still pass, then commit.

### The `sotcgpu.h` contract (step 0 writes this text exactly)

```c
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* sotcgpu: shadPS4's video_core + shader_recompiler (Vulkan) behind a C
 * interface. The runtime (C) calls in; the library calls back through
 * sotcgpu_host. Guest virtual addresses are GPU addresses (below 1 TiB).
 * Owned by workstream GL (implementation) and GI (runtime side); the
 * signatures below change only at integration. */
#ifndef SOTCGPU_H
#define SOTCGPU_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*sotcgpu_done_fn)(void *arg);

typedef struct sotcgpu_host {
    /* Page tracking (shadPS4 PageManager): change guest page protection. */
    bool (*protect)(uint64_t addr, uint64_t size, bool allow_read, bool allow_write);
    /* Write guest memory regardless of its protection (Memory::TryWriteBacking). */
    bool (*write_backing)(uint64_t addr, const void *src, uint64_t size);
    /* Copy guest memory; unmapped holes read as zero (Memory::CopySparseMemory). */
    void (*read_sparse)(void *dst, uint64_t addr, uint64_t size);
    /* Bytes from addr that are mapped contiguously, at most size (ClampRangeSize). */
    uint64_t (*clamp_range)(uint64_t addr, uint64_t size);
    /* Whole range mapped and GPU-visible (Memory::IsValidGpuMapping). */
    bool (*is_gpu_mapping)(uint64_t addr, uint64_t size);
    /* An EOP / RELEASE_MEM interrupt (GnmDriver EOP equeue events). */
    void (*eop_interrupt)(void);
    /* A flip was presented: VideoOut status and flip events. */
    void (*flip_done)(int32_t buf_index, int64_t flip_arg);
    /* Log a line; level as in log.h (0 error .. 4 trace). */
    void (*log)(int level, const char *msg);
} sotcgpu_host;

typedef struct sotcgpu_config {
    void *sdl_window;      /* SDL_Window*, created by the runtime's main thread */
    uint32_t width, height; /* initial swapchain size */
    bool neo;              /* SOTC_NEO=1: PS4 Pro GPU */
    bool validation;       /* SOTC_VK_VALIDATION=1 */
    const char *cache_dir; /* pipeline/shader cache directory, or NULL */
} sotcgpu_config;

/* 0 on success; anything else means "no GPU": the runtime uses the null executor. */
int sotcgpu_init(const sotcgpu_host *host, const sotcgpu_config *cfg);
void sotcgpu_shutdown(void);

/* Submissions (Liverpool::SubmitGfx / SubmitAsc). Return at once; `done` (may
 * be NULL) runs once the command processor has consumed the buffers, e.g. to
 * advance a compute ring's read pointer. */
void sotcgpu_submit_gfx(const uint32_t *dcb, uint32_t dcb_dwords, const uint32_t *ccb,
                        uint32_t ccb_dwords, sotcgpu_done_fn done, void *arg);
void sotcgpu_submit_compute(uint32_t vqid, const uint32_t *acb, uint32_t dwords,
                            sotcgpu_done_fn done, void *arg);
void sotcgpu_submit_done(void);

/* Display buffers: `attribute` is the guest SceVideoOutBufferAttribute (40 bytes). */
void sotcgpu_register_buffers(int32_t group, const uint64_t *addrs, int32_t count,
                              const void *attribute);
/* Present buffer buf_index (-1: blank). after_gpu_work: ordered after all GPU
 * work submitted so far (SubmitAndFlip); else immediate (sceVideoOutSubmitFlip).
 * Completion is reported through host->flip_done. */
void sotcgpu_flip(int32_t buf_index, int64_t flip_arg, bool after_gpu_work);

/* Guest memory events (Rasterizer::MapMemory / UnmapMemory). */
void sotcgpu_map_memory(uint64_t addr, uint64_t size);
void sotcgpu_unmap_memory(uint64_t addr, uint64_t size);
/* From the fault handler: true if the GPU's page tracking caused this fault
 * and has handled it (the access should be retried). */
bool sotcgpu_handle_fault(uint64_t addr, bool is_write);

/* Debug: write the most recently presented frame as a PNG. */
bool sotcgpu_screenshot(const char *path);

#ifdef __cplusplus
}
#endif

#endif
```

---

## Workstreams (run in parallel after step 0)

Each runs as its own agent in its own worktree (`isolation: worktree`). Each one:
- creates or edits only the files listed as its own
- writes notes in `docs/m3/<ws>.md`
- doesn't commit

| WS | Scope | Owns | Size |
|---|---|---|---|
| **GL. GPU library** | Build `sotcgpu` (C++20, clang-cl) from `third_party/shadps4`. Replacement ("shim") headers under `gpu/shim/` stand in for everything `video_core` takes from shadPS4's `core/` (details below). `gpu/src/sotcgpu.cpp` implements the contract by wrapping `AmdGpu::Liverpool`, `Vulkan::Presenter` and `Vulkan::Rasterizer`. Turned on by `SOTC_VULKAN=ON`. | `gpu/` except `gpu/include/` and `gpu/null/`; build glue under `third_party/`; `.gitmodules`; `gpu/tests/` | XL |
| **GW. Window and input** | SDL3 on the process main thread: `host_main_loop()` creates the window (1920×1080 logical, resizable, `SOTC_WINDOW=WxH`), pumps events, and exits cleanly on close. Exposes `window_sdl_handle()` for GI. **Pad:** an SDL gamepad, or the keyboard as a fallback (default bindings in the notes), feeds `scePadRead`/`ReadState`. | `runtime/src/window.c`, `window.h`, `runtime/src/hle/pad.c`, `runtime/tests/test_window.c` (skips without a display) | M |
| **GI. Runtime GPU bridge** | The runtime side of the contract (details below). | `runtime/src/gpu_bridge.c`, `gpu_bridge.h`, `hle/pm4.c`, `pm4.h`, `hle/videoout.c`, `hle/gnmdriver.c`, `hle/memory.c`, `platform_*_mem.c`, `gpu/null/`, `runtime/tests/test_gpu_bridge.c` | L |
| **P. Frame pacing** | ≥ 29 flips/s on the null GPU with steady frame times (details below). | `platform_*_thread.c`, `hle/audio.c`, `scripts/smoke.py`, `runtime/tests/test_timing.c` | S–M |
| **S. Stack and fault safety** | Red zone, fault-handler stack and fiber stacks (details below). | `platform_*_fault.c`, `hle/fiber.c`, `hle/thread.c`, `sse4a.c`, `tools/sotcconv/codescan.py` (scan only), `runtime/tests/test_fault.c` | M, risky |

**How they depend on each other:**
- All five start together after step 0.
- GL and GI meet only at `sotcgpu.h`.
- GW gives GI the SDL window. GI should code against `window_sdl_handle()` declared in `window.h`, which step 0 creates with a stub.
- P and S don't depend on the GPU at all.

### GL in detail
- **Dependencies:** start by compiling `common`, then `shader_recompiler`, then `video_core`. Each shim replaces one shadPS4 piece:

  | shadPS4 piece | Replacement |
  |---|---|
  | `core/memory.h` | `Core::Memory` forwarding to `sotcgpu_host` (TryWriteBacking, CopySparseMemory, ClampRangeSize, IsValidGpuMapping, GetAddressSpace for protection) |
  | `core/signals.h` | fault routing comes in through `sotcgpu_handle_fault` instead |
  | `core/emulator_settings.h` | a settings struct with shadPS4's defaults, filled from `sotcgpu_config` |
  | `core/debug_state.h` | no-ops |
  | `core/libraries/videoout/*` | the buffer attribute types and a small driver that maps `sotcgpu_flip` onto `Presenter` |
  | `sdl_window.h` | a `Frontend::WindowSDL` wrapping the `SDL_Window*` from the config |
  | `imgui/*` | compiled out, or imgui built from its submodule, whichever is smaller |
  | `core/libraries/kernel/process.h`, `common/elf_info.h` | SDK version 0x4508101; NEO from the config |
  | `core/libraries/gnmdriver/gnmdriver.h` | the few constants it needs |

- **Source edits:** keep edits to `third_party/shadps4` to a minimum, and list every one in `third_party/shadps4/README.md`.
- **Interrupts:** Liverpool's EOP interrupt reaches `host->eop_interrupt`. Liverpool writes its labels itself.
- **Tests:**
  - `gpu/tests/test_recompiler.cpp`: recompile a small hand-assembled GCN vertex and pixel shader to SPIR-V.
  - `test_sotcgpu_headless.cpp`: create a Vulkan instance and device without a window, submit a DCB with WRITE_DATA and RELEASE_MEM, and check the label and the interrupt callback. It skips when there's no Vulkan device.
- **Risk:** this is the biggest piece and is nearly serial inside. If the agent runs long, it hands back what compiles, plus a list of the remaining link errors.

### GI in detail
- **Backend choice:** `pm4_submit` sends work to `sotcgpu_submit_*` when the backend is Vulkan (`SOTC_GPU=vulkan`, the default once GL lands), and to the null executor otherwise. Choose the backend once, at the first submit or in `gpu_bridge_init()`.
- **Memory:**
  - Map the whole physical section once more as a "backing" view for `write_backing`. Direct memory translates VA to phys through the region list; private memory gets a temporary unprotect.
  - Implement `read_sparse`, `clamp_range` and `is_gpu_mapping` from the region list.
  - Call `sotcgpu_map_memory`/`unmap_memory` on every GPU-visible map and unmap.
  - `protect` must preserve the protection the guest asked for. Store it per region and combine the two.
- **Faults:** `gpu_bridge_on_fault` asks `sotcgpu_handle_fault` before memory.c's split retry.
- **VideoOut:** `RegisterBuffers` calls `sotcgpu_register_buffers`, and `SubmitFlip` calls `sotcgpu_flip(..., false)`. `flip_done` drives flip status and events, replacing the timer-driven flip. Vblank stays a 60 Hz timer.
- **GnmDriver:** SubmitAndFlip queues `sotcgpu_flip(..., true)`.
- **Screenshots:** `SOTC_SHOTS=N` saves a screenshot every N seconds to `build/game/shots/NNNN.png`.
- **Test:** `test_gpu_bridge` uses a recording build of `gpu/null` to check the call sequence and the backing-write path.

### P in detail
- **Timing:**
  - `timeBeginPeriod(1)` at startup.
  - `plat_sleep_us` uses a high-resolution waitable timer (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`) for waits of 1 ms or more, and spins briefly below that.
  - A `plat_sleep_until_ns` for deadline loops.
- **Null GPU and VideoOut:** the null PM4 worker and VideoOut's vblank thread are GI's files. P files a precise request in its notes rather than editing them.
- **Audio:** AudioOut deadlines use the new sleep.
- **smoke.py:**
  - `--gpu {null,vulkan}` (sets `SOTC_GPU`)
  - `--shots N`
  - a flips-per-second report: mean and the 1% low over the run, taken from new `videoout: frame` timestamps
  - frame logging every 60 flips is already there
- **Measure:** record where the null run loses time, in the notes.

### S in detail
- **Red zone (most important):**
  - Windows writes the exception frame directly below `rsp`. The SysV ABI lets leaf functions keep up to 128 bytes there, so every fault taken in guest code can corrupt live data.
  - M2 takes few faults. M3's page manager write-protects GPU-watched pages and faults on every first write. That is AjncSystem fixes #01 and #05.
  - Steps:
    1. Prove it with a test: a SysV leaf function stores below `rsp`, then touches a write-protected page.
    2. Count with `codescan.py` how many eboot functions touch memory below `rsp`.
    3. Choose a fix: patching at conversion time, or another way to track writes, or something else. Measure it and write it up.
- **Fault-handler stack:** do heavy fault work on a per-thread emergency stack (AjncSystem #02), so faults on nearly full 16 KiB fiber stacks are reported instead of killing the process.
- **Fibers:** a guard or check for HLE code running on small fiber stacks (the M2 gap).
- **Threads:** check that the thread-exit path is robust.

---

## Integration (serial, main session)

1. Copy each worktree's files into main without committing, then build.
2. Run ctest, both Python suites, and the M2 smoke test with `--gpu null`; it must still pass.
3. Turn on `SOTC_VULKAN=ON` and run `smoke.py --gpu vulkan --seconds 60 --shots 5`.
4. **Bring-up loop:** fix the first blocker, look at the screenshots, and repeat. Expect:
   - shader recompiler asserts
   - the AjncSystem items from `docs/RESEARCH.md` §2.4 as they come up (#06–#18 and #29 are likely early)
   - page-tracking deadlocks (#12)
5. Fold `docs/m3/*.md` into `docs/RECON.md` and update `CLAUDE.md`. Commit after the user confirms.

## Later milestones (unchanged order)
- **M4:** the AjncSystem fixes, done in parallel by area:
  - memory and page tracking: #04, #12, #27, #28
  - Vulkan robustness: #06–#11, #29
  - recompiler wave64 and LDS: #14, #15, #17, #18, #24–#26
  - SotC shader patches: #16, #23
  - performance: #19–#22
- **M5:**
  - Ngs2 LLE (`D:\libSceNgs2.sprx`) plus an Ajm HLE using LibAtrac9, with real audio output
  - polish keyboard and mouse input
  - saves
- **M6:** resolution and FSR, Pro "Performance mode", a frame-rate study.

---

## Next-session prompt

> Read CLAUDE.md, docs/RECON.md ("Milestone 2 result") and docs/MILESTONE3.md.
>
> 1. Do **Step 0** of docs/MILESTONE3.md yourself:
>    - split the platform files
>    - write scripts/import_shadps4.py and vendor shadPS4 at 14ada48 into third_party/shadps4
>    - add the pinned submodules
>    - write gpu/include/sotcgpu.h exactly as given, the null sotcgpu and the CMake option
>    - add the main.c hooks (host_main_loop, gpu_bridge_on_fault)
>
>    Build, then run ctest, both Python suites and `python scripts/smoke.py --seconds 120` with
>    SOTC_GAME_DIR='D:\CUSA08809\uroot', to confirm nothing regressed. Then show me the diff summary and wait for
>    my OK to commit it as "M3 foundation".
> 2. After that commit, launch 5 agents in parallel (GL, GW, GI, P, S), each in its own git worktree
>    (isolation: worktree). Give each one the agent prompt template from docs/MILESTONE2.md with its workstream
>    filled in from docs/MILESTONE3.md, plus its file ownership list. Tell GL and GI that sotcgpu.h is frozen.
>    Tell GI to use window_sdl_handle() from window.h. Tell P and S that they never touch GPU files.
> 3. When all agents finish, do the M3 integration: merge without committing, build, run the tests, run the
>    smoke test with `--gpu null`, then turn on SOTC_VULKAN and start the bring-up loop with
>    `--gpu vulkan --shots 5`, looking at the screenshots. Repeat until the exit criteria hold or something needs
>    my decision.
> 4. Fold docs/m3/*.md into docs/RECON.md and update CLAUDE.md. Give me a short summary: what works, the
>    smoke output (frames, flips/s, screenshots) and open issues. Commit only after I confirm.
