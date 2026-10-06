/* SPDX-License-Identifier: GPL-2.0-or-later */
/* High-level emulation of PS4 system libraries: only what SotC imports. */
#ifndef SOTC_HLE_H
#define SOTC_HLE_H

#include <stdint.h>

#include "../log.h"
#include "../platform.h"

typedef struct hle_symbol {
    const char *name;   /* Sony symbol name, as in the boot image import table */
    void *addr;         /* host function (SYSV) or data object */
} hle_symbol;

typedef struct hle_library {
    const char *name;   /* e.g. "libkernel", "libScePosix" */
    const hle_symbol *symbols;
    struct hle_library *next;
} hle_library;

/* Host implementations are named ps4_<SonyName>. */
#define HLE(n) {#n, (void *)ps4_##n}
#define HLE_AS(sony, fn) {sony, (void *)(fn)}
#define HLE_END {0, 0}

/* Define and self-register a table of functions for one PS4 library. Each
 * source file in hle/ declares its own tables, so adding a library means
 * adding a file: no edits to hle.c or CMakeLists.txt (sources are globbed).
 *
 *   HLE_LIBRARY(evflag, "libkernel",
 *       HLE(sceKernelCreateEventFlag),
 *       HLE(sceKernelWaitEventFlag),
 *   )
 */
void hle_register(hle_library *lib);
#define HLE_LIBRARY(var, libname, ...)                                   \
    static const hle_symbol var##_symbols[] = {__VA_ARGS__ HLE_END};     \
    static hle_library var##_library = {libname, var##_symbols, 0};      \
    __attribute__((constructor)) static void var##_register(void) {      \
        hle_register(&var##_library);                                    \
    }

/* An intentional "succeed and do nothing" service: logs once, returns 0.
 * Unlike unresolved imports, these show as "stub" (not "unimplemented") in
 * the smoke report. Use HLE(name) in the table as usual. */
#define HLE_STUB_OK(n)                                                   \
    static SYSV int32_t ps4_##n(void) {                                  \
        static int logged;                                               \
        if (!logged) {                                                   \
            logged = 1;                                                  \
            log_info("stub %s -> 0", #n);                                \
        }                                                                \
        return 0;                                                        \
    }

/* PS4 kernel error codes: 0x80020000 | errno. */
#define SCE_OK 0
#define SCE_KERNEL_ERROR(e) ((int32_t)(0x80020000u | (uint32_t)(e)))
enum {
    PS4_EPERM = 1, PS4_ENOENT = 2, PS4_ESRCH = 3, PS4_EINTR = 4, PS4_EIO = 5, PS4_EBADF = 9,
    PS4_EAGAIN = 35, PS4_ENOMEM = 12, PS4_EACCES = 13, PS4_EFAULT = 14, PS4_EBUSY = 16,
    PS4_EEXIST = 17, PS4_ENOTDIR = 20, PS4_EISDIR = 21, PS4_EINVAL = 22, PS4_EMFILE = 24,
    PS4_ENOSPC = 28, PS4_ENOSYS = 78, PS4_ETIMEDOUT = 60, PS4_EDEADLK = 11,
    PS4_ENAMETOOLONG = 63, PS4_ENOTEMPTY = 66,
};

/* Per-thread errno for __error(). */
int *hle_errno(void);

/* Registry. */
void hle_init(void);
void *hle_lookup(const char *library, const char *name);

/* Shared helpers. */
void hle_kernel_init(void);
void hle_memory_init(void);
/* Fault handler hook: true if a faulting arena access should be retried. */
bool hle_memory_fault_retry(uint64_t addr);
/* Read from a guest file descriptor at an offset (for mmap). Returns bytes or -1. */
int64_t hle_file_pread(int32_t fd, void *buf, uint64_t len, int64_t offset);
void hle_file_init(const char *game_dir);
/* Map guest paths under `guest_prefix` (e.g. "/savedata0") to `host_dir`.
 * A later mount of the same prefix replaces it. Returns false if full. */
bool hle_file_mount(const char *guest_prefix, const char *host_dir);
void hle_file_unmount(const char *guest_prefix);
void hle_thread_init(void);
/* Start the guest main thread at `entry` and never return. */
__attribute__((noreturn)) void hle_run_main(void);

#endif
