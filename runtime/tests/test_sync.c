/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Workstream A: event flags, event queues, kernel misc. */
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "hle/equeue.h"
#include "hle/hle.h"

static int g_failures;
#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                         \
        }                                                                         \
    } while (0)

#define ERR(e) SCE_KERNEL_ERROR(e)

typedef void *evf;
static SYSV int32_t (*CreateEventFlag)(evf *, const char *, uint32_t, uint64_t, const void *);
static SYSV int32_t (*DeleteEventFlag)(evf);
static SYSV int32_t (*WaitEventFlag)(evf, uint64_t, uint32_t, uint64_t *, uint32_t *);
static SYSV int32_t (*PollEventFlag)(evf, uint64_t, uint32_t, uint64_t *);
static SYSV int32_t (*SetEventFlag)(evf, uint64_t);
static SYSV int32_t (*ClearEventFlag)(evf, uint64_t);
static SYSV int32_t (*CancelEventFlag)(evf, uint64_t, int32_t *);

static SYSV int32_t (*CreateEqueue)(equeue **, const char *);
static SYSV int32_t (*DeleteEqueue)(equeue *);
static SYSV int32_t (*WaitEqueue)(equeue *, ps4_kevent *, int32_t, int32_t *, uint32_t *);
static SYSV int32_t (*AddUserEvent)(equeue *, int32_t);
static SYSV int32_t (*AddUserEventEdge)(equeue *, int32_t);
static SYSV int32_t (*TriggerUserEvent)(equeue *, int32_t, void *);
static SYSV int32_t (*DeleteUserEvent)(equeue *, int32_t);
static SYSV int32_t (*GetEventFilter)(const ps4_kevent *);
static SYSV uint64_t (*GetEventData)(const ps4_kevent *);

#define LOOKUP(var, lib, name)                                                    \
    do {                                                                          \
        *(void **)&var = hle_lookup(lib, name);                                   \
        CHECK(var != NULL);                                                       \
        if (!var) return false;                                                   \
    } while (0)

static bool lookup_all(void) {
    LOOKUP(CreateEventFlag, "libkernel", "sceKernelCreateEventFlag");
    LOOKUP(DeleteEventFlag, "libkernel", "sceKernelDeleteEventFlag");
    LOOKUP(WaitEventFlag, "libkernel", "sceKernelWaitEventFlag");
    LOOKUP(PollEventFlag, "libkernel", "sceKernelPollEventFlag");
    LOOKUP(SetEventFlag, "libkernel", "sceKernelSetEventFlag");
    LOOKUP(ClearEventFlag, "libkernel", "sceKernelClearEventFlag");
    LOOKUP(CancelEventFlag, "libkernel", "sceKernelCancelEventFlag");
    LOOKUP(CreateEqueue, "libkernel", "sceKernelCreateEqueue");
    LOOKUP(DeleteEqueue, "libkernel", "sceKernelDeleteEqueue");
    LOOKUP(WaitEqueue, "libkernel", "sceKernelWaitEqueue");
    LOOKUP(AddUserEvent, "libkernel", "sceKernelAddUserEvent");
    LOOKUP(AddUserEventEdge, "libkernel", "sceKernelAddUserEventEdge");
    LOOKUP(TriggerUserEvent, "libkernel", "sceKernelTriggerUserEvent");
    LOOKUP(DeleteUserEvent, "libkernel", "sceKernelDeleteUserEvent");
    LOOKUP(GetEventFilter, "libkernel", "sceKernelGetEventFilter");
    LOOKUP(GetEventData, "libkernel", "sceKernelGetEventData");
    return true;
}

enum { AND = 1, OR = 2, CLEAR_ALL = 0x10, CLEAR_PAT = 0x20, ATTR_MULTI = 0x20 };

/* ---- event flags ------------------------------------------------------------ */

