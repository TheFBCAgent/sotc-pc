/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libkernel event queues (sceKernel*Equeue, user events) and the host-side
 * equeue.h API used by VideoOut and GnmDriver.
 *
 * Semantics ported from shadPS4 (GPL-2.0-or-later),
 * src/core/libraries/kernel/equeue.{h,cpp}.
 *
 * SceKernelEqueue is a pointer-sized handle: the guest holds the address of a
 * host `equeue`. Live queues are kept in a registry so a stale handle (or a
 * trigger racing a delete) is rejected instead of touching freed memory.
 *
 * Locking: one global lock protects the registry and every queue. Event
 * traffic is tiny (a handful of queues, a few events each), so this keeps
 * equeue_trigger trivially safe from any host thread (vblank thread, PM4
 * executor). Each queue has its own condition variable. No guest code ever
 * runs under the lock. */
#include <stdlib.h>
#include <string.h>

#include "equeue.h"
#include "hle.h"
#include "caller_trace.h"
#include "../prof.h"

typedef struct eq_event {
    ps4_kevent ev;
    bool triggered;
} eq_event;

struct equeue {
    equeue *next;       /* registry link */
    plat_cond *cond;    /* waiters and the deleter */
    eq_event *events;
    int count, cap;
    int nwaiters;
    bool deleted;
    char name[33];
};

static plat_mutex *g_lock;
static equeue *g_queues;

__attribute__((constructor)) static void equeue_lock_init(void) { g_lock = plat_mutex_new(); }

/* Caller holds g_lock. */
static bool eq_live(const equeue *eq) {
    if (!eq) return false;
    for (equeue *q = g_queues; q; q = q->next)
        if (q == eq) return !q->deleted;
    return false;
}

static eq_event *eq_find(equeue *eq, uint64_t ident, int16_t filter) {
    for (int i = 0; i < eq->count; i++)
        if (eq->events[i].ev.ident == ident && eq->events[i].ev.filter == filter)
            return &eq->events[i];
    return NULL;
}

static bool eq_add(equeue *eq, uint64_t ident, int16_t filter, uint16_t flags, void *udata) {
    eq_event *e = eq_find(eq, ident, filter);
    if (e) { /* re-adding keeps the trigger state and data, updates udata */
        e->ev.udata = udata;
        return true;
    }
    if (eq->count == eq->cap) {
        int cap = eq->cap ? eq->cap * 2 : 8;
        eq_event *n = realloc(eq->events, (size_t)cap * sizeof(*n));
        if (!n) return false;
        eq->events = n;
        eq->cap = cap;
    }
    e = &eq->events[eq->count++];
    memset(e, 0, sizeof(*e));
    e->ev.ident = ident;
    e->ev.filter = filter;
    e->ev.flags = flags & (uint16_t)~EV_ADD;
    /* The kernel adds EV_CLEAR to every filter except user events (which
     * choose: AddUserEvent is level, AddUserEventEdge is edge). */
    if (filter != EVFILT_USER) e->ev.flags |= EV_CLEAR;
    e->ev.udata = udata;
    return true;
}

static bool eq_remove(equeue *eq, uint64_t ident, int16_t filter) {
    eq_event *e = eq_find(eq, ident, filter);
    if (!e) return false;
    int i = (int)(e - eq->events);
    memmove(&eq->events[i], &eq->events[i + 1], (size_t)(eq->count - i - 1) * sizeof(*e));
    eq->count--;
    return true;
}

/* VideoOut: `hint` is event_id | video_id << 8 | flip_arg << 16 (as shadPS4's
 * driver passes it). The event data becomes time:12 | count:4 | flip_arg:48,
 * where count is the number of triggers since the guest last consumed it. */
static void eq_trigger_display(eq_event *e, uint64_t hint) {
    if ((hint & 0xff) == e->ev.ident && e->ev.ident != 0xfe) {
        uint64_t count = (e->ev.data >> 12) & 0xf;
        if (count != 0xf) count++;
        e->ev.data = (plat_tsc() & 0xfff) | (count << 12) | (hint & 0xffffffffffff0000ull);
    } else {
        e->ev.data = hint;
    }
}

static bool eq_trigger_locked(equeue *eq, uint64_t ident, int16_t filter, uint64_t data) {
    eq_event *e = eq_find(eq, ident, filter);
    if (!e) return false;
    switch (filter) {
    case EVFILT_VIDEO_OUT: eq_trigger_display(e, data); break;
    case EVFILT_USER: e->ev.udata = (void *)(uintptr_t)data; break;
    case EVFILT_TIMER:
    case EVFILT_HRTIMER: e->ev.data++; break;
    default: e->ev.data = data; break;
    }
    e->triggered = true;
    plat_cond_broadcast(eq->cond);
    return true;
}

