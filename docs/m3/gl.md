# M3 workstream GL: the GPU library (`sotcgpu`)

## Status

`-DSOTC_VULKAN=ON` builds `sotcgpu` from the vendored shadPS4 `common`, `shader_recompiler` and `video_core`
(14ada48). It compiles and links with clang-cl, and every function in `gpu/include/sotcgpu.h` is
implemented on `AmdGpu::Liverpool`, `Vulkan::Presenter` and `Vulkan::Rasterizer`. No rendering path is
stubbed out. Only devtools, ImGui, Tracy and RenderDoc UI are compiled out (details below).

| Config | Builds | ctest |
|---|---|---|
| `build` (default, `SOTC_VULKAN=OFF`) | yes, unchanged: the null `sotcgpu` from `gpu/null` | 11/11 pass |
| `build-vk` (`-DSOTC_VULKAN=ON`) | yes: runtime, all runtime tests, `sotcgpu`, 3 GPU tests | 14/14 pass (RTX 2060) |

GPU tests. They are registered only when `SOTC_VULKAN=ON`. Exit code 77 means skipped (`SKIP_RETURN_CODE`).

| Test | What it checks |
|---|---|
| `test_recompiler` | Recompiles a hand-assembled GCN vertex shader (`v_mov`, `exp pos0`) and pixel shader (`exp mrt0`) to SPIR-V with `TranslateProgram` + `EmitSPIRV`, as `PipelineCache::CompileModule` does. Then it checks the module and its entry point. No GPU needed. |
| `test_sotcgpu_headless` | `sotcgpu_init` without a window. It submits a DCB with WRITE_DATA plus EVENT_WRITE_EOP (data + IRQ) and checks both labels, the `eop_interrupt` callback and the `done` callback. It submits an ACB on compute queue 1 with RELEASE_MEM and checks the label and `done`. It also checks an immediate flip reaching `flip_done`, fault claiming, and shutdown. It skips without a Vulkan loader or device; I checked this with `VK_LOADER_DRIVERS_SELECT=nonexistent_driver*`. |
| `test_sotcgpu_window` | (Windows) A real SDL window. It registers a linear 1280x720 buffer of a known colour, flips it 3 times, then calls `sotcgpu_screenshot` and checks the PNG's centre pixel (it was exact: `20 c0 40`). It skips without a display or Vulkan. |

**Note on RELEASE_MEM:** shadPS4's Liverpool handles RELEASE_MEM only on compute queues. The graphics ring's
end-of-pipe packet is EVENT_WRITE_EOP, so the headless test uses both.

## Build layout (`gpu/CMakeLists.txt`)
- `SOTC_VULKAN=OFF`: exactly the old two lines (`gpu/null/sotcgpu_null.c`), then `return()`. In
  `third_party/CMakeLists.txt` everything after SDL3 is also skipped.
- `SOTC_VULKAN=ON`:
  - The host shaders are built by shadPS4's own `video_core/host_shaders/CMakeLists.txt`, using
    `glslang-standalone` from the glslang submodule. The Vulkan SDK is not needed.
  - Object libraries are built in this order: `sotcgpu_common`, then `sotcgpu_recompiler`, then
    `sotcgpu_video_core`.
  - They are packed into the static `sotcgpu` together with `gpu/src/sotcgpu.cpp` and `gpu/src/shim_core.cpp`.
  - Shared settings live in the `sotcgpu_options` INTERFACE target:
    - C++23, as shadPS4. I did not try C++20; shadPS4 targets 23.
    - `-march=x86-64-v3`, as shadPS4 (AVX2/BMI2). These flags apply only to sotcgpu objects, not the runtime.
    - `SOTC_NO_IMGUI`.
    - The include order is `gpu/shim` first, then `third_party/shadps4/src`.
- Vulkan is loaded at run time:
  - shadPS4's `vk::detail::DynamicLoader` loads it for the renderer.
  - `ProbeVulkan()` loads `vulkan-1.dll` for the init check.
  - Nothing links `vulkan-1.lib`.
- Link dependencies are `fmt`, `sirit`, `xxhash`, `Zydis`/`Zycore`, `miniz`, SDL3, `ntdll` and `mincore`.
  `mincore` provides `CreateFileMapping2`, used by `common/io_file`.