static void test_evflag_basic(void) {
    evf ef = NULL;
    uint64_t res = 0;
    CHECK(CreateEventFlag(&ef, "basic", 0, 0x5, NULL) == SCE_OK);
    CHECK(ef != NULL);
    CHECK(CreateEventFlag(&ef, "0123456789012345678901234567890123", 0, 0, NULL) ==
          ERR(PS4_ENAMETOOLONG));
    CHECK(PollEventFlag(ef, 0, AND, &res) == ERR(PS4_EINVAL));
    CHECK(PollEventFlag(NULL, 1, AND, &res) == ERR(PS4_ESRCH));

    /* AND needs all bits, OR any. */
    CHECK(PollEventFlag(ef, 0x7, AND, &res) == ERR(PS4_EBUSY));
    CHECK(res == 0x5);
    CHECK(PollEventFlag(ef, 0x6, OR, &res) == SCE_OK && res == 0x5);
    CHECK(PollEventFlag(ef, 0x5, AND, &res) == SCE_OK && res == 0x5);

    /* CLEAR_PAT clears only the waited bits; result is the value before. */
    CHECK(SetEventFlag(ef, 0x18) == SCE_OK);
    CHECK(PollEventFlag(ef, 0x8, AND | CLEAR_PAT, &res) == SCE_OK && res == 0x1d);
    CHECK(PollEventFlag(ef, 0xff, OR, &res) == SCE_OK && res == 0x15);
    /* CLEAR_ALL clears everything. */
    CHECK(PollEventFlag(ef, 0x1, OR | CLEAR_ALL, &res) == SCE_OK && res == 0x15);
    CHECK(PollEventFlag(ef, ~0ull, OR, &res) == ERR(PS4_EBUSY) && res == 0);

    /* Clear keeps only the given bits. */
    CHECK(SetEventFlag(ef, 0xf0) == SCE_OK);
    CHECK(ClearEventFlag(ef, 0x30) == SCE_OK);
    CHECK(PollEventFlag(ef, ~0ull, OR, &res) == SCE_OK && res == 0x30);

    /* Timed wait times out with ETIMEDOUT and zeroes the timeout. */
    uint32_t timeout = 20000;
    uint64_t t0 = plat_monotonic_ns();
    CHECK(WaitEventFlag(ef, 0x1, AND, &res, &timeout) == ERR(PS4_ETIMEDOUT));
    CHECK(plat_monotonic_ns() - t0 >= 15000000ull);
    CHECK(timeout == 0);
    CHECK(res == 0x30);

    /* Satisfied immediately: timeout untouched or reduced, never errors. */
    timeout = 1000000;
    CHECK(WaitEventFlag(ef, 0x10, AND, &res, &timeout) == SCE_OK);
    CHECK(timeout > 0 && timeout <= 1000000);

    CHECK(DeleteEventFlag(ef) == SCE_OK);
    CHECK(DeleteEventFlag(NULL) == ERR(PS4_ESRCH));
}

typedef struct waiter_arg {
    evf ef;
    uint64_t bits;
    uint32_t mode;
    uint32_t *timeout;
    int32_t rc;
    uint64_t result;
    atomic_int started;
} waiter_arg;

static void waiter_fn(void *p) {
    waiter_arg *a = p;
    atomic_store(&a->started, 1);
    a->rc = WaitEventFlag(a->ef, a->bits, a->mode, &a->result, a->timeout);
}

static void wait_started(waiter_arg *a) {
    while (!atomic_load(&a->started)) plat_yield();
    plat_sleep_us(20000); /* let it block */
}

