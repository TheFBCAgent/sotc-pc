/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Guest thread-local storage, laid out like shadPS4's (src/core/tls.h). */
#ifndef SOTC_TLS_H
#define SOTC_TLS_H

#include <stdint.h>

#include "platform.h"

typedef union dtv_entry {
    uint64_t counter;
    uint8_t *pointer;
} dtv_entry;

/* x86-64 variant II: the static TLS block sits just below the TCB. */
typedef struct guest_tcb {
    struct guest_tcb *self;    /* fs:[0] */
    dtv_entry *dtv;
    void *thread;              /* struct guest_thread * */
    void *spare[2];
    uint64_t canary;
    void *fiber;
} guest_tcb;

typedef struct tls_index {
    uint64_t modid;
    uint64_t offset;
} tls_index;

void tls_init(void);
guest_tcb *tls_create(void *thread);
void tls_destroy(guest_tcb *tcb);
/* Make `tcb` current for guest code on this host thread. */
void tls_set_current(guest_tcb *tcb);
guest_tcb *tls_current(void);

/* Guest-callable. */
SYSV void *ps4___tls_get_addr(tls_index *index);
SYSV void ps4__sceKernelRtldSetApplicationHeapAPI(void *api[]);

#endif
