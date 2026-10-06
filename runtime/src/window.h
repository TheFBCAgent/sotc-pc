/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The host window and the process main thread's event loop.
 * Owned by workstream GW (M3). */
#ifndef SOTC_WINDOW_H
#define SOTC_WINDOW_H

#include "platform.h"

/* Runs on the process main thread once the guest main thread has started.
 * Returns the process exit code once the game should stop (guest main thread
 * returned, or the window was closed). */
int host_main_loop(plat_thread *guest_main);

/* The SDL_Window* the GPU presents into, or NULL when there is no window. */
void *window_sdl_handle(void);

#endif