static void test_evflag_threads(void) {
    evf ef = NULL;
    uint64_t res;
    CHECK(CreateEventFlag(&ef, "threads", ATTR_MULTI, 0, NULL) == SCE_OK);

    /* A blocked AND waiter wakes only when all bits are set. */
    waiter_arg a = {.ef = ef, .bits = 0x3, .mode = AND | CLEAR_ALL};
    plat_thread *t = plat_thread_create(waiter_fn, &a, 1 << 20);
    wait_started(&a);
    CHECK(SetEventFlag(ef, 0x1) == SCE_OK);
    plat_sleep_us(20000);
    CHECK(a.rc == 0 && a.result == 0); /* still waiting */
    CHECK(SetEventFlag(ef, 0x2) == SCE_OK);
    plat_thread_join(t);
    CHECK(a.rc == SCE_OK);
    CHECK(a.result == 0x3);
    CHECK(PollEventFlag(ef, ~0ull, OR, &res) == ERR(PS4_EBUSY)); /* cleared */

    /* Two CLEAR_ALL waiters on the same bit: Set wakes only the first one. */
    waiter_arg b1 = {.ef = ef, .bits = 0x1, .mode = OR | CLEAR_ALL};
    uint32_t to2 = 100000;
    waiter_arg b2 = {.ef = ef, .bits = 0x1, .mode = OR | CLEAR_ALL, .timeout = &to2};
    plat_thread *t1 = plat_thread_create(waiter_fn, &b1, 1 << 20);
    wait_started(&b1);
    plat_thread *t2 = plat_thread_create(waiter_fn, &b2, 1 << 20);
    wait_started(&b2);
    CHECK(SetEventFlag(ef, 0x1) == SCE_OK);
    plat_thread_join(t1);
    plat_thread_join(t2);
    CHECK(b1.rc == SCE_OK && b1.result == 0x1);
    CHECK(b2.rc == ERR(PS4_ETIMEDOUT));

    /* Cancel wakes waiters with ECANCELED and sets the pattern. */
    waiter_arg c = {.ef = ef, .bits = 0x80, .mode = AND};
    t = plat_thread_create(waiter_fn, &c, 1 << 20);
    wait_started(&c);
    int32_t n = -1;
    CHECK(CancelEventFlag(ef, 0x4, &n) == SCE_OK);
    plat_thread_join(t);
    CHECK(n == 1);
    CHECK(c.rc == SCE_KERNEL_ERROR(85));
    CHECK(c.result == 0x4);

    /* Delete wakes a blocked waiter with EACCES. */
    waiter_arg d = {.ef = ef, .bits = 0x80, .mode = AND};
    t = plat_thread_create(waiter_fn, &d, 1 << 20);
    wait_started(&d);
    CHECK(DeleteEventFlag(ef) == SCE_OK);
    plat_thread_join(t);
    CHECK(d.rc == ERR(PS4_EACCES));
}

static void test_evflag_single(void) {
    evf ef = NULL;
    CHECK(CreateEventFlag(&ef, "single", 0x10, 0, NULL) == SCE_OK);
    waiter_arg a = {.ef = ef, .bits = 0x1, .mode = AND};
    plat_thread *t = plat_thread_create(waiter_fn, &a, 1 << 20);
    wait_started(&a);
    uint32_t to = 1000;
    uint64_t res;
    CHECK(WaitEventFlag(ef, 0x1, AND, &res, &to) == ERR(PS4_EPERM));
    CHECK(SetEventFlag(ef, 0x1) == SCE_OK);
    plat_thread_join(t);
    CHECK(a.rc == SCE_OK);
    CHECK(DeleteEventFlag(ef) == SCE_OK);
}

/* ---- event queues ------------------------------------------------------------ */

static void test_equeue_user(void) {
    equeue *eq = NULL;
    ps4_kevent ev[4];
    int32_t n = -1;
    uint32_t zero = 0;
    CHECK(CreateEqueue(&eq, "user") == SCE_OK && eq != NULL);
    CHECK(CreateEqueue(NULL, "x") == ERR(PS4_EINVAL));

    /* Nothing triggered: poll times out. */
    CHECK(AddUserEventEdge(eq, 7) == SCE_OK);
    CHECK(WaitEqueue(eq, ev, 4, &n, &zero) == ERR(PS4_ETIMEDOUT) && n == 0);
    CHECK(WaitEqueue(eq, ev, 0, &n, &zero) == ERR(PS4_EINVAL));
    CHECK(TriggerUserEvent(eq, 8, NULL) == ERR(PS4_ENOENT));

    /* Edge: delivered once, carries udata. */
    CHECK(TriggerUserEvent(eq, 7, (void *)0x1234) == SCE_OK);
    CHECK(WaitEqueue(eq, ev, 4, &n, &zero) == SCE_OK && n == 1);
    CHECK(ev[0].ident == 7 && ev[0].udata == (void *)0x1234);
    CHECK(GetEventFilter(&ev[0]) == EVFILT_USER);
    CHECK(WaitEqueue(eq, ev, 4, &n, &zero) == ERR(PS4_ETIMEDOUT));

    /* Level (no EV_CLEAR): stays triggered. */
    CHECK(AddUserEvent(eq, 9) == SCE_OK);
    CHECK(TriggerUserEvent(eq, 9, NULL) == SCE_OK);
    CHECK(WaitEqueue(eq, ev, 4, &n, &zero) == SCE_OK && n == 1 && ev[0].ident == 9);
    CHECK(WaitEqueue(eq, ev, 4, &n, &zero) == SCE_OK && n == 1 && ev[0].ident == 9);
    CHECK(DeleteUserEvent(eq, 9) == SCE_OK);
    CHECK(DeleteUserEvent(eq, 9) == ERR(PS4_ENOENT));
    CHECK(WaitEqueue(eq, ev, 4, &n, &zero) == ERR(PS4_ETIMEDOUT));

    /* Timed wait. */
    uint32_t to = 20000;
    uint64_t t0 = plat_monotonic_ns();
    CHECK(WaitEqueue(eq, ev, 4, &n, &to) == ERR(PS4_ETIMEDOUT) && n == 0);
    CHECK(plat_monotonic_ns() - t0 >= 15000000ull);

    CHECK(DeleteEqueue(eq) == SCE_OK);
    CHECK(DeleteEqueue(eq) == ERR(PS4_EBADF));
    CHECK(TriggerUserEvent(eq, 7, NULL) == ERR(PS4_EBADF));
    equeue_trigger(eq, 7, EVFILT_USER, 0); /* stale handle: ignored */
}

