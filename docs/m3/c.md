# Step C: frame rate in the window title (M3)

The window title reads `Shadow of the Colossus — 30 fps (Vulkan)` (or `(null GPU)`), updated about once per
second with the guest's flip rate over the last interval.

## Where flips are counted
- `log_flip()` in `runtime/src/hle/videoout.c` calls `window_note_flip()`. Every completed flip passes through
  it:
  - null GPU: `vblank_tick()` completes queued flips on the vblank thread;
  - sotcgpu (Vulkan): `videoout_flip_done()`, called on the GPU bridge's event thread when sotcgpu reports
    the flip presented (`gpu_bridge.c`, `EV_FLIP`). EOP flips (SubmitAndFlip) are counted here too, once they
    actually complete, not when they are armed.
  
  So it counts the same flips as smoke.py's `videoout: flip N at T us` lines.
- `window_note_flip()` (`runtime/src/window.c`, declared in `window.h`) is one relaxed atomic increment
  (`g_flips`), safe from any thread.

## Title update
- `update_title()` runs in `host_main_loop`'s event loop on the main thread, which wakes at least every 8 ms.
- At most once per second it computes `(flips - last) / elapsed` and calls `SDL_SetWindowTitle`.
- Headless (no window) never reaches the loop, so the counter is just never read.
- The backend name comes from `gpu_bridge_active()`. Before `sceVideoOutOpen` initializes the bridge it reads
  "null GPU" (at 0 fps).

Built and tested only; not run (the main session verifies visually).
