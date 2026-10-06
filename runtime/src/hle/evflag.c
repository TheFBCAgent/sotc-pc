/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libkernel event flags (sceKernel*EventFlag).
 *
 * Semantics ported from shadPS4 (GPL-2.0-or-later),
 * src/core/libraries/kernel/threads/event_flag.cpp.
 *
 * SceKernelEventFlag is a pointer-sized handle: we hand the guest the address
 * of a host `evflag`. Each flag has its own lock and condition variable; every
 * waiter sits on a FIFO list with its own pattern and mode, and Set() decides
 * which waiters are satisfied (applying their clear mode in queue order), so
 * the waiter that wins a CLEAR_ALL race is deterministic. */
#include <stdlib.h>
#include <string.h>

#include "hle.h"

/* FreeBSD ECANCELED; not in hle.h yet (see docs/m2/a.md). */
#define PS4_ECANCELED_ 85

enum {
    EVF_ATTR_TH_FIFO = 0x01,
    EVF_ATTR_TH_PRIO = 0x02,
    EVF_ATTR_SINGLE = 0x10,
    EVF_ATTR_MULTI = 0x20,

    EVF_WAITMODE_AND = 0x01,
    EVF_WAITMODE_OR = 0x02,
    EVF_WAITMODE_CLEAR_ALL = 0x10,
    EVF_WAITMODE_CLEAR_PAT = 0x20,
};

typedef struct evf_waiter {
    struct evf_waiter *next;
    uint64_t bits;
    uint32_t mode;      /* EVF_WAITMODE_* */
    uint64_t result;
    bool ready, canceled, deleted;
} evf_waiter;

typedef struct evflag {
    plat_mutex *lock;
    plat_cond *cond;      /* waiters and the deleter wait here (broadcast) */
    evf_waiter *waiters;  /* FIFO */
    int nwaiters;
    uint64_t bits;
    bool multi;
    bool deleted;
    char name[32];
} evflag;

static bool evf_met(uint64_t cur, uint64_t bits, uint32_t mode) {
    return (mode & EVF_WAITMODE_AND) ? (cur & bits) == bits : (cur & bits) != 0;
}

static void evf_clear(evflag *ef, uint64_t bits, uint32_t mode) {
    if (mode & EVF_WAITMODE_CLEAR_ALL)
        ef->bits = 0;
    else if (mode & EVF_WAITMODE_CLEAR_PAT)
        ef->bits &= ~bits;
}

static void evf_unlink(evflag *ef, evf_waiter *w) {
    for (evf_waiter **p = &ef->waiters; *p; p = &(*p)->next) {
        if (*p == w) {
            *p = w->next;
            ef->nwaiters--;
            return;
        }
    }
}

/* Validate a wait mode: exactly one of AND/OR, at most one clear mode. */
static bool evf_mode_ok(uint32_t mode) {
    uint32_t wait = mode & 0xf, clear = mode & 0xf0;
    if (wait != EVF_WAITMODE_AND && wait != EVF_WAITMODE_OR) return false;
    if (clear != 0 && clear != EVF_WAITMODE_CLEAR_ALL && clear != EVF_WAITMODE_CLEAR_PAT)
        return false;
    return (mode & ~0xffu) == 0;
}

static SYSV int32_t ps4_sceKernelCreateEventFlag(evflag **out, const char *name, uint32_t attr,
                                                 uint64_t init, const void *opt) {
    if (!out || !name) return SCE_KERNEL_ERROR(PS4_EINVAL);
    if (opt || (attr & ~0x33u) || (attr & 0x3) == 0x3 || (attr & 0x30) == 0x30)
        return SCE_KERNEL_ERROR(PS4_EINVAL);
    if (strlen(name) >= 32) return SCE_KERNEL_ERROR(PS4_ENAMETOOLONG);
    evflag *ef = calloc(1, sizeof(*ef));
    if (!ef) return SCE_KERNEL_ERROR(PS4_ENOMEM);
    ef->lock = plat_mutex_new();
    ef->cond = plat_cond_new();
    ef->bits = init;
    ef->multi = (attr & EVF_ATTR_MULTI) != 0; /* default is single-waiter */
    strcpy(ef->name, name);
    *out = ef;
    log_debug("evflag create '%s' attr %#x init %#llx -> %p", name, attr,
              (unsigned long long)init, (void *)ef);
    return SCE_OK;
}

static SYSV int32_t ps4_sceKernelDeleteEventFlag(evflag *ef) {
    if (!ef) return SCE_KERNEL_ERROR(PS4_ESRCH);
    plat_mutex_lock(ef->lock);
    if (ef->deleted) {
        plat_mutex_unlock(ef->lock);
        return SCE_KERNEL_ERROR(PS4_ESRCH);
    }
    ef->deleted = true;
    for (evf_waiter *w = ef->waiters; w; w = w->next) w->deleted = true;
    plat_cond_broadcast(ef->cond);
    while (ef->nwaiters > 0) plat_cond_wait(ef->cond, ef->lock, UINT64_MAX);
    plat_mutex_unlock(ef->lock);
    plat_cond_free(ef->cond);
    plat_mutex_free(ef->lock);
    free(ef);
    return SCE_OK;
}

/* Common wait. timeout_us: NULL waits forever; *timeout_us == 0 polls. On
 * return *timeout_us holds the time left (0 on timeout). */
