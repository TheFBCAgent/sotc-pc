# R: residency / VRAM, part 1: suballocation

## The problem
- `BufferCache::EnsureResident` made one `vkAllocateMemory` per call (the gaps it fills, at least one
  64 KiB block on NVIDIA). Nothing ever freed it.
- Two runs made 1,700–2,400 such allocations (20–22k blocks resident) before the
  `ErrorOutOfDeviceMemory` crash. NVIDIA's `maxMemoryAllocationCount` is 4096, and VMA's blocks and
  dedicated images count toward it too. So the crash is at least partly the allocation count, not
  only VRAM (`docs/next/c.md`).

## Design (part 1)
- **`VideoCore::ResidencyAllocator`** (`third_party/shadps4/src/video_core/buffer_cache/residency_allocator.{h,cpp}`,
  a listed patch) carves device-memory **chunks** into block-granular **slices**.
  - The core doesn't depend on Vulkan. Memory is an opaque non-zero `u64` handle. `AllocateFn(bytes)` and
    `FreeFn(handle)` are injected. In the buffer cache they call `vkAllocateMemory` (type
    `arena_memory_type_index`, `eDeviceAddress` flag) and `vkFreeMemory`.
  - Units are blocks (`block_size`, the sparse alignment, ≥ 16 KiB). Offsets are block offsets in the
    chunk, so `memoryOffset` is always block-aligned.
  - Chunk size: `ResidencyAllocator::DEFAULT_CHUNK_BYTES` = 256 MiB. Chunks are created lazily, and none
    exists until the first `EnsureResident`.
  - Bookkeeping: per chunk, an `offset → length` map of free ranges, always coalesced. One global
    `std::set` of `(length, chunk, offset)` orders every free range by size.
- **`Allocate(blocks, out)`** appends one or more slices:
  1. **Best fit:** the smallest free range that holds the whole request gives one contiguous slice.
  2. **Otherwise, fill the holes:** take free ranges largest-first, and stop as soon as one fits what
     is left. A request becomes at most `MAX_SLICES` (16) slices.
  3. **The rest comes from a new chunk:** a 256 MiB chunk, or a **dedicated** chunk of exactly the rest
     when the rest is larger than a chunk.
     - If the 256 MiB chunk fails and the rest is smaller, it retries with exactly the rest. This helps
       near exhaustion.
     - If everything fails, the slices taken so far are given back and `out` is unchanged.
  - Without frees (part 1), a chunk's free tail fills the next request before a new chunk opens, so there's
    no tail waste. With the current request sizes (≤ 64 MiB uploads, `SOTC_UNBOUNDED_CAP_MB` = 128),
    expect one split per new chunk at most.
- **`Free(slice)`** accepts any sub-range of what was handed out within one chunk, such as a
  `BufferCache::Backing`.
  - Backings only merge when `memory` matches and the offsets are contiguous, so a Backing never spans
    two chunks.
  - Unknown memory, out-of-range slices and double frees return `false` and change nothing.
  - Unused in part 1.
- **Empty chunks** are kept: they are cheap to reuse and avoid allocate/free churn.
  - `ReleaseEmptyChunks(keep)` frees empty chunks beyond `keep` regular ones. Dedicated (odd-size)
    chunks are always freed.
  - Part 2 calls it after eviction. The caller makes sure the GPU no longer uses the chunk (binds
    replaced and the bind submission completed).
- The destructor frees every chunk. `BufferCache::~BufferCache` now does `waitIdle()` first.
  - `residency` is declared before `arenas`, so the sparse buffers are destroyed before their memory.
  - Before this, the memory was never freed (it leaked at shutdown).
- **`EnsureResident`** allocates `resident_blocks` as slices and walks the gaps and slices together.
  - A gap can span several slices, and a slice can span several gaps. Each piece becomes one
    `SparseMemoryBind` and one `Backing {memory, offset}`.
  - The BDA page-table writes and copies are unchanged: one copy per gap.
  - The arena migration in `GetArena` already rebinds per Backing with `backing.memory` and
    `backing.offset`, so it needs no change.
  - `DownloadMemory`, `SynchronizeMemory` and the fault manager don't look at `Backing`.
- The allocator has its own mutex: `GetStats` is safe from any thread. `EnsureResident` itself still runs
  where it did before, on the GPU thread.

## Diagnostics (C's `[vram]` line)
- `Vulkan::SotcDiag` (`vk_platform.h`):
  - `arena_resident_bytes`: bytes in slices, i.e. what was made resident.
  - `arena_allocations`: chunk `vkAllocateMemory` calls, cumulative.
  - New: `arena_chunks` (live) and `arena_chunk_bytes`.
  - `BufferCache::PublishResidencyStats` stores them after every allocation.
- The periodic line is now:
  ```
  [vram] periodic: arena resident=X MiB in C chunks / Y MiB (A allocations, device limit 4096), B arena buffers; images ...
  ```