/* Copy up to `num` triggered events out; apply EV_CLEAR / EV_ONESHOT. */
static int eq_collect(equeue *eq, ps4_kevent *out, int num) {
    int n = 0;
    for (int i = 0; i < eq->count && n < num;) {
        eq_event *e = &eq->events[i];
        if (!e->triggered) {
            i++;
            continue;
        }
        out[n++] = e->ev;
        if (e->ev.flags & EV_CLEAR) {
            e->triggered = false;
            e->ev.fflags = 0;
            e->ev.data = 0;
        }
        if (e->ev.flags & EV_ONESHOT)
            eq_remove(eq, e->ev.ident, e->ev.filter);
        else
            i++;
    }
    return n;
}

/* ---- host API (equeue.h) ------------------------------------------------ */

bool equeue_add_event(equeue *eq, uint64_t ident, int16_t filter, uint16_t flags, void *udata) {
    plat_mutex_lock(g_lock);
    bool ok = eq_live(eq) && eq_add(eq, ident, filter, flags, udata);
    plat_mutex_unlock(g_lock);
    return ok;
}

bool equeue_remove_event(equeue *eq, uint64_t ident, int16_t filter) {
    plat_mutex_lock(g_lock);
    bool ok = eq_live(eq) && eq_remove(eq, ident, filter);
    plat_mutex_unlock(g_lock);
    return ok;
}

void equeue_trigger(equeue *eq, uint64_t ident, int16_t filter, uint64_t data) {
    plat_mutex_lock(g_lock);
    if (eq_live(eq)) eq_trigger_locked(eq, ident, filter, data);
    plat_mutex_unlock(g_lock);
}

/* ---- guest API ---------------------------------------------------------- */

static SYSV int32_t ps4_sceKernelCreateEqueue(equeue **out, const char *name) {
    if (!out || !name) return SCE_KERNEL_ERROR(PS4_EINVAL);
    if (strlen(name) > 32) return SCE_KERNEL_ERROR(PS4_ENAMETOOLONG);
    equeue *eq = calloc(1, sizeof(*eq));
    if (!eq) return SCE_KERNEL_ERROR(PS4_ENOMEM);
    eq->cond = plat_cond_new();
    strcpy(eq->name, name);
    plat_mutex_lock(g_lock);
    eq->next = g_queues;
    g_queues = eq;
    plat_mutex_unlock(g_lock);
    *out = eq;
    log_debug("equeue create '%s' -> %p", name, (void *)eq);
    return SCE_OK;
}

static SYSV int32_t ps4_sceKernelDeleteEqueue(equeue *eq) {
    plat_mutex_lock(g_lock);
    if (!eq_live(eq)) {
        plat_mutex_unlock(g_lock);
        return SCE_KERNEL_ERROR(PS4_EBADF);
    }
    eq->deleted = true;
    plat_cond_broadcast(eq->cond);
    while (eq->nwaiters > 0) plat_cond_wait(eq->cond, g_lock, UINT64_MAX);
    for (equeue **p = &g_queues; *p; p = &(*p)->next) {
        if (*p == eq) {
            *p = eq->next;
            break;
        }
    }
    plat_mutex_unlock(g_lock);
    plat_cond_free(eq->cond);
    free(eq->events);
    free(eq);
    return SCE_OK;
}

/* timo: NULL waits forever; *timo == 0 polls. */
static SYSV int32_t ps4_sceKernelWaitEqueue(equeue *eq, ps4_kevent *ev, int32_t num, int32_t *out,
                                            uint32_t *timo) {
    CALLER_TRACE("sceKernelWaitEqueue");
    plat_mutex_lock(g_lock);
    if (!eq_live(eq)) {
        plat_mutex_unlock(g_lock);
        return SCE_KERNEL_ERROR(PS4_EBADF);
    }
    if (!ev) {
        plat_mutex_unlock(g_lock);
        return SCE_KERNEL_ERROR(PS4_EFAULT);
    }
    if (num < 1) {
        plat_mutex_unlock(g_lock);
        if (out) *out = 0;
        return SCE_KERNEL_ERROR(PS4_EINVAL);
    }
    uint64_t deadline = timo ? plat_monotonic_ns() + (uint64_t)*timo * 1000 : 0;
    int n = 0;
    eq->nwaiters++;
    PROF_ZONE_IF(z, "guest equeue wait", !timo || *timo != 0);
    PROF_ZONE_TEXT(z, eq->name);
    for (;;) {
        n = eq_collect(eq, ev, num);
        if (n > 0 || eq->deleted) break;
        if (!timo) {
            plat_cond_wait(eq->cond, g_lock, UINT64_MAX);
            continue;
        }
        uint64_t now = plat_monotonic_ns();
        if (now >= deadline) break;
        plat_cond_wait(eq->cond, g_lock, (deadline - now + 999) / 1000);
    }
    PROF_ZONE_END(z);
    eq->nwaiters--;
    bool deleted = eq->deleted;
    if (deleted) plat_cond_broadcast(eq->cond); /* let the deleter proceed */
    plat_mutex_unlock(g_lock);
    if (out) *out = n;
    if (n > 0) return SCE_OK;
    return deleted ? SCE_KERNEL_ERROR(PS4_EBADF) : SCE_KERNEL_ERROR(PS4_ETIMEDOUT);
}