- **Submodules added** (pinned to the commits `../shadps4` uses; staged in `.gitmodules` and the index):
  - `third_party/xbyak` `44a72f3`: the recompiler JITs the SRT walker.
  - `third_party/zydis` `120e0e7` (with `zycore`): `common/decoder`, the SRT walker fault fix-up.
  - `third_party/miniz` `174573d`: the archived pipeline cache in `video_core/cache_storage`.
- **Single headers copied** from shadPS4's `externals/`:
  - `third_party/stb/` (`stb_image.h`, `stb_image_write.h`; public domain)
  - `third_party/renderdoc/renderdoc_app.h` (MIT)

## Patches to `third_party/shadps4` (also listed in its README)
1. **`video_core/amdgpu/liverpool.{h,cpp}`:**
   - `SubmitGfx`/`SubmitAsc` take an optional `Common::UniqueFunction<void> on_done`.
   - It is stored next to the task. `Process()` runs it on the GPU thread when that task finishes.
   - This implements the contract's `done` callback; Liverpool had no completion hook.
2. **`video_core/renderer_vulkan/vk_presenter.cpp`:**
   - Under `#ifdef SOTC_NO_IMGUI`, `Present()` blits the frame into the swapchain image (fitted, black bars).
   - Without that define it draws an ImGui image as before. ImGui is compiled out.

## Shim decisions (`gpu/shim/`, implementations in `gpu/src/shim_core.cpp`)

