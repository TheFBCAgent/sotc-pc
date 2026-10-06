/* SPDX-License-Identifier: GPL-2.0-or-later */
/* First version: no window yet, the main loop only waits for the game.
 * Workstream GW replaces this with SDL3. */
#include "window.h"

#include "log.h"

int host_main_loop(plat_thread *guest_main) {
    plat_thread_join(guest_main);
    log_info("guest main thread returned");
    return 0;
}

void *window_sdl_handle(void) { return NULL; }
