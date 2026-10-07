# Vendored shadPS4

These files come from [shadPS4](https://github.com/shadps4-emu/shadPS4), licensed GPL-2.0 (see `LICENSE`).

- **Commit:** `14ada48a1320` ("vk_rasterizer: Apply pending fast clears on FMASK decompress (#5283)")
- **Paths:** `src/common`, `src/video_core`, `src/shader_recompiler`, `LICENSE`
- **Imported with:** `python scripts/import_shadps4.py --src ../shadps4 --commit 14ada48`

`sotcgpu` (`gpu/`) builds these sources. It replaces what they take from shadPS4's `core/` with shim headers
under `gpu/shim/`.

## Local patches

Local patches are allowed: M4's AjncSystem fixes will be patches on this copy. Every patch must be listed
below: the file, what changed, and why. Re-running `import_shadps4.py` drops them, so re-apply them
afterwards.

| File | Change | Why |
|---|---|---|
| `src/video_core/amdgpu/liverpool.h`, `liverpool.cpp` | `SubmitGfx`/`SubmitAsc` take an optional `Common::UniqueFunction<void> on_done` (default empty). `GpuQueue` keeps one callback per submission (`submit_done_cbs`, pushed with the task); `Process()` pops it with the finished task and runs it on the GPU thread, outside the queue lock. | `sotcgpu_submit_gfx`/`_compute` promise a `done` callback once the command processor has consumed the buffers (the runtime advances compute ring read pointers with it). Liverpool had no completion hook. |
| `src/video_core/renderer_vulkan/vk_rasterizer.cpp` | `Draw`, `DrawIndirect`, `DispatchDirect` and `DispatchIndirect` return early when `SotcSkipPipeline(pipeline)` (from `gpu/shim/core/emulator_settings.h`) is true. | Debug switch `SOTC_GPU_SKIP=<hash>[,<hash>...]`: drop the draws of one shader to bisect GPU hangs and rendering bugs. Inactive when the variable is unset. |
| `src/video_core/renderer_vulkan/vk_presenter.cpp` | In `Presenter::Present`, `#ifdef SOTC_NO_IMGUI`: the "Draw the game" ImGui block and `ImGui::Core::Render` are replaced by a plain fitted blit of the frame into the swapchain image (clear to black, `MakeImageBlitFit`, layouts restored to what the rest of `Present` expects). Without the define the file is unchanged. | shadPS4 draws the game frame as an ImGui image; sotcgpu compiles ImGui out (no imgui submodule, no src/imgui sources). |
| `src/video_core/buffer_cache/buffer_cache.h`, `buffer_cache.cpp` | `ObtainBuffer` streams a small read-only buffer only when the region is CPU-modified, and adds it to a new `streamed_ranges` set. Otherwise it binds the buffer in place from the arena. `TickFrame` calls the new `FlushStreamedRanges`, which uploads those ranges to the arena (`EnsureResident` + `SynchronizeMemory`, split at arena pages and 64 MiB). | SotC binds ~85k small read-only buffers per frame (~330 MiB). Streaming all of them each bind wrapped the 128 MiB stream buffer several times a frame, and each wrap waited for the GPU. The title ran at 4 flips/s with 1.3 GiB/s of guest copies; with the patch, copies fall to ~50 MiB/s and the title runs at 8–20 flips/s. |

Everything else shadPS4 takes from its `core/`, `imgui/`, Tracy and Abseil is replaced by headers under
`gpu/shim/` (see `docs/m3/gl.md`), not by edits here. `common/logging/log.cpp` and `common/path_util.cpp` are
not compiled; `gpu/src/log.cpp` and `gpu/src/path_util.cpp` replace them.