| shadPS4 piece | Replacement |
|---|---|
| `core/memory.h` (+ `address_space.h` bits) | `Core::MemoryManager` forwards to `sotcgpu_host`:<br>- `TryWriteBacking` → `write_backing`<br>- `CopySparseMemory` → `read_sparse`<br>- `ClampRangeSize` → `clamp_range`<br>- `IsValidGpuMapping` → `is_gpu_mapping`<br>- `AddressSpace::Protect` → `protect(read, write)`<br>`GetUsableRegions` (Linux uffd only) returns empty. |
| `core/signals.h` | A `SignalDispatch` with the same API that never installs an OS handler. `sotcgpu_handle_fault` builds an `EXCEPTION_POINTERS` (`ExceptionInformation[0]` = is_write, Rip = 0) and dispatches to the registered handlers: the page manager first, then the SRT walker. |
| `core/emulator_settings.h` | The `SotcGpuSettings` struct with shadPS4's defaults: Mailbox present mode, 60 Hz vblank, readbacks off, pipeline cache off, FSR off, RCAS on. `validation` comes from the config. |
| `core/debug_state.h`, `core/devtools/*` | No-ops. |
| `core/libraries/videoout/buffer.h` | A verbatim copy (`BufferAttribute` is the guest's 40-byte struct). |
| `core/libraries/videoout/driver.h` | `VideoOutPort` with only `IsVoLabel`/`WaitVoLabel`/`SignalVoLabel`. The labels live in the runtime, so `IsVoLabel` is always false and VO-label WAIT_REG_MEMs take the generic polling path. The flip driver itself is in `sotcgpu.cpp`. |
| `sdl_window.h` | A `Frontend::WindowSDL` that wraps the config's `SDL_Window*`. It takes the HWND from SDL properties and the size from `SDL_GetWindowSizeInPixels`. A null window means headless. |
| `imgui.h`, `imgui/*` | Compiled out: no-op stand-ins in `imgui/sotc_imgui_stub.h`, plus the presenter patch above. |
| `tracy/*` | Compiled out: the same empty macros Tracy defines without `TRACY_ENABLE`. |
| `absl/container/flat_hash_{map,set}.h` | Aliases to `std::unordered_{map,set}`. The texture cache is the only user; Abseil isn't vendored. |
| `core/libraries/kernel/process.h`, `common/elf_info.h` | `sceKernelIsNeoMode` comes from `sotcgpu_config::neo`. `ElfInfo` is filled with SDK/firmware `0x4508101`, serial `CUSA08809` (`Core::Emulator::FillElfInfo`). |
| `core/libraries/kernel/time.h` | Raw `__rdtsc` (the runtime's `sceKernelReadTsc` uses the same) and `Common::EstimateRDTSCFrequency`. |
| `core/libraries/gnmdriver/gnmdriver.h` | `sceGnmGetGpuCoreClockFrequency`: 911 MHz on NEO, 800 MHz otherwise, as shadPS4. |
| `core/platform.h` | shadPS4's `IrqController`, ported. The only change is that the context map is locked. |
| `core/libraries/kernel/threads/pthread.h`, `fiber.h`, `system/systemservice.h` | The minimal types `common/thread.cpp` and the presenter name. The splash is never visible. |
| `common/logging/log.cpp` | Not compiled. `gpu/src/log.cpp` sends every `LOG_*` line to `host->log`, prefixed `gpu: [Class]`, with file:line for warnings and errors. The default filter is Info; `SOTC_GPU_LOG=0..4` changes it. |
| `common/path_util.cpp` | Not compiled. shadPS4's version creates a whole user tree at static-init time. `gpu/src/path_util.cpp` roots every `PathType` at `cache_dir` (default `./sotcgpu`) and creates a directory only when it is first asked for. |

## What GI needs to know (calling the library)

**Init and shutdown**
- `sotcgpu_init(host, cfg)`:
  - Call it once, after the SDL window exists. Any thread works; the presenter does not need the window's
    thread.
  - It returns -1 with no Vulkan loader or device, or when initialization throws. The runtime should then
    use the null executor.
  - It copies `*host`.
  - `cfg->sdl_window == NULL` gives a headless GPU: no swapchain, flips still complete, and screenshots
    return false.
- `sotcgpu_shutdown()`:
  - Stops the present thread.
  - If the GPU still has work pending (the command processor may be stuck in a WAIT_REG_MEM), it leaks the
    GPU state instead of joining. Call it only at exit.

**Threads that call back into the runtime**

| Thread | Calls |
|---|---|
| Liverpool's command processor (`shadPS4:GpuCommandProcessor`) | `eop_interrupt`, every `done` callback, `write_backing`, `read_sparse`, `clamp_range`, `is_gpu_mapping`, `protect` |
| Present thread (`sotcgpu:Present`) | `flip_done` |
| Guest threads (inside `sotcgpu_map_memory` and fault handling) | `protect`, through the page manager |

- `eop_interrupt` runs with the IRQ controller's GfxEop lock held. It must not call `sotcgpu_shutdown`.
  Submitting or flipping from it is fine.
- None of these threads is a guest thread; the runtime must not assume a guest TLS/TCB there.
- The command processor reads command buffers, shaders and resources straight from guest addresses. A
  split-mapping fault on that thread must be retried by the runtime's handler, as for guest threads.

**Submissions**
- `sotcgpu_submit_gfx` and `sotcgpu_submit_compute` map directly to `Liverpool::SubmitGfx` and
  `Liverpool::SubmitAsc`.
- Buffers are read in place: shadPS4's `copy_gpu_buffers` is off. Keep a DCB alive and unchanged until its
  `done` callback runs.
- `vqid` is GnmDriver's (1..56). The library keeps a scratch ring read pointer per queue; the runtime advances
  the real one from `done`.
- The runtime must split ring wraps the way shadPS4's `sceGnmDingDong` does. Liverpool buffers a packet cut
  at the ring end until the next submission.
- `sotcgpu_submit_done` maps to `SubmitDone`.
- The GnmDriver init sequences (InitSequence350…) stay the runtime's job. `gnmdriver.c` already has them.

**Flips**
- `sotcgpu_flip(i, arg, false)`: sent to the GPU thread (Liverpool `SendCommand`), as shadPS4 does for
  non-EOP flips.
- `sotcgpu_flip(i, arg, true)`: registers a one-shot GfxFlip IRQ. It fires when Liverpool reaches the
  `PatchedFlip` NOP that `gnmdriver.c`'s `patch_flip_request` writes.
  - Call it **before** `sotcgpu_submit_gfx` for that DCB, in the same order as the DCBs, as
    `sceVideoOutSubmitEopFlip` then submit does.
  - Without the patched NOP the flip never happens.
- On the GPU thread, `PrepareFrame` reads the display buffer through the texture cache. The present thread
  then presents **one request per 60 Hz vblank**, as shadPS4's `PresentThread`, and calls
  `flip_done(index, arg)`.
- `index -1` presents a blank frame.

**Display buffers**
- `sotcgpu_register_buffers(group, addrs, count, attr)`: the contract has no start index, so
  **`addrs[i]` is the address of buffer slot `i`** for `i < count`, and **0 means "slot not part of this
  call"**.
- For `sceVideoOutRegisterBuffers(start, addrs, num)`, pass a 16-entry array with zeros outside
  `[start, start+num)` and `count = start + num`.
- `group` is the attribute group index (0..3).

**Memory**
- Call `sotcgpu_map_memory` and `sotcgpu_unmap_memory` for every GPU-visible mapping. They map to
  `Rasterizer::MapMemory` and `UnmapMemory`.
- `protect(addr, size, r, w)` gets page-aligned 4 KiB ranges. It must combine the request with the guest's
  own protection.

**Faults**
- `sotcgpu_handle_fault(addr, is_write)` follows shadPS4's semantics: it returns true for **any** fault inside
  GPU-mapped memory. It invalidates or reads back the caches and expects a retry.
- So only ask it when the GPU's `protect` is what denies the access. Otherwise a genuine guest fault in GPU
  memory would retry forever.
- Call it synchronously on the faulting thread. Liverpool uses the thread id to tell its own faults apart.
- Its work, cache invalidation, can be deep. It needs the emergency fault stack from workstream S on 16 KiB
  fiber stacks.

**Screenshots**
- `sotcgpu_screenshot(path)` blocks (at most about 2 s) until the present thread captures the next presented
  frame. If nothing new arrives, it re-presents the last frame.
- The PNG is written at the presenter's frame size (1920x1080 by default), before scaling to the window. It
  returns false when nothing has ever been presented.

**Asserts**
- A shadPS4 `ASSERT`/`UNREACHABLE` logs `Assertion Failed!` through `host->log` (level 0), then executes
  `int3`.
- The runtime's handler sees `EXCEPTION_BREAKPOINT` in sotcgpu code.

## Contract gaps (proposed changes for integration; `sotcgpu.h` is untouched)
1. **Flip rate:** `sceVideoOutSetFlipRate` has no way in. Presentation runs at one flip per vblank (60 Hz).
   Proposal: `void sotcgpu_set_flip_rate(int32_t rate)`.
2. **Compute interrupts:**
   - `eop_interrupt(void)` carries no IRQ id, so RELEASE_MEM interrupts on compute pipes
     (`Compute0..6RelMem`, GnmDriver equeue ids 0..6) are not forwarded.
   - Only GfxEop is forwarded.
   - Proposal: `void (*eop_interrupt)(uint32_t irq_id)`, with 0x40 for GfxEop.
3. **Fault context:**
   - `sotcgpu_handle_fault` gets no OS context.
   - shadPS4's SRT-walker fault fix-up needs RIP: it patches the JIT'd load that faulted.
   - That handler is registered, but it never claims a fault. A fault inside the SRT walker (an unmapped
     pointer in a shader resource table) is therefore not recovered.
   - Proposal: add `void *os_context` (`EXCEPTION_POINTERS*` / `ucontext_t*`).
4. **Buffer slots:** `register_buffers` has no start index. The convention above (zeros skip slots) works.
   An explicit `int32_t start_index` would be clearer. There is no `unregister_buffers` either.
5. **Compute queue mapping:** there is no `map_compute_queue(ring, size, rptr)`. The scratch read pointer
   works as long as the runtime advances the real one from `done`.

## Shared-file requests
- `docs/m3/gl.patch` adds `build-*/` to `.gitignore`, so a `build-vk/` directory stays untracked.
  `git apply --check` passes.

## Open issues and risks
- Bring-up with the real game has not happened yet: nothing in the runtime calls `sotcgpu_*` until GI lands.
  Expect the integration items listed in MILESTONE3.md (recompiler asserts, AjncSystem fixes, page-tracking
  deadlocks).
- `VK_KHR_swapchain` is enabled even in headless mode, as shadPS4's `CreateDevice` requires; NVIDIA accepts
  this.
- `test_sotcgpu_headless` takes about 4 s, mostly device creation and teardown.
- The Linux paths (X11/Wayland surface, `ucontext` fault dispatch, `dlopen` probe) are written but untested.
- No Windows-specific `-march` fallback: an x86-64-v3 CPU (Haswell / Zen 1 or later) is required for
  `SOTC_VULKAN=ON`.
