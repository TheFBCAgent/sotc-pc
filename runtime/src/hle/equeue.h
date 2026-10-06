/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Kernel event queues (kqueue-style), as other libraries use them.
 *
 * The guest's SceKernelEqueue is a pointer to `equeue`. Workstream A
 * implements this in equeue.c together with the guest functions
 * (sceKernelCreateEqueue, sceKernelWaitEqueue, ...). Until then
 * equeue_stub.c provides no-op versions; A deletes it. VideoOut and GnmDriver
 * use only this interface. */
#ifndef SOTC_EQUEUE_H
#define SOTC_EQUEUE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct equeue equeue;

/* SceKernelEvent, 32 bytes. */
typedef struct ps4_kevent {
    uint64_t ident;
    int16_t filter;
    uint16_t flags;
    uint32_t fflags;
    uint64_t data;
    void *udata;
} ps4_kevent;
_Static_assert(sizeof(ps4_kevent) == 32, "ps4_kevent layout");

enum {
    EVFILT_READ = -1,
    EVFILT_WRITE = -2,
    EVFILT_TIMER = -7,
    EVFILT_USER = -11,
    EVFILT_VIDEO_OUT = -13,
    EVFILT_GRAPHICS_CORE = -14,
    EVFILT_HRTIMER = -15,
};

enum {
    EV_ADD = 0x1, EV_DELETE = 0x2, EV_ENABLE = 0x4, EV_DISABLE = 0x8,
    EV_ONESHOT = 0x10, EV_CLEAR = 0x20,
};

/* Register an event (ident, filter) on `eq`. `udata` is returned to the guest
 * with the event. Re-adding an existing event updates its udata. */
bool equeue_add_event(equeue *eq, uint64_t ident, int16_t filter, uint16_t flags, void *udata);
/* Remove an event; false if it was not registered. */
bool equeue_remove_event(equeue *eq, uint64_t ident, int16_t filter);
/* Mark (ident, filter) triggered with `data` and wake waiters. No-op if the
 * event is not registered on `eq`. Safe to call from any host thread. */
void equeue_trigger(equeue *eq, uint64_t ident, int16_t filter, uint64_t data);

#endif
