# Q: predication + LOD stats

Workstream Q from `docs/NEXT.md`. Built and unit-tested only; the game wasn't run (the main session owns the
GPU).

## What shadPS4 had (14ada48; `../shadps4` is the same commit, no newer upstream locally)
- `IT_SET_PREDICATION` and `IT_GET_LOD_STATS`: logged as "Unimplemented", otherwise ignored.
- No `VK_EXT_conditional_rendering` anywhere in `video_core`, and no Vulkan occlusion queries.
- `EVENT_WRITE` `ZPASS_DONE` (`PixelPipeStatDump`) is faked on the CPU: each event writes a global counter with
  the valid bit (bit 63) into the `num_counter_pairs` (8, or 16 on Neo) slots at the event address, 16 bytes
  apart, and the counter grows by 0x2FFFFFF per event. A begin/end pair therefore always reads "visible".
- The type-3 header's predicate bit (bit 0) is parsed (`PM4Type3Header::predicate`) but never used.
  shadPS4's GnmDriver HLE sets it on draws and dispatches when the Gnm `flags & 1` bit is set.

## Packet semantics (GFX6-8 CP, as Mesa radeonsi/radv drive it)
`IT_SET_PREDICATION` (0x20), two body dwords (`PM4CmdSetPredication` in `pm4_cmds.h`):

| Field | Bits | Meaning |
|---|---|---|
| `start_addr_lo` | DW1 | Low address; bits 3:0 zero for ZPass/PrimCount. |
| `start_addr_hi` | DW2 7:0 | Address bits 39:32. |
| `draw_visible` | DW2 8 | 1: draw if visible / no overflow / value != 0. 0: draw if the opposite. |
| `hint` | DW2 12 | 0: wait for the final results. 1: draw if they aren't ready. |
| `pred_op` | DW2 18:16 | 0 Clear, 1 ZPass, 2 PrimCount, 3 Bool64, 4 Bool32. |
| `continue` | DW2 31 | 1: OR this result with the previous one (one packet per result block). |

- A non-Clear op starts a predicated range; it lasts until a `pred_op = Clear` packet. It is CP state, so it
  survives indirect buffers (and submissions) until cleared. Our state is file-static in `liverpool.cpp`,
  touched only by the GPU thread, so this holds.
- Inside the range only packets with the header predicate bit are conditional. Others always run.
- ZPass: the CP sums `end - begin` over the DB counter pairs at the address; "visible" means the sum is non-zero.
- The compute queues (MEC) don't have `SET_PREDICATION`; only `ProcessGraphics` handles it.

`IT_GET_LOD_STATS` (0x8E): layout from the CI PM4 headers as best remembered (`PM4CmdGetLodStats`): `bu_size`,
`base_lo` (bits 31:6), then `base_hi` (7:0), `interval`, reset bits. Not confirmed, so the raw dwords are logged.

## What was implemented
All in `third_party/shadps4/src/video_core/amdgpu/liverpool.cpp` and `pm4_cmds.h`. Listed in
`third_party/shadps4/README.md`. `vk_rasterizer.cpp` is untouched: skipping happens before the rasterizer is called.

- Every type-3 packet in `ProcessGraphics` first goes through `SotcPredicationSkips(header)`. A skipped packet is
  stepped over whole (`NextPacket`), so nothing of it runs (no draw, no register write, no IB).