typedef struct trig_arg {
    equeue *eq;
    int count;
} trig_arg;

/* Simulates the vblank thread / PM4 executor: a host thread triggering. */
static void trigger_fn(void *p) {
    trig_arg *a = p;
    for (int i = 0; i < a->count; i++) {
        plat_sleep_us(2000);
        equeue_trigger(a->eq, 0x40, EVFILT_GRAPHICS_CORE, 0x40);
    }
}

typedef struct eqwait_arg {
    equeue *eq;
    int32_t rc, n;
    atomic_int started;
} eqwait_arg;

static void eqwait_fn(void *p) {
    eqwait_arg *a = p;
    ps4_kevent ev[2];
    atomic_store(&a->started, 1);
    a->rc = WaitEqueue(a->eq, ev, 2, &a->n, NULL);
}

static void test_equeue_host(void) {
    equeue *eq = NULL;
    ps4_kevent ev[4];
    int32_t n = 0;
    CHECK(CreateEqueue(&eq, "gfx") == SCE_OK);
    CHECK(equeue_add_event(eq, 0x40, EVFILT_GRAPHICS_CORE, EV_ADD, (void *)0x99));
    CHECK(equeue_add_event(eq, 0x40, EVFILT_GRAPHICS_CORE, EV_ADD, (void *)0x98)); /* update */
    equeue_trigger(eq, 0x41, EVFILT_GRAPHICS_CORE, 1); /* unregistered: no-op */

    /* Blocking waits woken from another host thread. */
    trig_arg ta = {eq, 20};
    plat_thread *t = plat_thread_create(trigger_fn, &ta, 1 << 20);
    int got = 0;
    for (int i = 0; i < 200 && got < 20; i++) {
        uint32_t to = 500000;
        if (WaitEqueue(eq, ev, 4, &n, &to) == SCE_OK) {
            CHECK(n == 1);
            CHECK(ev[0].ident == 0x40 && ev[0].udata == (void *)0x98);
            CHECK(GetEventData(&ev[0]) == 0x40);
            CHECK(ev[0].flags & EV_CLEAR);
            got++;
            /* Triggers may coalesce, so drain more than we expect. */
        } else {
            break;
        }
        if (ta.count == got) break;
    }
    plat_thread_join(t);
    CHECK(got >= 1);

    /* VideoOut flip: data = time:12 | count:4 | flip_arg:48; count accumulates. */
    CHECK(equeue_add_event(eq, 0x6, EVFILT_VIDEO_OUT, EV_ADD, NULL));
    equeue_trigger(eq, 0x6, EVFILT_VIDEO_OUT, 0x6 | (0x1234ull << 16));
    equeue_trigger(eq, 0x6, EVFILT_VIDEO_OUT, 0x6 | (0x1235ull << 16));
    uint32_t zero = 0;
    CHECK(WaitEqueue(eq, ev, 4, &n, &zero) == SCE_OK && n == 1);
    CHECK(ev[0].filter == EVFILT_VIDEO_OUT);
    CHECK(((ev[0].data >> 12) & 0xf) == 2);
    CHECK((ev[0].data >> 16) == 0x1235);
    equeue_trigger(eq, 0x6, EVFILT_VIDEO_OUT, 0x6 | (0x1236ull << 16));
    CHECK(WaitEqueue(eq, ev, 4, &n, &zero) == SCE_OK && n == 1);
    CHECK(((ev[0].data >> 12) & 0xf) == 1); /* cleared on delivery */

    /* One-shot events are removed on delivery. */
    CHECK(equeue_add_event(eq, 0x50, EVFILT_GRAPHICS_CORE, EV_ADD | EV_ONESHOT, NULL));
    equeue_trigger(eq, 0x50, EVFILT_GRAPHICS_CORE, 1);
    CHECK(WaitEqueue(eq, ev, 4, &n, &zero) == SCE_OK && n == 1 && ev[0].ident == 0x50);
    CHECK(!equeue_remove_event(eq, 0x50, EVFILT_GRAPHICS_CORE));
    CHECK(equeue_remove_event(eq, 0x6, EVFILT_VIDEO_OUT));

    /* Delete wakes a blocked waiter. */
    eqwait_arg w = {.eq = eq};
    t = plat_thread_create(eqwait_fn, &w, 1 << 20);
    while (!atomic_load(&w.started)) plat_yield();
    plat_sleep_us(20000);
    CHECK(DeleteEqueue(eq) == SCE_OK);
    plat_thread_join(t);
    CHECK(w.rc == ERR(PS4_EBADF) && w.n == 0);
    CHECK(!equeue_add_event(eq, 1, EVFILT_USER, EV_ADD, NULL));
}

