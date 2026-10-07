# C: crash diagnostics + VRAM budget

Diagnostic only: no behavior changes. The patches are listed in `third_party/shadps4/README.md`.

## What the old crash line meant
- `vk_platform.h:66` is the `Check(vk::ResultValue<T>)` overload with no message. The assert had the
  result name all along, but `ASSERT_MSG` prepends `"Assertion Failed!\n"`, so `vk::Result=...` went to
  the *next* line of stderr with no timestamp prefix. It's easy to miss, and a grep for `gpu:` drops it.
  The call site was never logged.
- At runtime, only a few call sites use that overload without a message:
  - `buffer_cache.cpp` `EnsureResident`: `allocateMemory` for sparse-arena residency. **Prime suspect.**
  - `buffer.cpp` `UniqueBuffer::Create`: `createBuffer` for a new 4 GiB sparse arena page.
  - `fault_manager.cpp`: pipeline creation, at startup only.
- `vmaCreateImage` and `vmaCreateBuffer` failures have their own messages (`Failed allocating ...`), so they
  were not the ~230 s crash.

## Why residency is the suspect (reading the code)
- `EnsureResident` makes a **separate `vkAllocateMemory`** for every gap it fills, and **nothing ever frees
  it**. `resident_ranges` only grows, and no `freeMemory` exists anywhere in the buffer cache.
- So two limits can fail:
  - **VRAM:** resident bytes grow monotonically. On a 6 GB card that ends in `ErrorOutOfDeviceMemory`.
  - **Allocation count:** `maxMemoryAllocationCount` is 4096 on NVIDIA/Windows. Past it, `vkAllocateMemory`
    can return `ErrorTooManyObjects` or `ErrorOutOfDeviceMemory`, even with VRAM to spare. VMA images and
    dedicated buffers count toward the same limit.
- The texture cache GC measures *device-wide* usage (`GetDeviceMemoryUsage`), which includes the arena.
  As the arena grows, the GC evicts images harder, but it can't reclaim the arena itself.
- `SOTC_UNBOUNDED_CAP_MB` (runtime `memory.c`) bounds how much a single unbounded descriptor makes resident.
  It does not bound the total.

## What was added
1. **`Vulkan::Check`** (both overloads) takes a defaulted `std::source_location`. On failure it logs one
   line with the result name and the *caller's* file, line and function, then the `[vram]` report, then
   asserts as before.
2. **`[vram]` report** (`gpu/src/sotcgpu.cpp`, thread `sotcgpu:Vram`):
   - It runs once at init (`start`), every 10 s (`periodic`, log level Info), and before any assert
     caused by a failed Vulkan call or allocation (log level Error).
   - Per heap it reports usage and budget from `VK_EXT_memory_budget`, the heap size, and VMA's block bytes.
     shadPS4 already enables the extension when the device has it (`vk_instance.cpp`).
3. **Counters** (`Vulkan::SotcDiag` in `vk_platform.h`):
   - Arena: resident bytes and the number of `vkAllocateMemory` calls (`EnsureResident`), and the number of
     sparse arena buffers.
   - Images: live VMA images, with count and bytes (`UniqueImage`, sizes from `vmaGetAllocationInfo`).
4. **Failure lines with the request:** `EnsureResident` (size, ranges, address range, memory type, totals
   so far), sparse arena `createBuffer` (size), `vmaCreateBuffer` (size, memory type), and `vmaCreateImage`
   (extent, format, mips, layers, samples).

## Log lines to grep
```
[vram] start: ... (device limit N) ...          once; N = maxMemoryAllocationCount
[vram] periodic: arena resident=X MiB in A allocations (device limit N), B arena buffers;
       images C live, D MiB | heap0(local) usage=U budget=G size=S vma=V MiB | heap1 ...
[vkcheck] vk::Result=ErrorXxx [in <msg>] -- <HINT>   level Error, with caller file:line function
[vram] EnsureResident allocateMemory failed: <bytes> bytes (<n> ranges), addr=..-.., ...
[vram] sparse arena createBuffer failed: size=...
[vram] vmaCreateBuffer failed: ... / [vram] vmaCreateImage failed: ...
[vram] vkcheck: ... / [vram] vmaCreateImage: ...    the report printed at the failure
```
`grep -E "\[vram\]|\[vkcheck\]" build/game/smoke.log`

## Reading a 300 s run
- **Allocation count:** if A approaches the device limit (4096) before the crash, the crash is the
  allocation count, not VRAM. Fix (M4): suballocate residency from large blocks, e.g. 64–256 MiB chunks.
- **VRAM:** if heap0 `usage` approaches `budget` and arena `resident` keeps climbing, it is VRAM exhaustion
  by the arena. Fix: evict or free residency, or cap total residency.
- **Images:** if images D dominates instead, look at the texture cache GC thresholds.
- **`[vkcheck]` hint:** `DEVICE LOST` means a GPU fault or a TDR, not memory. Cross-check the System event
  log (`Get-WinEvent -FilterHashtable @{LogName='System';ProviderName='Display','nvlddmkm'}`).

## Device loss notes
- The queue submit paths (`vk_scheduler.cpp:241`, `buffer_cache.cpp` `bindSparse`) already assert with
  "Device lost during submit".
- `vk_semaphore.cpp:65` loops on `waitSemaphores` until it returns success. After a device loss that
  never happens, so a loss seen there shows up as a **hang** (a spinning thread), not a crash.
- Cheap follow-ups, not done:
  - `VK_EXT_device_fault`: after `ErrorDeviceLost`, `vkGetDeviceFaultInfoEXT` gives the faulting address
    and vendor info. It needs the extension and the `deviceFault` feature enabled at device creation in
    `vk_instance.cpp`. NVIDIA supports it on recent drivers.
  - `VK_NV_device_diagnostic_checkpoints`: `vkCmdSetCheckpointNV` per draw or dispatch, then
    `vkGetQueueCheckpointDataNV` after the loss shows the last executed marker. It costs per-command
    overhead, so it should sit behind an env var.
  - shadPS4's `enable_crash_diagnostic` (Aftermath-style) path exists in `CreateInstance`, but sotcgpu
    passes `false`.