- New lines:
  - `[vram] residency chunk of N MiB allocated`, at Info level, once per chunk.
  - `[vram] residency chunk of N MiB failed: <result>`, a warning.
  - On total failure, the unchanged `[vram] EnsureResident allocateMemory failed: ...` line, now with
    chunk totals, then `[vkcheck]` for the failed `vkAllocateMemory` and the assert.

## Tests
- `gpu/tests/test_residency.cpp` (`build-vk`, ctest `test_residency`) fakes device memory with callbacks.
  It covers:
  - basic allocation and lazy chunks
  - a split across chunks (the tail is used first)
  - oversize requests (dedicated chunk, also after filling a tail)
  - free and reuse, partial frees, coalescing, double and invalid frees
  - `ReleaseEmptyChunks`
  - fragmentation (holes used, the `MAX_SLICES` cap, best fit)
  - allocation failure (fallback to an exact-size chunk, full rollback)
  - the destructor
  - 4,000 random allocate/free steps against a model
- No Vulkan device is created.

## For the GPU run (main session)
- `python scripts/smoke.py --exe build-vk/sotc-runtime.exe --gpu vulkan --seconds 300`, then
  `grep -E "\[vram\]|\[vkcheck\]" build/game/smoke.log`.
- **Allocations** (`A` in the periodic line) should stay in the **tens**: about resident MiB / 256, plus
  dedicated chunks. Before the change it reached 1,700–2,400.
- **Resident vs. chunk bytes:** `X` should be close to `Y` (within one chunk), since nothing is freed yet.
- If the ~230 s crash still happens:
  - With `A` small and heap0 `usage` near `budget`, it's real VRAM exhaustion by the arena. That's part 2.
  - A `residency chunk ... failed` warning followed by a successful smaller chunk means VRAM is nearly full.
- **Shutdown:** `~BufferCache` now waits for the device to go idle before freeing. Watch for a hang at exit.
  `sotcgpu_shutdown` already skips teardown when the GPU isn't idle, so this should be safe.
- **Rendering must be unchanged.** Any new corruption would point at the bind/offset walk in
  `EnsureResident`: check `memoryOffset`, which must equal `backing.offset << block_shift`.

## Part 2: eviction under `VK_EXT_memory_budget` (not done)
1. **Usage tracking:** a last-used tick per resident range, set in `ObtainBuffer`/`EnsureResident` and
   the fault path. A coarse per-64 MiB (or per-arena-page) counter is probably enough.
2. **Trigger:** once per frame (`TickFrame`), read `heapUsage`/`heapBudget` for the arena's heap. The
   query is in `gpu/src/sotcgpu.cpp` `VramReport`; move it to a helper. Above about 85–90 % of the
   budget, evict the least-recently used ranges not used for N frames, down to a lower watermark.
3. **Eviction of a range:**
   1. If it's GPU-modified (`gpu_modified_ranges`), download it first (`DownloadMemory`) so the guest
      copy is current.
   2. Queue a sparse bind with `memory = VK_NULL_HANDLE` for the range (`BindsForArena`).
   3. Zero its BDA page-table entries so the fault manager faults it back in.
   4. Remove it from `resident_ranges`.
   5. Mark it CPU-modified in the memory tracker, so the next `EnsureResident` + `SynchronizeMemory`
      re-uploads it.
   6. Free its Backings with `ResidencyAllocator::Free` **only after** the timeline tick of the
      unbinding submit (and of every command buffer that used it) has passed. Keep a deferred-free
      list keyed by `memory_semaphore` / scheduler tick.
4. Then `ReleaseEmptyChunks(1)` to return whole chunks to the driver. Only fully empty chunks can be
   released, so eviction should prefer the ranges of the least-used chunk.
5. **Interplay:** the texture cache GC measures device-wide usage, so both should use the same budget
   numbers. Then `SOTC_UNBOUNDED_CAP_MB` can go.
6. **Fragmentation:** holes are reused (best fit, then up to 16 slices per request).
   `[vram]` should also show `free_ranges`/`largest_free` from `GetStats()` once eviction runs.

## GPU results (2026-10-07, main session)
- The driver reports `maxMemoryAllocationCount` = 4294967295 on this machine (RTX 2060, 596.49), so the
  allocation count was never the limit: the out-of-memory crashes are VRAM (heap0 usage 6.5 GB vs a
  5.1 GB budget, images 3.8 GB). See `docs/next/t.md`.
- A/B, 3 intro runs each, 100 s, shots every 5 s (`build/game/logs/*-ab-*.log`):
  - `chunks`: one OK, one stall (3D stops at 55 s, the frame freezes), one crash at 9 s: a guest read
    fault on a GPU-tracked page (`tracking known, guest prot 3, host prot -1`), i.e. a readback that didn't
    happen.
  - `legacy`: 3 of 3 OK.
- So `legacy` is the default again; `SOTC_RESIDENCY=chunks` opts in. Not the cause: `IntervalList`
  coalescing (correct for contiguous slices), arena migration (rebinds per `Backing` offset). All callers
  run on the GPU thread. Next suspects: something that assumes one `VkDeviceMemory` per `Backing` run, and
  readback/fault paths that look up memory by range.
