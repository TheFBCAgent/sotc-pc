# Whole-draw memo, W3: ring recon and `SotcRingMap` (wave 1)

Part of `draw-memo.md` (section 4, unit W3). The eboot findings with addresses are in the private
notes (`sotc-pc-notes/history/draw-memo-rings.md`, "W3 results"). This file holds only what
is generic.

## 1. What the recon found (generic)

- **Per-draw constants are not embedded in the command buffer.** The engine's DCB embedded data
  is only 8-byte labels for EOP and WAIT_REG_MEM.
- **The engine has dynamic GPU buffers with a "map discard"**: every map takes fresh memory from
  a **per-frame tagged heap** and rewrites only the V# base (word 0 and the low 12 bits of word
  1). Stride, num_records and word 3 stay the same for the buffer's lifetime. More than 150 call
  sites use it.
- **SRT tables are per-draw too.** The per-draw shader resource tables come from the same heap,
  and the user data passed per draw is a pointer to such a table. The tables hold raw pointers to
  the constants. So per-draw constants may reach the GPU as pointer reads (the SRT walker and flat
  reads) rather than as V# loads. W1's probe should tell which path they take.
- **The frame heap is not a contiguous ring.** It is a set of per-thread bump allocators. They
  take 1 MiB pages, tagged with the frame slot, from a GPU page arena. That arena grows in 32 MiB
  regions inside the big direct-memory blocks the engine maps at startup.
  - At the start of each frame, the renderer waits for the GPU frame two frames back. It then frees
    the pages tagged with the slot it is about to reuse.
  - The render heap keeps **2 frames**. A game-to-GPU heap on the same arena keeps 5.
  - So the same 1 MiB pages come back every 2 (or 5) frames. Inside a page, positions shift with
    allocation order and with how threads interleave.
- **Static buffers** (created once, sub-views, material parameters, per-view constants) keep
  their base address.
- **No address range is ring-only by construction**: the arena's regions sit inside the general
  direct-memory blocks. **Rings have to be discovered at run time.**

### What W1's classification should show
| source | base pattern | expected class |
|---|---|---|
| created / static buffers | same base every frame | static |
| map-discard constants, SRT tables | new base every draw and frame, inside a few 1 MiB pages that recur every 2 frames; increasing within a frame and thread, **no fixed stride** | moving |
| game-to-GPU data | as above, recurring every 5 frames | moving |
| uploads | - | one-shot |

"Moving with a fixed stride" will under-count these bindings. The useful test is "moving, inside a
small recurring set of 1 MiB pages".

## 2. `SotcRingMap` (`buffer_cache/sotc_ring_map.h`)

A Vulkan-free header, used on stage B only (like `SotcWriteTicks`). It maps a guest address to
`(ring id, offset)`. Storage is a sorted fixed array of at most 64 rings, with no allocation and
no hashing. A lookup is one binary search: under 30 ns per lookup with 64 rings, the test's random
generator included.

```cpp
VideoCore::SotcRingMap map;
map.Register(base, size, id);   // Added / Merged / Present / Overlap / Full / Empty
map.Find(addr);                 // optional<Hit{id, offset, base, size}>
map.FindRange(addr, size);      // only if the whole range lies in one ring
map.Unregister(id); map.Clear();
map.Generation();               // bumped by every change: part of a memo's generation
```

- **Learning page by page.** A range that touches or overlaps rings with the same id merges into
  one entry. Registering each 1 MiB page that receives moving bases therefore builds windows.
- A range that overlaps a ring with another id is rejected. Touching one is fine.
- **Generation.** A merge to the left moves the base, and so every offset in that ring. A memo
  keyed by `(ring id, offset)` must therefore also store `Generation()`.
- Unit test: `gpu/tests/test_ring_map.cpp`. It checks lookups, edges, merges, overlap and full
  rejection, a random check against a per-page model, and timing. It is registered in
  `gpu/CMakeLists.txt` and passes.

Integration still owes a row in `third_party/shadps4/README.md` (a shared file, so it was not
edited here). Suggested text:
`src/video_core/buffer_cache/sotc_ring_map.h` (new) | `SotcRingMap`: guest address -> (ring id,
offset) for per-frame allocator memory, registered at run time, merging same-id neighbours,
generation-bumped | Whole-draw memo W3 (`docs/next/draw-memo-W3.md`). Unit test:
`gpu/tests/test_ring_map.cpp`.

## 3. Recommendations for W5 (discovering rings at run time)

1. **Learn from behaviour (preferred, game-agnostic).** Reuse W1's per-binding address history.
   - When a binding slot keeps its stride, num_records and word 3 but its base changes from one
     frame to the next, register the base's 1 MiB page as a ring. Use one id per learned arena,
     for example one id for everything learned.
   - Same-id merging coalesces the pages into windows.
   - Cap the total learned size, and forget the map when a window stops receiving moving bases
     for a long time. The engine's arena only grows, so forgetting should be rare.
2. **Key for L1.** For a binding inside a ring, the key holds the ring id instead of the address,
   plus size, stride and word 3. The ring map's `Generation()` joins the layer's generation
   vector.
3. **Expect no content skip.** The CPU rewrites ring memory every frame, so W2's write sequence
   will report "changed" for these bindings. The gain has to come from cheap resolution: one
   resident buffer per ring (or per window), bound at an offset, with only the written bytes
   synced. It cannot come from skipping the content.
4. **Eboot hook (fallback only).** Wrapping the engine's map-discard, or its arena region
   creation, would give exact regions. It is game-specific, so use it only if the behavioural
   learning misses too much. The addresses are in the private notes.
5. **Check with I1 first.** Learning only pays if I1 shows that moving bases are a large part of
   L1's misses (`draw-memo.md`, go/no-go). If per-draw constants arrive mostly as pointer reads
   (SRT walker / flat), L1's key has to cover those bindings too.