static int32_t evf_wait(evflag *ef, uint64_t bits, uint32_t mode, uint64_t *result,
                        uint32_t *timeout_us) {
    if (!ef) return SCE_KERNEL_ERROR(PS4_ESRCH);
    if (bits == 0 || !evf_mode_ok(mode)) return SCE_KERNEL_ERROR(PS4_EINVAL);

    plat_mutex_lock(ef->lock);
    if (ef->deleted) {
        plat_mutex_unlock(ef->lock);
        return SCE_KERNEL_ERROR(PS4_EACCES);
    }
    if (!ef->multi && ef->nwaiters > 0) {
        plat_mutex_unlock(ef->lock);
        return SCE_KERNEL_ERROR(PS4_EPERM);
    }
    if (evf_met(ef->bits, bits, mode)) {
        if (result) *result = ef->bits;
        evf_clear(ef, bits, mode);
        plat_mutex_unlock(ef->lock);
        return SCE_OK;
    }
    if (timeout_us && *timeout_us == 0) {
        if (result) *result = ef->bits;
        plat_mutex_unlock(ef->lock);
        return SCE_KERNEL_ERROR(PS4_ETIMEDOUT);
    }

    evf_waiter w = {.bits = bits, .mode = mode};
    evf_waiter **tail = &ef->waiters;
    while (*tail) tail = &(*tail)->next;
    *tail = &w;
    ef->nwaiters++;

    uint64_t deadline = timeout_us ? plat_monotonic_ns() + (uint64_t)*timeout_us * 1000 : 0;
    int32_t rc = SCE_OK;
    for (;;) {
        if (w.ready || w.canceled || w.deleted) break;
        if (!timeout_us) {
            plat_cond_wait(ef->cond, ef->lock, UINT64_MAX);
            continue;
        }
        uint64_t now = plat_monotonic_ns();
        if (now >= deadline) {
            rc = SCE_KERNEL_ERROR(PS4_ETIMEDOUT);
            break;
        }
        plat_cond_wait(ef->cond, ef->lock, (deadline - now + 999) / 1000);
    }
    evf_unlink(ef, &w);
    if (w.ready) {
        rc = SCE_OK;
        if (result) *result = w.result;
    } else if (w.canceled) {
        rc = SCE_KERNEL_ERROR(PS4_ECANCELED_);
        if (result) *result = w.result;
    } else if (w.deleted) {
        rc = SCE_KERNEL_ERROR(PS4_EACCES);
    } else if (result) {
        *result = ef->bits; /* timed out */
    }
    if (timeout_us) {
        uint64_t now = plat_monotonic_ns();
        *timeout_us = (rc == SCE_KERNEL_ERROR(PS4_ETIMEDOUT) || now >= deadline)
                          ? 0 : (uint32_t)((deadline - now) / 1000);
    }
    if (ef->deleted) plat_cond_broadcast(ef->cond); /* let the deleter proceed */
    plat_mutex_unlock(ef->lock);
    return rc;
}

static SYSV int32_t ps4_sceKernelWaitEventFlag(evflag *ef, uint64_t bits, uint32_t mode,
                                               uint64_t *result, uint32_t *timeout_us) {
    return evf_wait(ef, bits, mode, result, timeout_us);
}

static SYSV int32_t ps4_sceKernelPollEventFlag(evflag *ef, uint64_t bits, uint32_t mode,
                                               uint64_t *result) {
    uint32_t zero = 0;
    int32_t rc = evf_wait(ef, bits, mode, result, &zero);
    return rc == SCE_KERNEL_ERROR(PS4_ETIMEDOUT) ? SCE_KERNEL_ERROR(PS4_EBUSY) : rc;
}

static SYSV int32_t ps4_sceKernelSetEventFlag(evflag *ef, uint64_t bits) {
    if (!ef) return SCE_KERNEL_ERROR(PS4_ESRCH);
    plat_mutex_lock(ef->lock);
    ef->bits |= bits;
    bool woke = false;
    for (evf_waiter *w = ef->waiters; w; w = w->next) {
        if (w->ready || w->canceled || w->deleted) continue;
        if (!evf_met(ef->bits, w->bits, w->mode)) continue;
        w->result = ef->bits;
        evf_clear(ef, w->bits, w->mode);
        w->ready = true;
        woke = true;
    }
    if (woke) plat_cond_broadcast(ef->cond);
    plat_mutex_unlock(ef->lock);
    return SCE_OK;
}

/* Note: the PS4 call keeps only the bits set in `bits` (ef &= bits). */
static SYSV int32_t ps4_sceKernelClearEventFlag(evflag *ef, uint64_t bits) {
    if (!ef) return SCE_KERNEL_ERROR(PS4_ESRCH);
    plat_mutex_lock(ef->lock);
    ef->bits &= bits;
    plat_mutex_unlock(ef->lock);
    return SCE_OK;
}

static SYSV int32_t ps4_sceKernelCancelEventFlag(evflag *ef, uint64_t pattern, int32_t *nwaiters) {
    if (!ef) return SCE_KERNEL_ERROR(PS4_ESRCH);
    plat_mutex_lock(ef->lock);
    int n = 0;
    ef->bits = pattern;
    for (evf_waiter *w = ef->waiters; w; w = w->next) {
        if (w->ready || w->canceled || w->deleted) continue;
        w->result = pattern;
        w->canceled = true;
        n++;
    }
    if (nwaiters) *nwaiters = n;
    plat_cond_broadcast(ef->cond);
    plat_mutex_unlock(ef->lock);
    return SCE_OK;
}

HLE_LIBRARY(hle_evflag, "libkernel",
    HLE(sceKernelCreateEventFlag),
    HLE(sceKernelDeleteEventFlag),
    HLE(sceKernelWaitEventFlag),
    HLE(sceKernelPollEventFlag),
    HLE(sceKernelSetEventFlag),
    HLE(sceKernelClearEventFlag),
    HLE(sceKernelCancelEventFlag),
)
