# GW: window and input

Files: `runtime/src/window.c`, `window.h`, `runtime/src/hle/pad.c`, `runtime/tests/test_window.c`.

## host_main_loop
Runs on the process main thread. SDL3 window 1920x1080 logical (shrunk to the usable desktop if larger),
resizable, `SDL_WINDOW_VULKAN`, title "Shadow of the Colossus". `SOTC_WINDOW=WxH` overrides.
`SOTC_HEADLESS=1`, or no video driver / window creation failure: no window, just waits for the guest.
Returns 0 when the window is closed or the guest main thread returns. The window is not destroyed on return.
SDL's 1 ms timer-resolution hint is disabled (frame pacing is workstream P's).
A watcher thread joins the guest main thread and wakes the event loop; the loop also wakes every 8 ms to sample input.
F11 toggles fullscreen.

## window_sdl_handle() contract
Callable from any thread at any time. Blocks until the window is created or creation failed/headless, at most
`WINDOW_HANDLE_TIMEOUT_MS` (10 s), then returns the `SDL_Window*` or NULL (headless, failure, timeout, or a
non-Vulkan fallback window). Safe SDL calls off the main thread: SDL_Vulkan_*, SDL_GetWindowSizeInPixels.
Do not call it from the main thread before host_main_loop.

## Pad
`pad_input_update()` (pad.c, thread-safe spinlock) receives snapshots from the main thread. Semantics follow
shadPS4: each change is a sample stamped with sceKernelGetProcessTime, queue of 64; `scePadRead(num==1)` and
`scePadReadState` return the current state; `num>1` drains the queue (may return 0); `scePadOpen` clears the queue
and queues the current state. Motion is neutral, no touches. Rumble and light bar go to the SDL gamepad
(`window_pad_rumble`, `window_pad_light`).

Keyboard (added to any gamepad; scancodes): left stick WASD, right stick IJKL, L1 Q, R1 U, L2 E, R2 O,
L3 X, R3 M, Triangle C or Num8, Circle B or Num6, Cross N or Num2, Square V or Num4, D-pad arrows,
Options Enter, Touchpad click Space, F11 fullscreen. Gamepad: SDL standard mapping, dead zone 5/127.

## Results
ctest: 12/12 pass (test_window: pad, headless, window, close). Smoke 60 s: still running, 600 frames
(~10/s, same as baseline), 0 unimplemented, no errors.

## Open issues
- Keyboard/gamepad were not exercised by hand (no interactive session); covered by the unit tests only.
- Linux build untested.
