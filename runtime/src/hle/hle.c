/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hle.h"

#include <string.h>

#include "../log.h"

static const hle_library *const g_libraries[] = {
    &hle_kernel, &hle_kernel_mem, &hle_kernel_thr, &hle_kernel_file, &hle_posix, &hle_sysmodule,
};
#define LIBRARY_COUNT (sizeof(g_libraries) / sizeof(g_libraries[0]))

static _Thread_local int t_errno;

int *hle_errno(void) { return &t_errno; }

void hle_init(void) {
    unsigned total = 0;
    for (unsigned i = 0; i < LIBRARY_COUNT; i++)
        for (const hle_symbol *s = g_libraries[i]->symbols; s->name; s++) total++;
    log_info("hle: %u functions in %u tables", total, (unsigned)LIBRARY_COUNT);
}

static void *find_in(const hle_library *lib, const char *name) {
    for (const hle_symbol *s = lib->symbols; s->name; s++)
        if (strcmp(s->name, name) == 0) return s->addr;
    return NULL;
}

void *hle_lookup(const char *library, const char *name) {
    if (!name || !*name) return NULL;
    /* Same library first: libkernel and libScePosix share some names. */
    for (unsigned i = 0; i < LIBRARY_COUNT; i++)
        if (strcmp(g_libraries[i]->name, library) == 0) {
            void *p = find_in(g_libraries[i], name);
            if (p) return p;
        }
    /* libkernel's tables are split by topic; posix names also live in libkernel. */
    if (strcmp(library, "libkernel") == 0 || strcmp(library, "libScePosix") == 0)
        for (unsigned i = 0; i < LIBRARY_COUNT; i++) {
            const char *n = g_libraries[i]->name;
            if (strcmp(n, "libkernel") == 0 || strcmp(n, "libScePosix") == 0) {
                void *p = find_in(g_libraries[i], name);
                if (p) return p;
            }
        }
    return NULL;
}
