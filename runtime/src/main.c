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
#ifndef _WIN32
#define _GNU_SOURCE /* dladdr */
#endif
#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
#include <dlfcn.h>
#include <string.h>
#include <sys/prctl.h>
#endif

#include "gpu_bridge.h"
#include "hle/hle.h"
#include "log.h"
#include "runtime.h"
#include "tls.h"
#include "window.h"

#ifndef _WIN32
/* Host code (the runtime, the Vulkan driver, libc): "libfoo.so+off (symbol+off)".
 * Not async-signal-safe, but the report ends in _Exit. */
static bool host_describe(uint64_t addr, char *buf, size_t n) {
    Dl_info di;
    if (!dladdr((void *)(uintptr_t)addr, &di) || !di.dli_fname) return false;
    const char *base = strrchr(di.dli_fname, '/');
    base = base ? base + 1 : di.dli_fname;
    uint64_t off = addr - (uint64_t)(uintptr_t)di.dli_fbase;
    if (di.dli_sname)
        snprintf(buf, n, "%s+%#llx (%s+%#llx)", base, (unsigned long long)off, di.dli_sname,
                 (unsigned long long)(addr - (uint64_t)(uintptr_t)di.dli_saddr));
    else snprintf(buf, n, "%s+%#llx", base, (unsigned long long)off);
    return true;
}
#endif

/* Called once a fault's report is printed, just before the process exits. Not inlined:
 * scripts/sotc.gdb breaks here, with the faulting frames above the signal frame. */
__attribute__((noinline)) void fault_reported(void) {
    __asm__ volatile("" ::: "memory");
}

static bool on_fault(plat_fault *f) {
    /* GPU page tracking write-protects watched pages; it goes first. */
    if (f->kind == PLAT_FAULT_ACCESS && gpu_bridge_on_fault(f)) return true;
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
#ifndef _WIN32
    char host[256];
    bool in_host = !loader_module_at((void *)(uintptr_t)f->rip) &&
                   host_describe(f->rip, host, sizeof(host));
    if (in_host) log_error("  host code: %s", host);
    char tname[17] = {0};
    prctl(PR_GET_NAME, tname);
    log_error("  thread '%s'", tname);
    if (!in_host && !loader_module_at((void *)(uintptr_t)f->rip)) {
        /* Neither guest nor a loaded library: JIT code or a bad jump; name its mapping. */
        FILE *mf = fopen("/proc/self/maps", "r");
        char line[256];
        while (mf && fgets(line, sizeof line, mf)) {
            unsigned long long lo, hi;
            if (sscanf(line, "%llx-%llx", &lo, &hi) == 2 && f->rip >= lo && f->rip < hi) {
                line[strcspn(line, "\n")] = 0;
                log_error("  rip mapping: %s (+%#llx)", line, (unsigned long long)(f->rip - lo));
                break;
            }
        }
        if (mf) fclose(mf);
    }
#endif
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
#ifndef _WIN32
            /* Faults in host code: also the host return addresses (a few). */
            else if (in_host && shown < 8 && host_describe(v, host, sizeof(host))) {
                log_error("  stack[%3d] host %s", i, host);
                shown++;
            }
#endif
        }
    }
    fflush(NULL);
    fault_reported();
    _Exit(3);
}

#ifdef SOTC_PGO_GENERATE
/* SOTC_PGO=generate (session 37): the game never exits cleanly (smoke kills it, and main
 * ends in _Exit), so write the profile every 30 s; the last write before the kill counts. */
#ifdef _WIN32
#include <direct.h>
#define pgo_mkdir(d) _mkdir(d)
#else
#include <sys/stat.h>
#define pgo_mkdir(d) mkdir(d, 0755)
#endif
int __llvm_profile_write_file(void);
void __llvm_profile_set_filename(const char *name);
static void pgo_writer(void *arg) {
    (void)arg;
    plat_thread_set_name("sotc:pgo");
    /* One file per run, rewritten whole each time (the counters are cumulative). */
    pgo_mkdir(SOTC_PGO_DIR);
    __llvm_profile_set_filename(SOTC_PGO_DIR "/sotc-%p.profraw");
    for (;;) {
        plat_sleep_us(30 * 1000000ull);
        if (__llvm_profile_write_file() != 0) log_warn("pgo: profile write failed");
    }
}
#endif

int main(int argc, char **argv) {
    log_init();
#ifdef SOTC_PGO_GENERATE
    plat_thread_detach(plat_thread_create(pgo_writer, NULL, 1 << 20));
#endif
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
