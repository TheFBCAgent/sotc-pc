/* SPDX-License-Identifier: GPL-2.0-or-later */
/* sotc-runtime: run Shadow of the Colossus (PS4) natively from a boot image.
 *
 * Usage: sotc-runtime [boot.img]
 * Environment:
 *   SOTC_GAME_DIR  game folder (contains eboot.bin), mounted as /app0 (required)
 *   SOTC_USER_DIR  writable data folder (default ./user)
 *   SOTC_LOG       log level 0-4 (default 2)
 *   SOTC_STRICT=1  stop at the first unimplemented import
 *   SOTC_NEO=1     report PS4 Pro hardware */
#include <stdio.h>
#include <stdlib.h>

#include "gpu_bridge.h"
#include "hle/hle.h"
#include "log.h"
#include "runtime.h"
#include "tls.h"
#include "window.h"

static bool on_fault(plat_fault *f) {
    /* GPU page tracking write-protects watched pages; it goes first. */
    if (f->kind == PLAT_FAULT_ACCESS && gpu_bridge_on_fault(f->addr, f->is_write, f->rip)) return true;
    if (sse4a_handle_fault(f)) return true;
    /* An access that raced with a split of its mapping: retry it, a few
     * times at most for the same instruction. */
    static _Thread_local uint64_t last_rip;
    static _Thread_local int retries;
    if (f->kind == PLAT_FAULT_ACCESS) {
        retries = f->rip == last_rip ? retries + 1 : 0;
        last_rip = f->rip;
        if (retries < 8 && hle_memory_fault_retry(f->addr)) return true;
    }

    static const char *kinds[] = {"access violation", "illegal instruction", "fatal exception"};
    char where[96], what[96];
    log_error("%s at %s", kinds[f->kind], loader_describe((void *)(uintptr_t)f->rip, where,
                                                           sizeof(where)));
    if (f->kind == PLAT_FAULT_ACCESS) {
        char page[128];
        gpu_bridge_describe(f->addr, page, sizeof(page));
        log_error("  faulting address %#llx (%s; %s)", (unsigned long long)f->addr,
                  f->is_write ? "write" : "read", page);
    }

    /* Poor man's backtrace: stack words that point into guest code. */
    if (f->rsp) {
        int shown = 0;
        for (int i = 0; i < 256 && shown < 12; i++) {
            if (!plat_is_readable(&f->rsp[i], sizeof(uint64_t))) break;
            uint64_t v = f->rsp[i];
            const guest_module *m = loader_module_at((void *)(uintptr_t)v);
            if (m) {
                log_error("  stack[%3d] %s", i, loader_describe((void *)(uintptr_t)v, what,
                                                                 sizeof(what)));
                shown++;
            }
        }
    }
    fflush(NULL);
    _Exit(3);
}

int main(int argc, char **argv) {
    log_init();
    const char *image = argc > 1 ? argv[1] : "build/game/boot.img";
    g_rt.game_dir = getenv("SOTC_GAME_DIR");
    const char *strict = getenv("SOTC_STRICT");
    g_rt.strict = strict && *strict == '1';
    if (!g_rt.game_dir) log_fatal("set SOTC_GAME_DIR to the game folder (the one with eboot.bin)");

    log_info("sotc-runtime: boot image %s, game %s", image, g_rt.game_dir);
    if (!plat_tls_init()) return 1;
    plat_install_fault_handler(on_fault);

    hle_init();
    hle_kernel_init();
    hle_memory_init();
    hle_thread_init();
    hle_file_init(g_rt.game_dir);
    if (!loader_load(image)) return 1;
    tls_init();
    /* The guest runs on its own thread; the process main thread belongs to
     * the window (SDL wants its events pumped there). */
    int code = host_main_loop(hle_start_main());
    /* _Exit, not exit: guest and GPU threads are still running, and atexit
     * handlers / C++ static destructors (shadPS4's singletons, the decoder)
     * would tear state down under them (faults at window close). */
    fflush(NULL);
    _Exit(code);
}
