# M3 workstream GI: runtime GPU bridge

## Files changed
- `runtime/src/gpu_bridge.c`, `gpu_bridge.h`: backend choice, the `sotcgpu_host` callbacks, the page-tracking table, the event thread and screenshots.
- `runtime/src/hle/memory.c`:
  - reports every map, unmap and mprotect to the bridge
  - sends map and unmap notifications to sotcgpu
  - answers the callback queries: `hle_memory_write_backing`, `read_sparse`, `clamp_range`, `is_gpu_mapping` and `for_each_mapping`
- `runtime/src/hle/pm4.c`, `pm4.h`:
  - `pm4_submit` forwards to sotcgpu while the GPU is active
  - `pm4_raise_eop_interrupt` is now public
  - the null queues wait on a condition variable instead of short sleeps
- `runtime/src/hle/videoout.c`:
  - in GPU mode: register buffers with sotcgpu, flip through sotcgpu, and complete flips through `videoout_flip_done`
  - null-GPU back-pressure for EOP flips (see Smoke)
  - `videoout: flip N at T us` lines
  - rate-limited "flip queue full" logging
- `runtime/src/hle/gnmdriver.c`:
  - backend init before taking its lock
  - SubmitAndFlip calls `sotcgpu_flip(..., true)` through VideoOut
  - SubmitDone calls `sotcgpu_submit_done`
  - the failed-flip log is now `log_debug`
- `runtime/src/platform_win_mem.c`, `platform_posix_mem.c`:
  - new `plat_phys_backing()` (a second view of the whole physical section) and `plat_query_prot()`
  - on Windows, `plat_protect` now splits ranges that span several allocations
- `gpu/null/sotcgpu_null.c` and the new `sotcgpu_null.h`: a recording build behind `SOTCGPU_NULL_RECORDING`. The normal build is unchanged, and its init returns -1.
- `runtime/tests/test_gpu_bridge.c`: the new test.
- `docs/m3/gi.patch`: shared-file change (see below).

## Backend selection and init
- `SOTC_GPU=vulkan|null`.
  - Unset: Vulkan when a game image is loaded (`g_rt.module_count > 0`), null otherwise. Unit tests that never set it, such as test_pm4 and test_gnm, therefore keep the null executor without touching the window.
  - Any other value logs a warning and uses null.
