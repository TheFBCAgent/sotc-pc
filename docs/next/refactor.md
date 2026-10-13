# Workstream `refactor` (session 60): pipe and CPU copy out of `vk_rasterizer.cpp`

Plan item 13 (NEXT.md). A pure move: the moved code is byte-for-byte the old text, the
behaviour is unchanged, and the generated SPIR-V is the same (no `kPipelineCacheVersion` bump).
All files are in `third_party/shadps4/src/video_core/renderer_vulkan/`.

## What moved where

`vk_rasterizer.cpp`: 3,215 -> 2,124 lines (based on main 2908534, after session 59).

- **`sotc_rasterizer_internal.h`** (new, private to the rasterizer): the session 43-47
  description of the two-stage pipeline, `enum class SotcPipeKind`,
  `struct Rasterizer::SotcPipeArgs` and `struct Rasterizer::SotcPipe`. It is included by
  `vk_rasterizer.cpp` (the constructor creates `SotcPipe`, `~Rasterizer` pushes the `Stop`
  packet and destroys it, `Draw`/`DrawIndirect`/`DispatchDirect`/`DispatchIndirect` build
  `SotcPipeArgs`, the register accessors read `sotc_pipe->regs`) and by `sotc_pipe.cpp`.
  It also declares `bool SotcCpuCopyLogOn();`.
- **`sotc_pipe.cpp`** (new): the pipe and its verify mode, in the old order:
  `SotcPipeSubmit`, `SotcPipeRun`, `SotcPipeThread`, `SotcPipeWait`, `SotcPipeDrain`,
  `SotcPipeRelease`, `SotcPipeCommand`, `SotcPipeRunCommands`, `SotcPipeDefer`,
  `SotcPipeFence`, `SotcPipeWaitMet`, `SotcFaultOnGpuSide`, `SotcNoteQueuedFault`,
  `SotcPipeOverlay`, `SotcPipeMergeRead`, `SotcPipeNoteWrites`, `SotcPipeVerifyWalk`,
  `SotcPipeReport`, `SotcPipeRunPacket` (all `Rasterizer::` members).
- **`sotc_cpu_copy.cpp`** (new): `SotcCpuCopyMode`, `SotcCpuCopyLogOn`, `SotcCopySharp`,
  `Rasterizer::SotcCpuCopy`, `SotcCpuCopyReject`, the three log-only reject codes
  (`SotcCopyResources`, `SotcCopyImage`, `SotcCopyWriteFailed`),
  `Rasterizer::SotcCpuCopyCompute`, `Rasterizer::SotcCpuCopyCommit`. The Vulkan-free part stays
  in `sotc_cpu_copy.h`.

Stayed in `vk_rasterizer.cpp` (on purpose): the constructor/destructor, the hooks inside
upstream functions (`SotcPipeSubmit` calls in Draw/Dispatch, `SotcPipeDrain` guards, the CPU
copy calls in `RecordDispatchDirect`), `SotcGcTick`, `SotcPatchFlatBuf`, the memos
(`SotcTexMemoOn`, `SotcRenderStateMemoOn`, `SotcMakeBeginSignature`) and the register accessors
`Regs()`, `CbExtent()`, `DbExtent()`, `CsRegs()`: the recording code in `vk_rasterizer.cpp`
calls these on every draw, and keeping them in the same TU keeps them inlinable there (no
codegen change without LTO).

## Non-verbatim edits (each needed to compile across files)

- `SotcCpuCopyLogOn` lost `static` (now external, declared in `sotc_rasterizer_internal.h`):
  `RecordDispatchDirect` (`vk_rasterizer.cpp`) and the CPU copy both call it. Its function-local
  statics (`from`, `start`: the 8 s log window) stay one instance; an `inline` header copy would
  also be one instance but would have moved the body into a header.
- The other file-static helpers (`SotcCpuCopyMode` with its `static const int mode` env read,
  `SotcCopySharp`, `SotcCpuCopyReject` with its `logged` array) are used only by the CPU copy,
  so they stay `static` in `sotc_cpu_copy.cpp`: still one instance each, still read lazily on
  first call (no static-initialization-order change). The `thread_local`s (`words`,
  `src_words`, `gpu_only`, the pipe's `copy_words`) are function-local and moved with their
  functions.
- The new `.cpp` files repeat `vk_rasterizer.cpp`'s include list; the header includes what the
  structs need (`sotc_gpu_pipe.h`, `liverpool.h`, `vk_rasterizer.h`, `unique_function.h`, std).
- Comments only: a pointer comment where each block was, and "see SotcPipeArgs above" on the
  register accessors now names the header.

No CMake change: `gpu/CMakeLists.txt` globs `video_core/*.cpp` (`CONFIGURE_DEPENDS`), so a
reconfigure picks the new files up.

## How it was checked

- Sorted-line diff of the old `vk_rasterizer.cpp` against the four new/changed files together:
  the only differences are `#include` lines, SPDX headers, `namespace Vulkan {` / `}` wrappers,
  the new comments, the declaration of `SotcCpuCopyLogOn` and its lost `static`.
- `third_party/shadps4/README.md`: the rows whose `vk_rasterizer.cpp` code moved name the new
  files (CPU copy, pipe build steps 1-4, `SOTC_COPY_FAST`, B and A wait timers, asynchronous
  readback, hot-flush budget, submits by caller), plus one row for this refactor.
- Build and tests: see the session's report (RelWithDebInfo `build-vk`, clang-cl, `-j 6`;
  `ctest -E test_sotcgpu_headless`). No game run (the main session owns the GPU); a short
  `SOTC_GPU_PIPE=1` and `SOTC_GPU_PIPE_VERIFY=64` run after the merge confirms it.