- `SOTC_PRED` (read once; default `run`):

  | Value | Behaviour |
  |---|---|
  | `run` | Ignore predication, execute everything (shadPS4 behaviour). Logging still happens. |
  | `skip` | While a predication is active, drop packets that have the header predicate bit. This is "the predicate always fails". |
  | `skipall` | While a predication is active, drop every draw/dispatch (`DRAW_*`, `DISPATCH_*`), predicate bit or not, plus any predicated packet. Use it if the game doesn't set the predicate bit. |
  | `eval` | Evaluate the predicate on the CPU when `SET_PREDICATION` is processed and drop predicated packets that fail it. ZPass reads the counter pairs (through `CopySparseMemory`, so it doesn't fault on tracked pages); Bool64/Bool32 read the value; PrimCount always reports "no overflow". Results that aren't ready or are unreadable count as "draw" (the CP would wait; we can't). |

  Because ZPASS_DONE is faked as always-visible, `eval` on ZPass predicates equals `run` for `draw_visible = 1`
  and equals `skip` for `draw_visible = 0`. A Bool predicate written by a GPU shader reads stale memory.
- `IT_GET_LOD_STATS` writes zeros to its destination: at most 4 KiB (the unit of `bu_size` isn't confirmed; a
  prefix is harmless), only if the address is non-zero and a GPU mapping, through `TryWriteBacking`.
  `SOTC_LOD_STATS=off` turns the write off.

## Log lines (Info, `Render`; visible with the default `SOTC_GPU_LOG`)
Logged for the first 32 occurrences, then at every power of two.
- `SOTC_PRED mode: Run|Skip|SkipAll|Eval` once, at the first `SET_PREDICATION` (absent if the game never sends one).
- `IT_SET_PREDICATION #N: op=ZPass addr=0x... draw_visible=1 hint=0 continue=false raw=[0x... 0x...]
  [eval: value= ready= pass=] | totals: clear= zpass= primcount= bool64= bool32= other= pred_pkts_active=
  pred_pkts_idle= skipped=`
- `predicated packet N (DrawIndexAuto), predication active|not active`: the first 8 packets with the header
  predicate bit, for each case.
- `SOTC_PRED: skipped DrawIndex2 (N so far), predicate addr=0x... props=0x...`
- `IT_GET_LOD_STATS #N: body=3 dw=[...] addr=0x... zeroed=true bytes=...`

The old `Unimplemented IT_SET_PREDICATION` / `Unimplemented IT_GET_LOD_STATS` warnings are gone.

## Phase 2: real support (design, not implemented)
Not implemented: it would gain nothing while the occlusion results are fake.
1. **Real occlusion results first.** Map `EVENT_WRITE` `ZPASS_DONE` begin/end to `vkCmdBeginQuery`/`EndQuery`
   (`VK_QUERY_TYPE_OCCLUSION`, a pool in the rasterizer).
   - Problem: Vulkan needs a query to begin and end inside the same render pass instance (or both outside).
     shadPS4 begins and ends dynamic rendering per render-target change, so a guest begin/end pair can span
     several host passes. Options: one query per host pass segment, summed with a compute shader; or end
     and resume the query at each `BeginRendering`/`EndRendering`, accumulating with
     `vkCmdCopyQueryPoolResults(…, VK_QUERY_RESULT_64_BIT | WAIT)` into a buffer.
   - Write the summed result as the guest's begin/end pairs (begin = 0|valid, end = count|valid) into the
     guest buffer through the buffer cache, so the CPU readers (`sceGnm` occlusion queries) see real data.
     This needs readbacks for the guest buffer (`Precise` is the default already).
2. **Conditional rendering.** Enable `VK_EXT_conditional_rendering` (instance/device feature in `vk_instance`).
   At `SET_PREDICATION`, run a tiny compute shader that reduces the predicate source (ZPass pairs, Bool64,
   Bool32, ORed over `continue` packets, polarity from `draw_visible`) into a 32-bit value in a scratch buffer.
   Then wrap each predicated draw in `vkCmdBeginConditionalRenderingEXT`/`End` on that buffer (or keep it open
   across the range, closing it at render-pass boundaries as the spec requires). Dispatches use the
   `conditionalRenderingEnable`-equivalent: the extension covers `vkCmdDispatch*` too.
3. Keep `SOTC_PRED=run` the default until the main session confirms the visible result is right.

## What the main session should test
1. Any run (e.g. `smoke.py --exe build-vk/sotc-runtime.exe --gpu vulkan --seconds 120`): grep the log for
   `IT_SET_PREDICATION` and `predicated packet`. This answers whether SotC uses predication at all, which op,
   which polarity, and whether its draws carry the predicate bit. Also check for `IT_GET_LOD_STATS`.
2. If it does, compare `SOTC_PRED=skip` (and `skipall` if `predicated packet` never appears) with the default
   on the DOF squares (#4) and the flashing shapes: burst shots during the logos and the intro DOF.
3. `SOTC_PRED=eval` only matters for Bool predicates (ZPass is faked always-visible).
