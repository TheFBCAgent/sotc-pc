/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "tls.h"

#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "runtime.h"

#define TCB_SIZE 0x40
#define TLS_ALIGN 0x20

static uint64_t g_static_size;     /* bytes of static TLS below the TCB */
static uint32_t g_max_modid;
static _Thread_local guest_tcb *t_current;

/* libc hands us its heap (malloc/free) through _sceKernelRtldSetApplicationHeapAPI.
 * Like shadPS4 we use it for dynamic TLS blocks once it is available. */
typedef SYSV void *(*guest_malloc_fn)(uint64_t);
static guest_malloc_fn g_heap_malloc;

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

void tls_init(void) {
    const guest_module *main = &g_rt.modules[0];
    g_static_size = align_up(main->tls_memsz, TLS_ALIGN);
    for (uint32_t i = 0; i < g_rt.module_count; i++)
        if (g_rt.modules[i].tls_modid > g_max_modid) g_max_modid = g_rt.modules[i].tls_modid;
    log_info("tls: static block %llu bytes, %u modules",
             (unsigned long long)g_static_size, g_max_modid);
}

guest_tcb *tls_create(void *thread) {
    uint8_t *block = calloc(1, g_static_size + TCB_SIZE);
    guest_tcb *tcb = (guest_tcb *)(block + g_static_size);
    tcb->self = tcb;
    tcb->thread = thread;
    tcb->dtv = calloc(g_max_modid + 2, sizeof(dtv_entry));
    tcb->dtv[0].counter = 1;          /* generation */
    tcb->dtv[1].counter = g_max_modid; /* libkernel convention: number of entries */

    /* The main module's TLS lives in the static block, ending at the TCB. */
    const guest_module *main = &g_rt.modules[0];
    if (main->tls_memsz) {
        uint8_t *dest = (uint8_t *)tcb - main->tls_memsz;
        memcpy(dest, main->tls_image, main->tls_filesz);
        tcb->dtv[main->tls_modid + 1].pointer = dest;
    }
    return tcb;
}

void tls_destroy(guest_tcb *tcb) {
    /* Dynamic blocks come from the guest heap or calloc; leak them for now. */
    free(tcb->dtv);
    free((uint8_t *)tcb - g_static_size);
}

void tls_set_current(guest_tcb *tcb) {
    t_current = tcb;
    plat_set_guest_tcb(tcb);
}

guest_tcb *tls_current(void) { return t_current; }

SYSV void *ps4___tls_get_addr(tls_index *index) {
    guest_tcb *tcb = t_current;
    if (!tcb || index->modid == 0 || index->modid > g_max_modid)
        log_fatal("__tls_get_addr: bad module id %llu", (unsigned long long)index->modid);
    dtv_entry *slot = &tcb->dtv[index->modid + 1];
    if (!slot->pointer) {
        const guest_module *mod = NULL;
        for (uint32_t i = 0; i < g_rt.module_count; i++)
            if (g_rt.modules[i].tls_modid == index->modid) mod = &g_rt.modules[i];
        if (!mod) log_fatal("__tls_get_addr: no module %llu", (unsigned long long)index->modid);
        uint64_t size = mod->tls_memsz ? mod->tls_memsz : 1;
        uint8_t *block = g_heap_malloc ? g_heap_malloc(size) : malloc(size);
        memcpy(block, mod->tls_image, mod->tls_filesz);
        memset(block + mod->tls_filesz, 0, size - mod->tls_filesz);
        slot->pointer = block;
    }
    return slot->pointer + index->offset;
}

SYSV void ps4__sceKernelRtldSetApplicationHeapAPI(void *api[]) {
    /* api[0] is malloc in the libc heap API table. */
    g_heap_malloc = (guest_malloc_fn)api[0];
    log_debug("libc heap API registered (malloc at %p)", api[0]);
}
