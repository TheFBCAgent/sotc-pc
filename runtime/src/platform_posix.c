/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _WIN32

#define _GNU_SOURCE
#include <asm/prctl.h>
#include <cpuid.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <x86intrin.h>

#include "log.h"
#include "platform.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

/* ---- memory ------------------------------------------------------------ */

static int posix_prot(int prot) {
    int p = 0;
    if (prot & PLAT_PROT_R) p |= PROT_READ;
    if (prot & PLAT_PROT_W) p |= PROT_READ | PROT_WRITE;
    if (prot & PLAT_PROT_X) p |= PROT_READ | PROT_EXEC;
    return p;
}

static void *try_map(uint64_t addr, size_t size, int prot) {
    void *p = mmap((void *)(uintptr_t)addr, size, posix_prot(prot),
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return NULL;
    if ((uint64_t)(uintptr_t)p != addr) { /* old kernels ignore NOREPLACE */
        munmap(p, size);
        return NULL;
    }
    return p;
}

void *plat_map(uint64_t addr, size_t size, int prot, bool fixed) {
    if (fixed) return try_map(addr, size, prot);
    const uint64_t step = 0x10000;
    uint64_t start = addr ? (addr + step - 1) & ~(step - 1) : 0x10000000ull;
    for (uint64_t cur = start; cur + size <= PLAT_GUEST_LIMIT;) {
        void *p = try_map(cur, size, prot);
        if (p) return p;
        /* Skip ahead in growing strides so a crowded area is passed quickly. */
        cur += size > step ? ((size + step - 1) & ~(step - 1)) : step;
    }
    return NULL;
}

void plat_unmap(void *addr, size_t size) { munmap(addr, size); }

void *plat_reserve(uint64_t addr, size_t size) {
    void *p = mmap((void *)(uintptr_t)addr, size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

bool plat_commit(void *addr, size_t size, int prot) {
    return mmap(addr, size, posix_prot(prot), MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) !=
           MAP_FAILED;
}

void plat_decommit(void *addr, size_t size) {
    mmap(addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
}

bool plat_protect(void *addr, size_t size, int prot) {
    return mprotect(addr, size, posix_prot(prot)) == 0;
}

void plat_flush_icache(void *addr, size_t size) {
    (void)addr; (void)size; /* x86 keeps instruction and data caches coherent */
}

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

static void install_altstack(void) {
    stack_t ss = {0};
    ss.ss_size = 64 * 1024;
    ss.ss_sp = malloc(ss.ss_size);
    sigaltstack(&ss, NULL);
}

static void *thread_main(void *p) {
    plat_thread *t = p;
    install_altstack();
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

/* ---- faults ------------------------------------------------------------ */

static plat_fault_handler g_handler;

static void on_signal(int sig, siginfo_t *info, void *uctx) {
    ucontext_t *uc = uctx;
    plat_fault f = {0};
    f.kind = sig == SIGSEGV || sig == SIGBUS ? PLAT_FAULT_ACCESS
           : sig == SIGILL                   ? PLAT_FAULT_ILLEGAL
                                             : PLAT_FAULT_OTHER;
    f.addr = (uint64_t)(uintptr_t)info->si_addr;
    f.rip = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
    f.rip_ptr = (uint64_t *)&uc->uc_mcontext.gregs[REG_RIP];
    f.rsp = (uint64_t *)uc->uc_mcontext.gregs[REG_RSP];
    f.xmm = uc->uc_mcontext.fpregs ? (void *)uc->uc_mcontext.fpregs->_xmm : NULL;
    if (g_handler && g_handler(&f)) return;
    signal(sig, SIG_DFL); /* let the default action produce a core dump */
}

void plat_install_fault_handler(plat_fault_handler handler) {
    g_handler = handler;
    install_altstack();
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) sigaction(sigs[i], &sa, NULL);
}

bool plat_cpu_has_sse4a(void) {
    unsigned a, b, c, d;
    return __get_cpuid(0x80000001, &a, &b, &c, &d) && (c & (1u << 6));
}

#endif
