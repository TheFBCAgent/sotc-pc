/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The host window, the process main thread's event loop, and the input
 * hand-off to libScePad. Owned by workstream GW (M3). */
#ifndef SOTC_WINDOW_H
#define SOTC_WINDOW_H

#include <stdint.h>

#include "platform.h"

/* Runs on the process main thread once the guest main thread has started.
 * Creates the SDL3 window, pumps events and feeds the pad, and returns the
 * process exit code once the game should stop: the guest main thread
 * returned (0), or the window was closed (0). The window is not destroyed on
 * return (the GPU may still present into it); process exit cleans it up.
 *
 * Environment:
 *   SOTC_WINDOW=WxH   window size in logical (DPI-scaled) units; default
 *                     1920x1080, shrunk to fit the display's usable area
 *   SOTC_HEADLESS=1   no window, no SDL: only wait for the guest main thread
 * No video driver (SDL_Init or window creation fails) behaves like
 * SOTC_HEADLESS=1, with a warning. */
int host_main_loop(plat_thread *guest_main);

/* The SDL_Window* the GPU presents into, or NULL when there is no window.
 *
 * Contract (for the GPU bridge, workstream GI):
 *  - Callable from any thread, any number of times, before or after
 *    host_main_loop starts. The guest main thread starts before the window
 *    exists, so this blocks until host_main_loop has either created the
 *    window or given up (headless, no video driver, creation failed), for at
 *    most WINDOW_HANDLE_TIMEOUT_MS. After that it returns immediately.
 *  - Returns NULL when headless, on failure or on timeout. The caller then
 *    runs without presenting (null backend or offscreen).
 *  - The window is created with SDL_WINDOW_VULKAN, so the caller may create
 *    a VkSurfaceKHR from it on its own thread (SDL_Vulkan_CreateSurface,
 *    SDL_Vulkan_GetInstanceExtensions and SDL_GetWindowSizeInPixels are fine
 *    off the main thread). Any other SDL video call (resize, fullscreen,
 *    destroy) belongs to the main thread: ask window.c instead.
 *  - The window lives until process exit.
 *  - Never call it from the main thread before host_main_loop: it would wait
 *    for the full timeout. */
#define WINDOW_HANDLE_TIMEOUT_MS 10000
void *window_sdl_handle(void);

/* ---- Pad input hand-off (window.c -> hle/pad.c) ---------------------------
 * A complete snapshot of the pad, in libScePad units. */
enum {
    PAD_AXIS_LEFT_X, PAD_AXIS_LEFT_Y, PAD_AXIS_RIGHT_X, PAD_AXIS_RIGHT_Y,
    PAD_AXIS_L2, PAD_AXIS_R2, PAD_AXIS_COUNT
};
/* ScePadButtonDataOffset bits. */
enum {
    PAD_BTN_L3 = 0x2, PAD_BTN_R3 = 0x4, PAD_BTN_OPTIONS = 0x8,
    PAD_BTN_UP = 0x10, PAD_BTN_RIGHT = 0x20, PAD_BTN_DOWN = 0x40, PAD_BTN_LEFT = 0x80,
    PAD_BTN_L2 = 0x100, PAD_BTN_R2 = 0x200, PAD_BTN_L1 = 0x400, PAD_BTN_R1 = 0x800,
    PAD_BTN_TRIANGLE = 0x1000, PAD_BTN_CIRCLE = 0x2000, PAD_BTN_CROSS = 0x4000,
    PAD_BTN_SQUARE = 0x8000, PAD_BTN_TOUCHPAD = 0x100000,
};
typedef struct pad_input {
    uint32_t buttons;               /* PAD_BTN_* */
    uint8_t axes[PAD_AXIS_COUNT];   /* sticks 0..255 (128 centered, y down), triggers 0..255 */
} pad_input;

/* Implemented in hle/pad.c; thread-safe. Records the snapshot as a new pad
 * sample (stamped with the process time) if it differs from the last one. */
void pad_input_update(const pad_input *in);

/* Implemented in window.c; thread-safe, cheap (guest threads call them).
 * Forwarded to the SDL gamepad, if any, on the main thread. */
void window_pad_rumble(uint8_t large_motor, uint8_t small_motor);
void window_pad_light(uint8_t r, uint8_t g, uint8_t b);

/* Implemented in window.c; thread-safe, cheap. Called once per completed
 * guest flip (hle/videoout.c, null GPU and sotcgpu paths alike); the main
 * thread shows the rate in the window title about once per second. */
void window_note_flip(void);

#endif