/* ---- misc ---------------------------------------------------------------------- */

typedef struct ps4_timesec {
    int64_t t;
    uint32_t west_sec, dst_sec;
} ps4_timesec;

static void test_misc(void) {
    SYSV int32_t (*utc2local)(int64_t, int64_t *, ps4_timesec *, uint32_t *);
    SYSV int32_t (*local2utc)(int64_t, int32_t, int64_t *, ps4_timesec *, int32_t *);
    SYSV void (*heap_trace)(void *);
    SYSV int32_t (*seginfo)(uint64_t *);
    SYSV int32_t (*modinfo)(uint64_t, int32_t, void *);
    *(void **)&utc2local = hle_lookup("libkernel", "sceKernelConvertUtcToLocaltime");
    *(void **)&local2utc = hle_lookup("libkernel", "sceKernelConvertLocaltimeToUtc");
    *(void **)&heap_trace = hle_lookup("libSceLibcInternalExt", "sceLibcHeapGetTraceInfo");
    *(void **)&seginfo = hle_lookup("libkernel", "sceKernelInternalMemoryGetModuleSegmentInfo");
    *(void **)&modinfo = hle_lookup("libkernel", "sceKernelGetModuleInfoFromAddr");
    CHECK(utc2local && local2utc && heap_trace && seginfo && modinfo);
    if (!utc2local || !local2utc || !heap_trace || !seginfo || !modinfo) return;

    int64_t utc = 1700000000, local = 0, back = 0;
    ps4_timesec ts = {0}, ts2 = {0};
    uint32_t dst = 99;
    int32_t dst2 = 99;
    CHECK(utc2local(utc, &local, &ts, &dst) == SCE_OK);
    CHECK(ts.t == utc);
    CHECK(local - utc == (int64_t)(int32_t)ts.west_sec + ts.dst_sec);
    CHECK(local - utc > -15 * 3600 && local - utc < 15 * 3600);
    CHECK(local2utc(local, 0, &back, &ts2, &dst2) == SCE_OK);
    CHECK(back == utc);
    CHECK(ts2.west_sec == ts.west_sec);

    uint64_t info[4] = {0x20, 0, 0, 0};
    heap_trace(info);
    CHECK((info[1] >> 32) == 0); /* getSegmentInfo off */
    CHECK(info[2] != 0 && info[3] != 0);

    uint64_t seg[2] = {1, 2};
    CHECK(seginfo(seg) == SCE_OK && seg[0] == 0 && seg[1] == 0);

    static uint8_t mi[0x1a8];
    CHECK(modinfo(0x1234, 3, mi) == ERR(PS4_EINVAL));
    CHECK(modinfo(0x1234, 0, NULL) == ERR(PS4_EFAULT));
    CHECK(modinfo(0x1234, 0, mi) == ERR(PS4_ESRCH)); /* no image loaded */
}

int main(void) {
    log_init();
    hle_init();
    if (lookup_all()) {
        test_evflag_basic();
        test_evflag_threads();
        test_evflag_single();
        test_equeue_user();
        test_equeue_host();
    }
    test_misc();
    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("sync tests passed\n");
    return 0;
}
