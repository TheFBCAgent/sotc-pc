/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hle.h"

#include <string.h>

/* Filled by HLE_LIBRARY constructors before main() runs. */
static hle_library *g_libraries;

static _Thread_local int t_errno;

int *hle_errno(void) { return &t_errno; }

void hle_register(hle_library *lib) {
    /* Runs from static constructors: no logging, no allocation. */
    lib->next = g_libraries;
    g_libraries = lib;
}

void hle_init(void) {
    unsigned tables = 0, total = 0;
    for (const hle_library *lib = g_libraries; lib; lib = lib->next, tables++)
        for (const hle_symbol *s = lib->symbols; s->name; s++) total++;
    log_info("hle: %u functions in %u tables", total, tables);
}

static void *find_in(const hle_library *lib, const char *name) {
    for (const hle_symbol *s = lib->symbols; s->name; s++)
        if (strcmp(s->name, name) == 0) return s->addr;
    return NULL;
}

static bool is_kernel(const char *library) {
    return strcmp(library, "libkernel") == 0 || strcmp(library, "libScePosix") == 0;
}

void *hle_lookup(const char *library, const char *name) {
    if (!name || !*name) return NULL;
    /* Same library first: libkernel and libScePosix share some names. */
    for (const hle_library *lib = g_libraries; lib; lib = lib->next)
        if (strcmp(lib->name, library) == 0) {
            void *p = find_in(lib, name);
            if (p) return p;
        }
    /* posix names also live in libkernel, and the other way round. */
    if (is_kernel(library))
        for (const hle_library *lib = g_libraries; lib; lib = lib->next)
            if (is_kernel(lib->name)) {
                void *p = find_in(lib, name);
                if (p) return p;
            }
    return NULL;
}
