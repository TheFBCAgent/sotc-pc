/* SPDX-License-Identifier: GPL-2.0-or-later */
/* POSIX: guest TLS, threads, sync, time and sleep. */
#ifndef _WIN32

#define _GNU_SOURCE
#include <asm/prctl.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <x86intrin.h>

#include "log.h"
#include "platform.h"

/* ---- guest TLS --------------------------------------------------------- */

/* GS is unused by glibc on x86-64, so it can point straight at the guest TCB.
 * A rewritten `mov reg, fs:[disp]` becomes `mov reg, gs:[disp]`. */
bool plat_tls_init(void) { return true; }

uint32_t plat_tls_patch_disp(int32_t fs_disp) { return (uint32_t)fs_disp; }

void plat_set_guest_tcb(void *tcb) {
    syscall(SYS_arch_prctl, ARCH_SET_GS, (unsigned long)(uintptr_t)tcb);
}

/* ---- threads ----------------------------------------------------------- */

struct plat_thread {
    pthread_t handle;
    plat_thread_fn fn;
    void *arg;
};

/* platform_posix_fault.c: the signal stack every thread needs. */
void posix_install_altstack(void);

static void *thread_main(void *p) {
    plat_thread *t = p;
    posix_install_altstack();
    t->fn(t->arg);
    return NULL;
}

plat_thread *plat_thread_create(plat_thread_fn fn, void *arg, size_t stack_size) {
    plat_thread *t = calloc(1, sizeof(*t));
    t->fn = fn;
    t->arg = arg;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack_size) pthread_attr_setstacksize(&attr, stack_size);
    int err = pthread_create(&t->handle, &attr, thread_main, t);
    pthread_attr_destroy(&attr);
    if (err) {
        free(t);
        return NULL;
    }
    return t;
}

void plat_thread_join(plat_thread *t) {
    pthread_join(t->handle, NULL);
    free(t);
}

void plat_thread_detach(plat_thread *t) { pthread_detach(t->handle); }

void plat_thread_set_name(const char *name) {
    char buf[16];
    strncpy(buf, name, 15);
    buf[15] = 0;
    pthread_setname_np(pthread_self(), buf);
}

void plat_yield(void) { sched_yield(); }

void plat_sleep_us(uint64_t us) {
    struct timespec ts = {(time_t)(us / 1000000), (long)(us % 1000000) * 1000};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

uint64_t plat_thread_id(void) { return (uint64_t)syscall(SYS_gettid); }

/* ---- sync -------------------------------------------------------------- */

struct plat_mutex { pthread_mutex_t m; };
struct plat_cond { pthread_cond_t c; };

plat_mutex *plat_mutex_new(void) {
    plat_mutex *m = malloc(sizeof(*m));
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m->m, &attr);
    pthread_mutexattr_destroy(&attr);
    return m;
}
void plat_mutex_free(plat_mutex *m) { pthread_mutex_destroy(&m->m); free(m); }
void plat_mutex_lock(plat_mutex *m) { pthread_mutex_lock(&m->m); }
bool plat_mutex_trylock(plat_mutex *m) { return pthread_mutex_trylock(&m->m) == 0; }
void plat_mutex_unlock(plat_mutex *m) { pthread_mutex_unlock(&m->m); }

plat_cond *plat_cond_new(void) {
    plat_cond *c = malloc(sizeof(*c));
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&c->c, &attr);
    pthread_condattr_destroy(&attr);
    return c;
}
void plat_cond_free(plat_cond *c) { pthread_cond_destroy(&c->c); free(c); }

bool plat_cond_wait(plat_cond *c, plat_mutex *m, uint64_t timeout_us) {
    if (timeout_us == UINT64_MAX) return pthread_cond_wait(&c->c, &m->m) == 0;
    uint64_t deadline = plat_monotonic_ns() + timeout_us * 1000;
    struct timespec ts = {(time_t)(deadline / 1000000000), (long)(deadline % 1000000000)};
    return pthread_cond_timedwait(&c->c, &m->m, &ts) == 0;
}
void plat_cond_signal(plat_cond *c) { pthread_cond_signal(&c->c); }
void plat_cond_broadcast(plat_cond *c) { pthread_cond_broadcast(&c->c); }

/* ---- time -------------------------------------------------------------- */

uint64_t plat_monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec;
}

uint64_t plat_realtime_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec;
}

uint64_t plat_tsc(void) { return __rdtsc(); }

uint64_t plat_tsc_frequency(void) {
    static uint64_t freq;
    if (!freq) {
        uint64_t t0 = plat_monotonic_ns(), c0 = __rdtsc();
        plat_sleep_us(50000);
        uint64_t t1 = plat_monotonic_ns(), c1 = __rdtsc();
        freq = (c1 - c0) * 1000000000ull / (t1 - t0); /* < 2^64 for any real CPU */
    }
    return freq;
}

#endif