- `gpu_bridge_init()` runs once, lazily. It is called from `sceVideoOutOpen` (before the port lock) and from the GnmDriver submit, SubmitAndFlip and DingDong entries (before the gnm lock). main.c is untouched.
  - **Vulkan:** `window_sdl_handle()` (which may block until GW's main thread has created the window), then `plat_phys_backing()`, then `sotcgpu_init(&host, {window, 1920, 1080, SOTC_NEO, SOTC_VK_VALIDATION, "$SOTC_USER_DIR/gpu_cache"})`.
    - Nonzero → log it and use the null PM4 executor. The null library always returns nonzero, so today's build behaves as in M2.
    - 0 → start the event thread, mark active, replay existing mappings (memory.c regions plus the loaded image) with `sotcgpu_map_memory`, and start the screenshot thread if `SOTC_SHOTS=N`.
  - `gpu_bridge_active()` is a plain atomic load and never initializes.
- `gpu_bridge_set_window_provider()` is a test hook that replaces `window_sdl_handle`.

## Memory and page tracking
- **Page table:** one byte per 4 KiB page, the PageManager granularity. It holds the guest's protection (host `PLAT_PROT_*`), a "known" bit, and the GPU's deny-read and deny-write bits. Chunks are 64 Ki pages, allocated lazily and never freed, so the fault path reads them without a lock.
- **Host protection:** always guest ∩ GPU. Deny-read means no access, because x86 has no write-only pages.
  - `protect(addr, size, r, w)` (the host callback) sets the deny bits. It only calls `plat_protect` for pages whose effective protection changed, coalesced into runs.
  - `sceKernelMprotect` goes through `gpu_bridge_guest_protect`, so a guest mprotect keeps the GPU's restrictions.
  - Splits re-apply them on the remapped pieces (`gpu_bridge_guest_remap`).
  - Fresh maps reset the pages; unmaps clear them.
- **Unknown pages:** pages mapped before the GPU started, or outside memory.c (the eboot image), are filled in from the host's current protection on first use (`plat_query_prot`).
- **Inactive GPU:** the table isn't maintained and all hooks reduce to the old `plat_protect` calls, so M2 behaviour is unchanged.
- **write_backing** (shadPS4 TryWriteBacking):
  - 64 KiB-aligned direct maps (views): copies through the backing view at the physical offset.
  - Private memory, unaligned direct maps and the image: `gpu_bridge_force_copy` lifts the protection to RW under the tracking lock, copies, and restores guest ∩ GPU.
  - Returns false if part of the range is unmapped.
- **read_sparse:** the same paths, and holes read as zero.
- **clamp_range:** the exact size of the contiguous mapped span: regions plus the image, with reserved ranges counting as holes. Unlike shadPS4, it doesn't skip the check for sizes under 1 GB; region lookup is a binary search.
- **is_gpu_mapping:** below 1 TiB and fully mapped.
- **Map notifications:**
  - `sotcgpu_map_memory` after every successful non-reserved map, outside `g_lock`.
  - `sotcgpu_unmap_memory` before munmap, and before a MAP_FIXED map over its range, also outside `g_lock`.
- **Faults:** `gpu_bridge_on_fault` (called first by main.c):
  - The page carries a GPU deny bit that matches the access → `sotcgpu_handle_fault(addr, is_write)`.
  - Otherwise, if the guest protection allows the access → retry. This covers the GPU lifting a restriction just after the fault; at most 8 retries for the same address.
  - Otherwise → false, and the old handlers run.
  - If sotcgpu reports a fault as handled more than 10,000 times in a row for the same address, the bridge gives up and logs it, so the process doesn't livelock.

## Threading and locks
- **Locks:** memory.c's `g_lock` comes first, then the bridge's tracking lock `g_track`.
  - sotcgpu's callbacks take only `g_lock` (read_sparse, write_backing, clamp_range, is_gpu_mapping) and/or `g_track` (protect).
  - No code that holds either lock calls into sotcgpu or touches GPU-protected memory without first lifting the protection under `g_track`. `unmap_range` holds `g_track` for the whole split, so the GPU can't re-protect pages while they are copied.
  - So sotcgpu can call back while holding its own page-manager locks without deadlocking (AjncSystem #12).
- **Callbacks that need other runtime locks are deferred:** `eop_interrupt`, `flip_done` and every submission's `done` only queue an event (malloc plus the event-list lock). The thread `sotc:gpu-events` then runs them in order:
  - `pm4_raise_eop_interrupt`, which triggers the equeues
  - `videoout_flip_done`, which takes the port lock and triggers the equeues
  - the GnmDriver done function, for example advancing a compute ring's read pointer, which can write guest memory
  - Deferring them means sotcgpu's threads never wait on equeue, VideoOut or Gnm locks, which a guest thread could hold while taking a page fault into sotcgpu.
- **Which thread calls what into sotcgpu:**

  | Call | Thread | Locks held |
  |---|---|---|
  | `sotcgpu_init` | the first guest thread calling `sceVideoOutOpen` or a Gnm submit | the bridge init lock only |
  | `sotcgpu_submit_gfx` / `_compute` | guest threads, via `pm4_submit` | GnmDriver's gnm lock |
  | `sotcgpu_flip(i, arg, false)` | guest thread, in `sceVideoOutSubmitFlip` | VideoOut's `g_flip_order` (keeps the pending queue and sotcgpu in the same order; never the port lock) |
  | `sotcgpu_flip(i, arg, true)` | guest thread, in SubmitAndFlip | gnm lock and `g_flip_order` |
  | `sotcgpu_register_buffers` | guest thread | none |
  | `sotcgpu_submit_done` | guest thread | none |
  | `sotcgpu_map_memory` / `_unmap_memory` | guest threads | none (outside `g_lock`) |
  | `sotcgpu_handle_fault` | the faulting thread, inside the vectored exception handler | none of ours, normally |
  | `sotcgpu_screenshot` | `sotc:gpu-shots` | none |

## VideoOut and GnmDriver
- **GPU mode:**
  - `RegisterBuffers` → `sotcgpu_register_buffers(group, table, start+num, attr)`. `table` is indexed by buffer index, with 0 in the slots before `start`.
  - `SubmitChangeBufferAttribute` registers the group again (there is no "change" call in the contract).
  - `SubmitFlip` and SubmitAndFlip queue the request (for order and the EOP flag) and call `sotcgpu_flip`. `flip_done` pops it, updates the flip status and fires the flip events.
  - SubmitAndFlip returns `0x80D11081` to the game when the flip queue is full, as shadPS4 does.
  - The vblank thread stays a 60 Hz timer for vblank events and doesn't pop flips.
- **Null mode:** as M2, with one fix. An EOP flip that finds the flip queue full blocks the graphics queue's worker on the vblank condition variable until a slot frees, like the GPU's flip queue, instead of failing silently. That gives the game back-pressure (see Smoke).
- **Logging:**
  - Every consumed flip logs `videoout: flip <count> at <monotonic us> us` (P's request), plus `videoout: frame N` every 60.
  - "flip queue full" is a rate-limited warning: the first one, then at most one line per second with a count.

## Screenshots
`SOTC_SHOTS=N` (GPU active only) starts a thread that calls `sotcgpu_screenshot("build/game/shots/NNNN.png")` every N seconds, numbered from 0000. The path is relative to the current directory, and the directories are created.

## Pacing changes (for P)
- **PM4 waits:** `WAIT_REG_MEM` and `MEM_SEMAPHORE` waits in the null queue workers spin 64 yields, then block on a condition variable with a 1 ms timeout instead of `plat_sleep_us(50)`. The variable is broadcast by every PM4 memory write, semaphore signal, job completion and `pm4_submit`, and only when someone is waiting (an atomic counter). This was P's request 3.
- **Flip back-pressure:** the null-mode EOP flip now blocks while the flip queue is full, as described under VideoOut and GnmDriver.
- **Vblank thread:** unchanged. It sleeps to absolute deadlines and benefits from P's `plat_sleep_us`.

## Tests
- `runtime/tests/test_gpu_bridge.c`:
  - **How it uses the recording build:** it does `#define SOTCGPU_NULL_RECORDING` and `#include "../../gpu/null/sotcgpu_null.c"`. Its own object then defines every `sotcgpu_*` symbol, so the linked `sotcgpu` library (null, or GL's Vulkan library) isn't pulled for them. No CMake change is needed.
    - **Check at integration:** this relies on GL's library being a normal static library, not linked with /WHOLEARCHIVE.
  - **Init:** checks init with a fake window and 1920×1080, and the replay of a mapping made before init.
  - **Map notifications:** map and unmap are reported.
  - **Protection:** write-protect, then a backing write through the view; a CPU write faults into `handle_fault`, which unprotects. Read-protect, then a sparse read; a guest mprotect combines with the GPU's restriction, and the GPU lifting it leaves the guest's read-only.
  - **Private memory:** a backing write across two no-access pages, with protection restored afterwards.
  - **Ranges:** holes read as zero and `write_backing` fails on them; `clamp_range` and `is_gpu_mapping`.
  - **Splits:** a munmap split keeps the GPU's restriction on the surviving piece.
  - **VideoOut:** register buffers, `SubmitFlip`, then flip status after `flip_done`.
  - **Queues:** `pm4_submit` on gfx and compute, with `done` delivered on the event thread.
  - **GnmDriver:** SubmitAndFlip records the submit, then `flip(0, 99, true)`, and the status ends with `gc_queue_num` back at 0. SubmitDone is forwarded.
  - **Interrupts:** `eop_interrupt` raises a PM4 interrupt.
- `ctest --test-dir build`: 12/12 pass.

## Smoke
`python scripts/smoke.py --seconds 120` with the default backend: no window in this worktree, so it falls back to null.

| Run | Result | Frames | Unimplemented | Errors |
|---|---|---|---|---|
| 1 | still running | 4080 | 0 | 0 |
| 2 | still running | 2940; flips stopped at 99 s (see open issues) | 0 | 0 |
| 3 | still running | 3540 | 0 | 0 |
| 4 | still running | 4080 | 0 | 0 |

- The M2 baseline was 1140.
- Steady state is 30 flips/s (60 frames every 2.0 s) even without P's timer changes.
- **What went wrong first:** the condition-variable PM4 waits made the null GPU fast enough that the game outran the 60 Hz flip consumer. That produced ~700 "flip queue full" errors/s, because M2 performed EOP flips on the worker and only logged failures, so the game was never throttled. M2 was throttled only by its slow polling.
- **The fix:** the blocking EOP flip. With it, the queue never fills in null mode.

## Shared-file patches
- `docs/m3/gi.patch`: declares `plat_phys_backing` and `plat_query_prot` in `platform.h`. They are declared in `gpu_bridge.h` until then; delete them there when applying.
- **No patch, a request to GW:** `window_sdl_handle()` must return NULL at once when `host_main_loop` has never run, as in unit tests or tools.
  - The bridge only calls it in Vulkan mode, which tests reach only with `SOTC_GPU=vulkan`.
  - test_gpu_bridge uses its own window provider.

## Contract gaps (sotcgpu.h; for GL and integration)
1. **No start index in `sotcgpu_register_buffers`.** The bridge passes a table indexed by buffer index (length start+num, with 0 before `start`). GL should treat array position as `buf_index` and skip 0 entries. Re-registering a group replaces it, which is how ChangeBufferAttribute is done. SotC registers buffers 0..1 with start 0, so this is moot for now.
2. **No `width`/`height` change and no shutdown path.** `sotcgpu_shutdown` is never called; the process exits.
3. **NULL window:** `sotcgpu_init` gets a NULL `sdl_window` if GW has no window. GL should return nonzero (→ null executor) unless it supports headless.
4. **`submit_compute` with 0 dwords:** it can be called with `acb == NULL, dwords == 0` (an empty doorbell range). It should just run `done`.
5. **Flip rate:** `flip_rate` (sceVideoOutSetFlipRate) isn't passed to sotcgpu. In GPU mode, pacing is the presenter's job.

## Open issues
- **Stalled run:** in 1 of 4 smoke runs, all logging stopped at 99 s (flips at 2990), and the process was still alive at 120 s.
  - Not reproduced in the other three. No warnings were logged before it, and a stuck PM4 wait would have logged "queue still waiting" every 10 s.
  - **Unknown:** whether it existed before (M2 at 9.5 flips/s may not reach that point) or comes from the new pacing.
  - **Next step:** catch it with `scripts/crashcatch.py` or a debugger attach. Suspects: a game-side wait on something the null GPU doesn't produce at 30 fps, or a lock cycle involving the log.
- **Writes during a lifted protection:** `force_copy` on private memory briefly lifts protection, and a guest write to those pages in that window isn't seen by the GPU's tracking. Direct memory, the usual GPU case, uses the backing view and has no such window.
- **Linux untested:** the POSIX `plat_query_prot` parses /proc/self/maps.
- **The Vulkan path itself is untested** against GL's library. Only the recording null library has exercised it.
