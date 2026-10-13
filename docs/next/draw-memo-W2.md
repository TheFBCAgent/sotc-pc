# Whole-draw memo, W2: the CPU write sequence

Plan: `draw-memo.md` (section 2, "A per-range CPU write sequence"; brief in section 5).

## What was built

- `third_party/shadps4/src/video_core/buffer_cache/sotc_cpu_write_seq.h`: `VideoCore::SotcCpuWriteSeq`.
  It is header-only and Vulkan-free.
- `gpu/tests/test_cpu_write_seq.cpp`: correctness cases, a concurrent-bump check and a microbenchmark.
- `gpu/CMakeLists.txt`: the `test_cpu_write_seq` block, next to `test_write_ticks`.

Not applied: no shared file was edited (`memory_tracker.h`, `buffer_cache.{h,cpp}`,
`vk_rasterizer.cpp`). Integration (W5) does that.

## API

```cpp
struct Snap { u64 addr, size, sum; };          // 24 bytes, whatever the binding size
void Bump(u64 addr, u64 size);                 // +1 on every 64 KiB block touched; any thread
Snap Snapshot(u64 addr, u64 size) const;       // the sum of the covered counters
bool Unchanged(const Snap&) const;             // the sum is still the same
u64  Counter(u64 addr) const;                  // one block's counter (tests, logs)
```

- **Storage.** It is two levels, like `MemoryTracker::top_tier`:
  - 65536 atomic leaf pointers, one per 16 MiB of the 1 TiB guest space (512 KiB, allocated up front).
  - A leaf holds 256 `std::atomic<u64>` counters (2 KiB). It is installed by CAS on its first
    `Bump` and never freed.
  - A block costs one array index, not `SotcWriteTicks`' hash lookup.
  - The game's working set (~5 GB) needs ~320 leaves, about 640 KiB.
- **Snapshot.** Counters only grow, so the sum over the covered blocks is unchanged exactly when
  every counter is unchanged. u64 counters never wrap, which is the wraparound policy. So `Snap`
  is a fixed size with no heap and no cap. Above 64 KiB the cost grows linearly with the blocks
  covered (16 MiB is 256 blocks, ~100 ns).
- **Edge cases.**
  - Size 0: `Bump` does nothing, and a zero-size snapshot is always unchanged.
  - A range that wraps 2^64 is clamped.
  - Blocks at or above 1 TiB share one "far" counter (conservative).
- **Concurrent snapshot.** A snapshot that races with bumps is safe: a bump of a block after the
  snapshot read it raises the later sum, so the result is "changed".

## Benchmark (Ryzen 5 3600, clang-cl 18 -O2, 1024 bindings spread over 200 MiB)

| binding | Unchanged (hit) | Snapshot + Unchanged |
|---|---|---|
| 256 B | 2.7 ns | 4.0 ns |
| 4 KiB | 3.1 ns | 4.0 ns |
| 64 KiB, unaligned (2 blocks) | 2.9 ns | 4.4 ns |
| 1 MiB | 7.7 ns | 13.0 ns |
| 16 MiB | 103 ns | 194 ns |

The target was under 50 ns for 64 KiB or less; the measured cost is about 4 ns.

**Build note.** The worktree's submodules are empty, so `build-vk` could not be configured
there. The test was compiled directly with clang-cl, with `sotcgpu_options`' include paths and
flags (`-std:c++latest -O2 -march=x86-64-v3 -W4`), behind the build lock. The CMake block is a
copy of `test_write_ticks`' and was not configured.

## Threading conclusion

`SotcWriteTicks` is GPU-thread only. **This one cannot be:** the tracker sets CPU-modified bits
on the faulting thread. `GuestFaultSignalHandler` (`page_manager.cpp`) calls
`Rasterizer::InvalidateMemory`, which reaches `MemoryTracker::InvalidateRegion` on whichever
thread wrote:

- guest threads;
- Liverpool's thread (stage A), with or without a pipe guard;
- the thread that unmaps memory;
- on Linux, the uffd thread.

Only the download that follows a write to a GPU-modified page is queued to B. So:

- `Bump` uses relaxed `fetch_add`, and leaves are installed with a CAS (acquire/release on the
  pointer).
- `Snapshot`/`Unchanged` use relaxed loads. They are safe on any thread, but the memo calls them
  on B.
- Ordering against the tracker comes from two rules:
  1. **Bump after the CPU bit is set**, after the RegionManager mutex is released. If the bump came
     first, B could snapshot it, then run a sync that does not see the bit yet, and the memo would
     keep stale arena data. With the bump after the set, the region mutex orders things: a bump
     that B's snapshot could read happens after B's sync, so it cannot be missed.
  2. **Snapshot before the `SynchronizeMemory` that clears the binding's bits**, and only when that
     sync covers the whole binding. Any set after the clear then comes with a bump after the
     snapshot.