static int32_t user_event_add(equeue *eq, int32_t id, uint16_t flags) {
    plat_mutex_lock(g_lock);
    int32_t rc = !eq_live(eq) ? SCE_KERNEL_ERROR(PS4_EBADF)
                 : eq_add(eq, (uint64_t)(int64_t)id, EVFILT_USER, flags, NULL)
                     ? SCE_OK
                     : SCE_KERNEL_ERROR(PS4_ENOMEM);
    plat_mutex_unlock(g_lock);
    return rc;
}

static SYSV int32_t ps4_sceKernelAddUserEvent(equeue *eq, int32_t id) {
    return user_event_add(eq, id, EV_ADD);
}

static SYSV int32_t ps4_sceKernelAddUserEventEdge(equeue *eq, int32_t id) {
    return user_event_add(eq, id, EV_ADD | EV_CLEAR);
}

static SYSV int32_t ps4_sceKernelTriggerUserEvent(equeue *eq, int32_t id, void *udata) {
    plat_mutex_lock(g_lock);
    int32_t rc = !eq_live(eq) ? SCE_KERNEL_ERROR(PS4_EBADF)
                 : eq_trigger_locked(eq, (uint64_t)(int64_t)id, EVFILT_USER,
                                     (uint64_t)(uintptr_t)udata)
                     ? SCE_OK
                     : SCE_KERNEL_ERROR(PS4_ENOENT);
    plat_mutex_unlock(g_lock);
    return rc;
}

static SYSV int32_t ps4_sceKernelDeleteUserEvent(equeue *eq, int32_t id) {
    plat_mutex_lock(g_lock);
    int32_t rc = !eq_live(eq) ? SCE_KERNEL_ERROR(PS4_EBADF)
                 : eq_remove(eq, (uint64_t)(int64_t)id, EVFILT_USER)
                     ? SCE_OK
                     : SCE_KERNEL_ERROR(PS4_ENOENT);
    plat_mutex_unlock(g_lock);
    return rc;
}

/* Event accessors: plain field reads of the guest's SceKernelEvent. */
static SYSV int32_t ps4_sceKernelGetEventFilter(const ps4_kevent *ev) { return ev->filter; }
static SYSV uint64_t ps4_sceKernelGetEventId(const ps4_kevent *ev) { return ev->ident; }
static SYSV uint64_t ps4_sceKernelGetEventData(const ps4_kevent *ev) { return ev->data; }
static SYSV uint32_t ps4_sceKernelGetEventFflags(const ps4_kevent *ev) { return ev->fflags; }
static SYSV void *ps4_sceKernelGetEventUserData(const ps4_kevent *ev) { return ev->udata; }
static SYSV int32_t ps4_sceKernelGetEventError(const ps4_kevent *ev) {
    return (ev->flags & 0x4000) ? (int32_t)ev->data : 0; /* EV_ERROR */
}

HLE_LIBRARY(hle_equeue, "libkernel",
    HLE(sceKernelCreateEqueue),
    HLE(sceKernelDeleteEqueue),
    HLE(sceKernelWaitEqueue),
    HLE(sceKernelAddUserEvent),
    HLE(sceKernelAddUserEventEdge),
    HLE(sceKernelTriggerUserEvent),
    HLE(sceKernelDeleteUserEvent),
    HLE(sceKernelGetEventFilter),
    HLE(sceKernelGetEventId),
    HLE(sceKernelGetEventData),
    HLE(sceKernelGetEventFflags),
    HLE(sceKernelGetEventUserData),
    HLE(sceKernelGetEventError),
)