- Visibility on B's next draw comes from the submit chain: guest submit, then Liverpool's queue,
  then A, then B's packet queue (all mutexes).

Bumping from B's side instead (where the tracker clears bits in `ForEachUploadRange`) does not
work. The memo's hit skips `ObtainBuffer`, so that upload never runs, which makes it circular.

## Sites: where the tracker sets or clears CPU-modified state

The bit that matters is `RegionManager::cpu` (set = CPU-modified = unprotected, and uploaded at
the next sync). Only `MemoryTracker` changes it: `RegionManager::ChangeRegionState` and
`ForEachModifiedRange` have no other callers. **Recommended integration: three `Bump` calls
inside `MemoryTracker` (S1-S3)**, after the region change, with the `SotcCpuWriteSeq` owned by
`BufferCache` and passed to `MemoryTracker` (or a pointer member). That covers every caller
below without touching them.

### Sets (Bump needed)

| # | site | what | thread | Bump |
|---|---|---|---|---|
| S1 | `memory_tracker.h: MemoryTracker::InvalidateRegion`, the per-region `ChangeRegionState<Set,None,false>` for regions with no GPU-modified page | write fault on a protected page that holds no GPU data: marks CPU-modified and unprotects | **faulting thread** (guest, A, unmapper, uffd) | after `manager->Unlock(bounds)`, for `(offset, size)` of that region, or once after `IteratePages` for `(cpu_addr, size)` (conservative: also bumps when `should_flush`, which is harmless) |
| S2 | `memory_tracker.h: MemoryTracker::MarkRegionAsCpuModified` | `ChangeRegionState<Set,None>` | depends on the caller, see S2a-c | after `IteratePages`, `Bump(cpu_addr, size)` |
| S2a | `InvalidateRegion`, early return when readbacks are `Disabled` | as S1 | faulting thread | covered by S2 |
| S2b | `buffer_cache.cpp: BufferCache::ReadMemory`, `flush_request`, `is_write && !(async && async->taken)` | write fault on GPU-modified pages: download, then mark CPU-modified | B (queued fault, `SendCommand`), or A holding a guard / B inline (`assume_locks`) | covered by S2 |
| S2c | `buffer_cache.cpp: BufferCache::SotcRbFinish` (`slot.request.is_write`) | async readback (session 50) finished: mark the faulting range CPU-modified | B (`sotc_rb_post`, `SotcRbFinishReady`) | covered by S2 |
| S3 | `memory_tracker.h: MemoryTracker::UnmarkRegionAsGpuModified(..., is_write=true)` | `ChangeRegionState<Set,Clear>` | no caller passes `true` today (`DownloadMemory`, `SotcRbFinish`, the prefetch serve all pass `false`) | add the bump in the `is_write` branch anyway, for future callers |
| S4 | `region_manager.h: RegionManager` constructor, `cpu.Fill(true)` | a new 16 MiB region starts all CPU-modified (`MemoryTracker::CreateRegion`, only from `ForEachUploadRange`'s `IteratePages<true>`) | B | **no bump needed**: regions are created by a sync, which is the one the snapshot precedes, and regions are never destroyed or recycled (`free_managers` is never refilled). If that ever changes, bump the whole 16 MiB |

Callers that reach S1/S2 and so need nothing of their own:

- `Rasterizer::InvalidateMemory` (`vk_rasterizer.cpp`), from `page_manager.cpp`:
  - `SignalImpl::GuestFaultSignalHandler`, reached from `sotcgpu_handle_fault`, then
    `DispatchFault` (`gpu/src/sotcgpu.cpp`), from `runtime/src/gpu_bridge.c`;
  - Linux `UffdImpl`'s thread.
- `Rasterizer::UnmapMemory` (from `sotcgpu_unmap_memory`) calls `BufferCache::InvalidateMemory`.
  On unmap the whole range is marked, and the bump covers it.
- `Rasterizer::CopyBuffer`'s host `memcpy` fast path (`vk_rasterizer.cpp`, DMA): a write to a
  protected `dst` faults on A while it holds the DMA guard, then `InvalidateMemory(assume_locks)`,
  then S1.
- Liverpool's `WRITE_DATA`/DMA writes and HLE file reads into guest memory (bounce buffers): they
  all fault, then S1.

### Clears (no Bump; they constrain where Snapshot goes)

| site | what | thread | rule |
|---|---|---|---|
| `MemoryTracker::ForEachUploadRange`, via `BufferCache::SynchronizeMemory` | clears CPU (and with `is_written`, sets GPU); re-protects; uploads the ranges | B (A with a guard) | **Snapshot before it** in `ObtainBuffer` |
| ... from `ObtainBuffer`, normal path | one `SynchronizeMemory` over the whole binding | B | memoizable |
| ... from `ObtainBuffer`, the mirrored path (session 21 CPU copy, `sotc_mirrored_*`) | piecewise syncs that together cover the whole binding | B | memoizable for the CPU-modified part, but it is a written binding (see W5 notes) |
| ... from `FlushStreamedRanges` (`TickFrame`) | clears the pages that the **stream path** left CPU-modified | B | the stream path itself (`ObtainBuffer`: `!is_written && size <= 16 KiB && !GpuModified && CpuModified`) leaves the bits set and returns a `stream_buffer` offset: **never memoize a stream result** |
| ... from `FaultManager::ProcessFaultBuffer` (via `ObtainBuffer`) | DMA fault buffer | B (`DeferOperation`) | nothing |

### Host writes that bypass the tracker (no CPU bit, so no Bump, by design)

`TryWriteBacking` writes guest memory through the backing alias without faulting:

- `SotcWriteDownload` (buffer downloads, sync and async);
- `SotcServeFromPrefetch`;
- the CPU copy commit (`sotc_cpu_copy.cpp`, session 47: A computes, B commits);
- fence labels (`Liverpool::SotcRunFence`: EOP, EOS, ReleaseMem);
- `IT_GET_LOD_STATS` zeroing;
- image downloads (`texture_cache.cpp`).

None of them sets a CPU bit, so today's `ObtainBuffer` does not upload them either. A memo hit
gives the same result as today's path. The memo matches current behaviour; it does not fix
anything here.

If any of these ever starts marking CPU-modified (`MarkRegionAsCpuModified`), S2 covers it. The
A-side overlay (`SotcPendingRanges`, `SotcPipeOverlay`) is stage A's view only and touches no
tracker state.

### Invariant for `CLAUDE.md` (at integration)

> Anything that sets `RegionManager` CPU-modified bits bumps `SotcCpuWriteSeq` **after** the
> set (it lives in `MemoryTracker`'s three Set paths). A cached buffer binding is snapshotted
> before the `SynchronizeMemory` that covers it, and stream-path or texel-buffer results are
> never memoized.

## Notes for W5 (L1 integration)

- **What a hit may skip.** A hit may skip `GetArena`, `EnsureResident` and `SynchronizeMemory`
  for **read** bindings only. Written bindings still need `SotcNoteGpuWrite` (the plan's "write
  notes stay") and the mirrored-copy split.
- **Texel buffers.** `is_texel_buffer && !is_written` runs `SynchronizeMemoryFromImage` on every
  call: an image-to-arena copy (or an HTile fill) that the CPU write sequence does not track. Do
  not memoize them, or key them on the texture cache's state.
- **Other conditions.** The memo payload `{Buffer*, offset}` is the arena's. It also needs the
  buffer-cache generation that the plan names: arena or residency changes, `GetArena`.
- **Snapshot placement.** Snapshot at the top of `ObtainBuffer`'s arena path, before
  `EnsureResident`/`SynchronizeMemory`. Taking it after the sync would race with a fault between
  the clear and the snapshot.

## Open questions

1. **Is the counter needed at all?** All bindings of a range share one arena (except the stream
   buffer), so "no CPU bit set in the range now" (`MemoryTracker::IsRegionCpuModified`) is
   exactly "today's sync would upload nothing". It needs no snapshot and no bump sites, and it
   also covers syncs by other bindings. Its cost is `IteratePages` plus 1-2 word-mask tests for
   a binding of 64 KiB or less. That is probably 5-10 ns: not measured here, because the test
   would need the shim's `EmulatorSettings`.

   What the counter adds:
   - one check for a whole draw (sum several bindings into one `Snap`);
   - no dependence on the RegionManager's racy unlocked word reads.

   W5 should measure both inside `BindBuffers` before choosing.
2. **Bump granularity.** S1 bumps the 64 KiB block of each faulting 4 KiB page. A write storm
   into one block costs at most one bump per fault, and faults happen only on protected pages
   (once per page per sync), so the rate is the fault rate (hundreds to thousands per second).
3. **Atomic cost on the fault path.** One relaxed `lock xadd` per fault, which is noise next to
   the fault itself (~µs).
4. **The 512 KiB top level.** A hash map would make it smaller, but would need a lock for
   concurrent `Bump`s. The flat array was kept.
