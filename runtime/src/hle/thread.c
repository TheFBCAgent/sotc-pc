/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libkernel/libScePosix threads and synchronization.
 *
 * Guest threads are native threads. PS4 pthread objects (mutex, cond, attr,
 * rwlock) are pointer-sized handles the guest owns; we allocate the real
 * object on first use, so the static initializers (NULL) work.
 *
 * Each operation has one implementation returning an errno value; the sce*
 * entry points convert it to a SCE kernel error, the posix ones return it. */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../log.h"
#include "../runtime.h"
#include "../tls.h"
#include "hle.h"

#define MAX_KEYS 256
#define DEFAULT_STACK (2u << 20)
#define MIN_STACK (1u << 20) /* host code also runs on guest thread stacks */

typedef SYSV void *(*guest_start_fn)(void *);
typedef SYSV void (*guest_dtor_fn)(void *);

typedef struct guest_thread {
    guest_tcb *tcb;
    plat_thread *host;
    guest_start_fn start;
    void *arg;
    void *retval;
    uint64_t id;
    int32_t prio;
    uint64_t affinity;
    uint64_t stack_size;
    bool detached;
    char name[32];
    void *specific[MAX_KEYS];
    void *exit_jmp[5];  /* __builtin_setjmp buffer for pthread_exit */
    bool exit_armed;    /* exit_jmp is set: pthread_exit may jump to it */
} guest_thread;

/* fiber.c: pthread_exit from inside sceFiberRun leaves the fiber for good. */
void hle_fiber_thread_exit(void);

typedef struct guest_attr {
    uint64_t stack_size;
    int32_t detach;
    int32_t prio, policy, inherit;
    uint64_t affinity;
} guest_attr;

static _Thread_local guest_thread *t_self;
static _Atomic uint64_t g_next_id = 1;
static guest_dtor_fn g_key_dtor[MAX_KEYS];
static bool g_key_used[MAX_KEYS];
static plat_mutex *g_lock;

static int32_t sce(int e) { return e ? SCE_KERNEL_ERROR(e) : SCE_OK; }

void hle_thread_init(void) { g_lock = plat_mutex_new(); }

static guest_thread *self(void) {
    if (!t_self) { /* a host thread calling in (should not happen) */
        t_self = calloc(1, sizeof(guest_thread));
        t_self->id = atomic_fetch_add(&g_next_id, 1);
    }
    return t_self;
}

/* ---- thread lifecycle -------------------------------------------------- */

static void run_key_destructors(guest_thread *t) {
    for (int round = 0; round < 4; round++) {
        bool any = false;
        for (int k = 0; k < MAX_KEYS; k++) {
            void *v = t->specific[k];
            if (v && g_key_dtor[k]) {
                t->specific[k] = NULL;
                g_key_dtor[k](v);
                any = true;
            }
        }
        if (!any) break;
    }
}

static void thread_finish(void);

static void thread_body(void *p) {
    guest_thread *t = p;
    t_self = t;
    t->tcb = tls_create(t);
    tls_set_current(t->tcb);
    plat_thread_set_name(t->name[0] ? t->name : "Game:Thread");
    /* The fault handler's emergency stack (allocated now, not in a fault). */
    if (!plat_fault_thread_init()) log_warn("thread '%s': no emergency fault stack", t->name);
    log_debug("thread %llu '%s' started", (unsigned long long)t->id, t->name);
    if (__builtin_setjmp(t->exit_jmp) == 0) {
        t->exit_armed = true;
        t->retval = t->start(t->arg);
    }
    /* After a longjmp from thread_exit only rbp/rsp are restored. clang keeps
     * values such as the address of t_self in callee-saved registers across
     * the setjmp, and the guest code that ran in between clobbered them, so
     * everything after it lives in a separate, non-inlined function. */
    thread_finish();
}

static __attribute__((noinline)) void thread_finish(void) {
    guest_thread *t = t_self;
    t->exit_armed = false; /* pthread_exit from a key destructor: not supported */
    run_key_destructors(t);
    log_debug("thread %llu '%s' exited", (unsigned long long)t->id, t->name);
    /* The TCB and DTV go with the thread; guest_thread stays (it is the
     * handle others join on or compare against). */
    guest_tcb *tcb = t->tcb;
    tls_set_current(NULL);
    t->tcb = NULL;
    tls_destroy(tcb);
    plat_fault_thread_exit();
}

static int thread_create(guest_thread **out, const guest_attr *attr, guest_start_fn start,
                         void *arg, const char *name) {
    if (!out || !start) return PS4_EINVAL;
    guest_thread *t = calloc(1, sizeof(*t));
    t->start = start;
    t->arg = arg;
    t->id = atomic_fetch_add(&g_next_id, 1);
    t->stack_size = attr && attr->stack_size ? attr->stack_size : DEFAULT_STACK;
    t->prio = attr ? attr->prio : 700;
    t->affinity = attr ? attr->affinity : 0x7F;
    t->detached = attr && attr->detach;
    if (name) strncpy(t->name, name, sizeof(t->name) - 1);
    uint64_t stack = t->stack_size < MIN_STACK ? MIN_STACK : t->stack_size;
    t->host = plat_thread_create(thread_body, t, stack);
    if (!t->host) {
        free(t);
        return PS4_EAGAIN;
    }
    if (t->detached) plat_thread_detach(t->host);
    log_debug("created thread %llu '%s' (stack %#llx)", (unsigned long long)t->id, t->name,
              (unsigned long long)t->stack_size);
    *out = t;
    return 0;
}

static int thread_join(guest_thread *t, void **ret) {
    if (!t) return PS4_EINVAL;
    if (t == t_self) return PS4_EDEADLK;
    /* Joining a detached or already joined thread: its host handle is gone. */
    plat_mutex_lock(g_lock);
    plat_thread *host = t->detached ? NULL : t->host;
    if (host) t->host = NULL;
    plat_mutex_unlock(g_lock);
    if (!host) return PS4_EINVAL;
    plat_thread_join(host);
    if (ret) *ret = t->retval;
    return 0;
}

__attribute__((noreturn)) static void thread_exit(void *value) {
    guest_thread *t = t_self;
    if (!t || !t->exit_armed)
        log_fatal("pthread_exit on thread %llu, which has no exit point (host thread, or "
                  "called from a key destructor)", t ? (unsigned long long)t->id : 0ull);
    t->retval = value;
    /* From inside sceFiberRun: drop the fiber state and, on Windows, put the
     * thread's own stack bounds back into the TEB before leaving the fiber stack. */
    hle_fiber_thread_exit();
    __builtin_longjmp(t->exit_jmp, 1);
}

/* ---- the guest main thread -------------------------------------------- */

typedef struct entry_params {
    int32_t argc;
    uint32_t pad;
    const char *argv[33];
    uint64_t entry_addr;
} entry_params;

static SYSV void guest_exit_fn(void) { log_info("guest exit function called"); }

/* Enter the eboot like the PS4 kernel: params on the stack and in rdi, the
 * exit function in rsi. Mirrors shadPS4's RunMainEntry. */
__attribute__((noreturn)) static void enter_guest(uint64_t entry, entry_params *params) {
    __asm__ volatile("andq $-16, %%rsp\n"
                     "subq $8, %%rsp\n"
                     "pushq 8(%1)\n"
                     "pushq 0(%1)\n"
                     "movq %1, %%rdi\n"
                     "movq %2, %%rsi\n"
                     "jmp *%0\n"
                     :
                     : "r"(entry), "r"(params), "r"(guest_exit_fn)
                     : "rax", "rsi", "rdi", "memory");
    __builtin_unreachable();
}

typedef SYSV int32_t (*module_init_fn)(uint64_t args, const void *argp, void *param);

static void main_thread_finish(void);

static void main_thread_body(void *p) {
    guest_thread *t = p;
    t_self = t;
    t->tcb = tls_create(t);
    tls_set_current(t->tcb);
    plat_thread_set_name("Game:Main");
    if (!plat_fault_thread_init()) log_warn("main thread: no emergency fault stack");

    /* pthread_exit is allowed from the module inits on, too. */
    if (__builtin_setjmp(t->exit_jmp) == 0) {
        t->exit_armed = true;
        /* Start the game's bundled libraries (libc, Fios2), then the eboot. */
        for (uint32_t i = 1; i < g_rt.module_count; i++) {
            const guest_module *m = &g_rt.modules[i];
            if (!m->init) continue;
            log_info("starting module %s", m->name);
            int32_t r = ((module_init_fn)(void *)m->init)(0, NULL, NULL);
            log_info("module %s started: %d", m->name, r);
        }

        static entry_params params = {.argc = 1, .argv = {"/app0/eboot.bin"}};
        params.entry_addr = (uint64_t)(uintptr_t)g_rt.entry;
        log_info("entering eboot at %p", (void *)g_rt.entry);
        enter_guest(params.entry_addr, &params);
    }
    main_thread_finish(); /* see thread_body: registers are stale here */
}

/* pthread_exit on the main thread: the process lives on in its other threads,
 * as on the PS4. The TCB stays: other threads may still read the main
 * thread's TLS through pointers it handed out. */
static __attribute__((noinline, noreturn)) void main_thread_finish(void) {
    guest_thread *t = t_self;
    t->exit_armed = false;
    run_key_destructors(t);
    log_info("guest main thread exited; other threads keep running");
    for (;;) plat_sleep_us(1000000);
}

plat_thread *hle_start_main(void) {
    guest_thread *t = calloc(1, sizeof(*t));
    t->id = atomic_fetch_add(&g_next_id, 1);
    strcpy(t->name, "Game:Main");
    t->stack_size = 8u << 20;
    /* The host handle belongs to the caller (host_main_loop joins it), so
     * guest join/detach of the main thread fail with EINVAL. */
    t->detached = true;
    plat_thread *host = plat_thread_create(main_thread_body, t, t->stack_size);
    if (!host) log_fatal("cannot create the guest main thread");
    return host;
}

void hle_run_main(void) {
    plat_thread_join(hle_start_main());
    log_info("guest main thread returned");
    exit(0);
}

/* ---- attributes -------------------------------------------------------- */

static int attr_init(guest_attr **a) {
    if (!a) return PS4_EINVAL;
    *a = calloc(1, sizeof(guest_attr));
    (*a)->stack_size = DEFAULT_STACK;
    (*a)->prio = 700;
    (*a)->affinity = 0x7F;
    return 0;
}

static int attr_destroy(guest_attr **a) {
    if (!a || !*a) return PS4_EINVAL;
    free(*a);
    *a = NULL;
    return 0;
}

#define ATTR_SETTER(fn, field, type)                                              \
    static SYSV int32_t ps4_scePthreadAttr##fn(guest_attr **a, type v) {          \
        if (!a || !*a) return SCE_KERNEL_ERROR(PS4_EINVAL);                        \
        (*a)->field = v;                                                           \
        return SCE_OK;                                                             \
    }
ATTR_SETTER(Setstacksize, stack_size, uint64_t)
ATTR_SETTER(Setdetachstate, detach, int32_t)
ATTR_SETTER(Setinheritsched, inherit, int32_t)
ATTR_SETTER(Setschedpolicy, policy, int32_t)
ATTR_SETTER(Setaffinity, affinity, uint64_t)

static SYSV int32_t ps4_scePthreadAttrSetschedparam(guest_attr **a, const int32_t *param) {
    if (!a || !*a || !param) return SCE_KERNEL_ERROR(PS4_EINVAL);
    (*a)->prio = *param;
    return SCE_OK;
}

static SYSV int32_t ps4_scePthreadAttrGetstacksize(guest_attr **a, uint64_t *v) {
    if (!a || !*a || !v) return SCE_KERNEL_ERROR(PS4_EINVAL);
    *v = (*a)->stack_size;
    return SCE_OK;
}
static SYSV int32_t ps4_scePthreadAttrGetdetachstate(guest_attr **a, int32_t *v) {
    if (!a || !*a || !v) return SCE_KERNEL_ERROR(PS4_EINVAL);
    *v = (*a)->detach;
    return SCE_OK;
}
static SYSV int32_t ps4_scePthreadAttrGetaffinity(guest_attr **a, uint64_t *v) {
    if (!a || !*a || !v) return SCE_KERNEL_ERROR(PS4_EINVAL);
    *v = (*a)->affinity;
    return SCE_OK;
}

static SYSV int32_t ps4_scePthreadAttrInit(guest_attr **a) { return sce(attr_init(a)); }
static SYSV int32_t ps4_scePthreadAttrDestroy(guest_attr **a) { return sce(attr_destroy(a)); }

static SYSV int32_t ps4_scePthreadAttrGet(guest_thread *t, guest_attr **a) {
    if (!t || !a || !*a) return SCE_KERNEL_ERROR(PS4_EINVAL);
    (*a)->stack_size = t->stack_size;
    (*a)->detach = t->detached;
    (*a)->prio = t->prio;
    (*a)->affinity = t->affinity;
    return SCE_OK;
}

static SYSV int32_t ps4_pthread_attr_init(guest_attr **a) { return attr_init(a); }
static SYSV int32_t ps4_pthread_attr_destroy(guest_attr **a) { return attr_destroy(a); }
static SYSV int32_t ps4_pthread_attr_setstacksize(guest_attr **a, uint64_t v) {
    if (!a || !*a) return PS4_EINVAL;
    (*a)->stack_size = v;
    return 0;
}

/* ---- thread API -------------------------------------------------------- */

static SYSV int32_t ps4_scePthreadCreate(guest_thread **t, guest_attr **attr, guest_start_fn fn,
                                         void *arg, const char *name) {
    return sce(thread_create(t, attr ? *attr : NULL, fn, arg, name));
}
static SYSV int32_t ps4_pthread_create(guest_thread **t, guest_attr **attr, guest_start_fn fn,
                                       void *arg) {
    return thread_create(t, attr ? *attr : NULL, fn, arg, NULL);
}

static SYSV int32_t ps4_scePthreadJoin(guest_thread *t, void **ret) { return sce(thread_join(t, ret)); }
static SYSV int32_t ps4_pthread_join(guest_thread *t, void **ret) { return thread_join(t, ret); }

static SYSV int32_t ps4_scePthreadDetach(guest_thread *t) {
    if (!t) return SCE_KERNEL_ERROR(PS4_EINVAL);
    plat_mutex_lock(g_lock);
    plat_thread *host = t->detached ? NULL : t->host;
    if (host) t->detached = true;
    plat_mutex_unlock(g_lock);
    if (!host) return SCE_KERNEL_ERROR(PS4_EINVAL); /* already detached or joined */
    plat_thread_detach(host);
    return SCE_OK;
}

static SYSV void ps4_scePthreadExit(void *value) { thread_exit(value); }
static SYSV guest_thread *ps4_scePthreadSelf(void) { return self(); }
static SYSV int32_t ps4_scePthreadEqual(guest_thread *a, guest_thread *b) { return a == b; }
static SYSV int32_t ps4_scePthreadGetthreadid(void) { return (int32_t)self()->id; }
static SYSV void ps4_scePthreadYield(void) { plat_yield(); }
static SYSV int32_t ps4_sched_yield(void) { plat_yield(); return 0; }

static SYSV int32_t ps4_scePthreadGetprio(guest_thread *t, int32_t *prio) {
    if (!t || !prio) return SCE_KERNEL_ERROR(PS4_EINVAL);
    *prio = t->prio;
    return SCE_OK;
}
static SYSV int32_t ps4_scePthreadSetprio(guest_thread *t, int32_t prio) {
    if (!t) return SCE_KERNEL_ERROR(PS4_EINVAL);
    t->prio = prio; /* host scheduling priorities are left alone */
    return SCE_OK;
}
static SYSV int32_t ps4_scePthreadGetaffinity(guest_thread *t, uint64_t *mask) {
    if (!t || !mask) return SCE_KERNEL_ERROR(PS4_EINVAL);
    *mask = t->affinity;
    return SCE_OK;
}
static SYSV int32_t ps4_scePthreadSetaffinity(guest_thread *t, uint64_t mask) {
    if (!t) return SCE_KERNEL_ERROR(PS4_EINVAL);
    t->affinity = mask;
    return SCE_OK;
}

static SYSV int32_t ps4_pthread_getschedparam(guest_thread *t, int32_t *policy, int32_t *param) {
    if (!t) return PS4_ESRCH;
    if (policy) *policy = 1;
    if (param) *param = t->prio;
    return 0;
}
static SYSV int32_t ps4_pthread_setschedparam(guest_thread *t, int32_t policy, const int32_t *param) {
    (void)policy;
    if (!t) return PS4_ESRCH;
    if (param) t->prio = *param;
    return 0;
}
static SYSV int32_t ps4_sched_get_priority_max(int32_t policy) { (void)policy; return 256; }
static SYSV int32_t ps4_sched_get_priority_min(int32_t policy) { (void)policy; return 767; }
static SYSV int32_t ps4_pthread_cancel(guest_thread *t) {
    log_warn("pthread_cancel(%p) ignored", (void *)t);
    return 0;
}

/* ---- thread-specific data ---------------------------------------------- */

static int key_create(uint32_t *key, guest_dtor_fn dtor) {
    if (!key) return PS4_EINVAL;
    plat_mutex_lock(g_lock);
    for (uint32_t k = 0; k < MAX_KEYS; k++)
        if (!g_key_used[k]) {
            g_key_used[k] = true;
            g_key_dtor[k] = dtor;
            plat_mutex_unlock(g_lock);
            *key = k;
            return 0;
        }
    plat_mutex_unlock(g_lock);
    return PS4_EAGAIN;
}

static int key_delete(uint32_t key) {
    if (key >= MAX_KEYS || !g_key_used[key]) return PS4_EINVAL;
    g_key_used[key] = false;
    g_key_dtor[key] = NULL;
    return 0;
}

static SYSV int32_t ps4_scePthreadKeyCreate(uint32_t *key, guest_dtor_fn d) { return sce(key_create(key, d)); }
static SYSV int32_t ps4_scePthreadKeyDelete(uint32_t key) { return sce(key_delete(key)); }
static SYSV int32_t ps4_pthread_key_create(uint32_t *key, guest_dtor_fn d) { return key_create(key, d); }
static SYSV int32_t ps4_pthread_key_delete(uint32_t key) { return key_delete(key); }

static SYSV void *ps4_scePthreadGetspecific(uint32_t key) {
    return key < MAX_KEYS ? self()->specific[key] : NULL;
}
static SYSV int32_t ps4_scePthreadSetspecific(uint32_t key, void *value) {
    if (key >= MAX_KEYS || !g_key_used[key]) return SCE_KERNEL_ERROR(PS4_EINVAL);
    self()->specific[key] = value;
    return SCE_OK;
}
static SYSV int32_t ps4_pthread_setspecific(uint32_t key, void *value) {
    if (key >= MAX_KEYS || !g_key_used[key]) return PS4_EINVAL;
    self()->specific[key] = value;
    return 0;
}

/* ---- once -------------------------------------------------------------- */

typedef struct guest_once {
    _Atomic int32_t state; /* 0 never, 1 done, 2 in progress */
    void *mutex;
} guest_once;

static SYSV int32_t ps4_scePthreadOnce(guest_once *once, SYSV void (*fn)(void)) {
    if (!once || !fn) return SCE_KERNEL_ERROR(PS4_EINVAL);
    if (atomic_load(&once->state) == 1) return SCE_OK;
    plat_mutex_lock(g_lock); /* serializes all once-inits; recursive, so nesting is fine */
    if (atomic_load(&once->state) == 0) {
        atomic_store(&once->state, 2);
        fn();
        atomic_store(&once->state, 1);
    }
    plat_mutex_unlock(g_lock);
    return SCE_OK;
}

/* ---- mutexes ----------------------------------------------------------- */

enum { MUTEX_ERRORCHECK = 1, MUTEX_RECURSIVE = 2, MUTEX_NORMAL = 3, MUTEX_ADAPTIVE = 4 };

typedef struct guest_mutex {
    plat_mutex *pm;           /* held exactly once while owned */
    _Atomic(guest_thread *) owner;
    uint32_t count;
    int32_t type;
    char name[32];
} guest_mutex;

typedef struct guest_mutexattr {
    int32_t type, protocol;
} guest_mutexattr;

static guest_mutex *mutex_new(int32_t type, const char *name) {
    guest_mutex *m = calloc(1, sizeof(*m));
    m->pm = plat_mutex_new();
    m->type = type ? type : MUTEX_ERRORCHECK;
    if (name) strncpy(m->name, name, sizeof(m->name) - 1);
    return m;
}

/* Resolve a guest handle, creating the mutex for static initializers. */
static guest_mutex *mutex_get(guest_mutex **h) {
    if (!h) return NULL;
    guest_mutex *m = *(guest_mutex *volatile *)h;
    if ((uintptr_t)m > 2) return m;
    guest_mutex *fresh = mutex_new((uintptr_t)m == 1 ? MUTEX_ADAPTIVE : MUTEX_ERRORCHECK, NULL);
    guest_mutex *expected = m;
    if (atomic_compare_exchange_strong((_Atomic(guest_mutex *) *)h, &expected, fresh)) return fresh;
    plat_mutex_free(fresh->pm);
    free(fresh);
    return expected;
}

static int mutex_init(guest_mutex **h, const guest_mutexattr *const *attr, const char *name) {
    if (!h) return PS4_EINVAL;
    *h = mutex_new(attr && *attr ? (*attr)->type : MUTEX_ERRORCHECK, name);
    return 0;
}

static int mutex_destroy(guest_mutex **h) {
    if (!h) return PS4_EINVAL;
    guest_mutex *m = *h;
    if ((uintptr_t)m > 2) {
        if (atomic_load(&m->owner)) return PS4_EBUSY;
        plat_mutex_free(m->pm);
        free(m);
    }
    *h = NULL;
    return 0;
}

static int mutex_lock(guest_mutex **h, bool try_only) {
    guest_mutex *m = mutex_get(h);
    if (!m) return PS4_EINVAL;
    guest_thread *me = self();
    if (atomic_load(&m->owner) == me) {
        if (m->type == MUTEX_RECURSIVE) { m->count++; return 0; }
        return try_only ? PS4_EBUSY : PS4_EDEADLK;
    }
    if (try_only) {
        if (!plat_mutex_trylock(m->pm)) return PS4_EBUSY;
    } else {
        plat_mutex_lock(m->pm);
    }
    atomic_store(&m->owner, me);
    m->count = 1;
    return 0;
}

static int mutex_unlock(guest_mutex **h) {
    guest_mutex *m = mutex_get(h);
    if (!m) return PS4_EINVAL;
    if (atomic_load(&m->owner) != self()) return PS4_EPERM;
    if (--m->count == 0) {
        atomic_store(&m->owner, NULL);
        plat_mutex_unlock(m->pm);
    }
    return 0;
}

static SYSV int32_t ps4_scePthreadMutexInit(guest_mutex **h, const guest_mutexattr *const *a,
                                            const char *name) { return sce(mutex_init(h, a, name)); }
static SYSV int32_t ps4_scePthreadMutexDestroy(guest_mutex **h) { return sce(mutex_destroy(h)); }
static SYSV int32_t ps4_scePthreadMutexLock(guest_mutex **h) { return sce(mutex_lock(h, false)); }
static SYSV int32_t ps4_scePthreadMutexTrylock(guest_mutex **h) { return sce(mutex_lock(h, true)); }
static SYSV int32_t ps4_scePthreadMutexUnlock(guest_mutex **h) { return sce(mutex_unlock(h)); }
static SYSV int32_t ps4_pthread_mutex_init(guest_mutex **h, const guest_mutexattr *const *a) {
    return mutex_init(h, a, NULL);
}
static SYSV int32_t ps4_pthread_mutex_destroy(guest_mutex **h) { return mutex_destroy(h); }
static SYSV int32_t ps4_pthread_mutex_lock(guest_mutex **h) { return mutex_lock(h, false); }
static SYSV int32_t ps4_pthread_mutex_trylock(guest_mutex **h) { return mutex_lock(h, true); }
static SYSV int32_t ps4_pthread_mutex_unlock(guest_mutex **h) { return mutex_unlock(h); }

static int mutexattr_init(guest_mutexattr **a) {
    if (!a) return PS4_EINVAL;
    *a = calloc(1, sizeof(guest_mutexattr));
    (*a)->type = MUTEX_ERRORCHECK;
    return 0;
}
static int mutexattr_destroy(guest_mutexattr **a) {
    if (!a) return PS4_EINVAL;
    free(*a);
    *a = NULL;
    return 0;
}
static int mutexattr_settype(guest_mutexattr **a, int32_t type) {
    if (!a || !*a || type < MUTEX_ERRORCHECK || type > MUTEX_ADAPTIVE) return PS4_EINVAL;
    (*a)->type = type;
    return 0;
}
static int mutexattr_setprotocol(guest_mutexattr **a, int32_t protocol) {
    if (!a || !*a) return PS4_EINVAL;
    (*a)->protocol = protocol;
    return 0;
}

static SYSV int32_t ps4_scePthreadMutexattrInit(guest_mutexattr **a) { return sce(mutexattr_init(a)); }
static SYSV int32_t ps4_scePthreadMutexattrDestroy(guest_mutexattr **a) { return sce(mutexattr_destroy(a)); }
static SYSV int32_t ps4_scePthreadMutexattrSettype(guest_mutexattr **a, int32_t t) { return sce(mutexattr_settype(a, t)); }
static SYSV int32_t ps4_scePthreadMutexattrSetprotocol(guest_mutexattr **a, int32_t p) { return sce(mutexattr_setprotocol(a, p)); }
static SYSV int32_t ps4_pthread_mutexattr_init(guest_mutexattr **a) { return mutexattr_init(a); }
static SYSV int32_t ps4_pthread_mutexattr_destroy(guest_mutexattr **a) { return mutexattr_destroy(a); }
static SYSV int32_t ps4_pthread_mutexattr_settype(guest_mutexattr **a, int32_t t) { return mutexattr_settype(a, t); }
static SYSV int32_t ps4_pthread_mutexattr_setprotocol(guest_mutexattr **a, int32_t p) { return mutexattr_setprotocol(a, p); }

/* ---- condition variables ---------------------------------------------- */

typedef struct guest_cond { plat_cond *pc; } guest_cond;

static guest_cond *cond_get(guest_cond **h) {
    if (!h) return NULL;
    guest_cond *c = *(guest_cond *volatile *)h;
    if (c) return c;
    guest_cond *fresh = calloc(1, sizeof(*fresh));
    fresh->pc = plat_cond_new();
    guest_cond *expected = NULL;
    if (atomic_compare_exchange_strong((_Atomic(guest_cond *) *)h, &expected, fresh)) return fresh;
    plat_cond_free(fresh->pc);
    free(fresh);
    return expected;
}

static int cond_init(guest_cond **h) {
    if (!h) return PS4_EINVAL;
    *h = NULL;
    return cond_get(h) ? 0 : PS4_ENOMEM;
}

static int cond_destroy(guest_cond **h) {
    if (!h) return PS4_EINVAL;
    if (*h) {
        plat_cond_free((*h)->pc);
        free(*h);
    }
    *h = NULL;
    return 0;
}

static int cond_wait(guest_cond **ch, guest_mutex **mh, uint64_t timeout_us) {
    guest_cond *c = cond_get(ch);
    guest_mutex *m = mutex_get(mh);
    if (!c || !m) return PS4_EINVAL;
    if (atomic_load(&m->owner) != self()) return PS4_EPERM;
    uint32_t count = m->count;
    atomic_store(&m->owner, NULL);
    bool ok = plat_cond_wait(c->pc, m->pm, timeout_us);
    atomic_store(&m->owner, self());
    m->count = count;
    return ok ? 0 : PS4_ETIMEDOUT;
}

static SYSV int32_t ps4_scePthreadCondInit(guest_cond **h, const void *attr, const char *name) {
    (void)attr; (void)name;
    return sce(cond_init(h));
}
static SYSV int32_t ps4_scePthreadCondDestroy(guest_cond **h) { return sce(cond_destroy(h)); }
static SYSV int32_t ps4_scePthreadCondWait(guest_cond **c, guest_mutex **m) {
    return sce(cond_wait(c, m, UINT64_MAX));
}
static SYSV int32_t ps4_scePthreadCondTimedwait(guest_cond **c, guest_mutex **m, uint32_t usec) {
    return sce(cond_wait(c, m, usec));
}
static SYSV int32_t ps4_scePthreadCondSignal(guest_cond **h) {
    guest_cond *c = cond_get(h);
    if (!c) return SCE_KERNEL_ERROR(PS4_EINVAL);
    plat_cond_signal(c->pc);
    return SCE_OK;
}
static SYSV int32_t ps4_scePthreadCondBroadcast(guest_cond **h) {
    guest_cond *c = cond_get(h);
    if (!c) return SCE_KERNEL_ERROR(PS4_EINVAL);
    plat_cond_broadcast(c->pc);
    return SCE_OK;
}

typedef struct ps4_timespec { int64_t tv_sec, tv_nsec; } ps4_timespec;

static SYSV int32_t ps4_pthread_cond_init(guest_cond **h, const void *attr) {
    (void)attr;
    return cond_init(h);
}
static SYSV int32_t ps4_pthread_cond_destroy(guest_cond **h) { return cond_destroy(h); }
static SYSV int32_t ps4_pthread_cond_wait(guest_cond **c, guest_mutex **m) {
    return cond_wait(c, m, UINT64_MAX);
}
static SYSV int32_t ps4_pthread_cond_timedwait(guest_cond **c, guest_mutex **m,
                                               const ps4_timespec *abstime) {
    if (!abstime) return PS4_EINVAL;
    uint64_t deadline = (uint64_t)abstime->tv_sec * 1000000000 + (uint64_t)abstime->tv_nsec;
    uint64_t now = plat_realtime_ns();
    return cond_wait(c, m, deadline > now ? (deadline - now) / 1000 : 0);
}
static SYSV int32_t ps4_pthread_cond_broadcast(guest_cond **h) {
    guest_cond *c = cond_get(h);
    if (!c) return PS4_EINVAL;
    plat_cond_broadcast(c->pc);
    return 0;
}

/* ---- rwlocks (exclusive for now: correct, if less concurrent) ---------- */

static SYSV int32_t ps4_scePthreadRwlockInit(guest_mutex **h, const void *attr, const char *name) {
    (void)attr;
    if (!h) return SCE_KERNEL_ERROR(PS4_EINVAL);
    *h = mutex_new(MUTEX_RECURSIVE, name);
    return SCE_OK;
}
static SYSV int32_t ps4_scePthreadRwlockDestroy(guest_mutex **h) { return sce(mutex_destroy(h)); }
static SYSV int32_t ps4_scePthreadRwlockRdlock(guest_mutex **h) { return sce(mutex_lock(h, false)); }
static SYSV int32_t ps4_scePthreadRwlockWrlock(guest_mutex **h) { return sce(mutex_lock(h, false)); }
static SYSV int32_t ps4_scePthreadRwlockTryrdlock(guest_mutex **h) { return sce(mutex_lock(h, true)); }
static SYSV int32_t ps4_scePthreadRwlockTrywrlock(guest_mutex **h) { return sce(mutex_lock(h, true)); }
static SYSV int32_t ps4_scePthreadRwlockUnlock(guest_mutex **h) { return sce(mutex_unlock(h)); }

/* ---- tables ------------------------------------------------------------ */

HLE_LIBRARY(hle_kernel_thr, "libkernel",
    HLE(scePthreadAttrInit), HLE(scePthreadAttrDestroy), HLE(scePthreadAttrGet),
    HLE(scePthreadAttrSetstacksize), HLE(scePthreadAttrSetdetachstate),
    HLE(scePthreadAttrSetinheritsched), HLE(scePthreadAttrSetschedpolicy),
    HLE(scePthreadAttrSetschedparam), HLE(scePthreadAttrSetaffinity),
    HLE(scePthreadAttrGetstacksize), HLE(scePthreadAttrGetdetachstate),
    HLE(scePthreadAttrGetaffinity),
    HLE(scePthreadCreate), HLE(scePthreadJoin), HLE(scePthreadDetach), HLE(scePthreadExit),
    HLE(scePthreadSelf), HLE(scePthreadEqual), HLE(scePthreadGetthreadid), HLE(scePthreadYield),
    HLE(scePthreadGetprio), HLE(scePthreadSetprio), HLE(scePthreadGetaffinity),
    HLE(scePthreadSetaffinity),
    HLE(scePthreadKeyCreate), HLE(scePthreadKeyDelete), HLE(scePthreadGetspecific),
    HLE(scePthreadSetspecific), HLE(scePthreadOnce),
    HLE(scePthreadMutexInit), HLE(scePthreadMutexDestroy), HLE(scePthreadMutexLock),
    HLE(scePthreadMutexTrylock), HLE(scePthreadMutexUnlock),
    HLE(scePthreadMutexattrInit), HLE(scePthreadMutexattrDestroy),
    HLE(scePthreadMutexattrSettype), HLE(scePthreadMutexattrSetprotocol),
    HLE(scePthreadCondInit), HLE(scePthreadCondDestroy), HLE(scePthreadCondWait),
    HLE(scePthreadCondTimedwait), HLE(scePthreadCondSignal), HLE(scePthreadCondBroadcast),
    HLE(scePthreadRwlockInit), HLE(scePthreadRwlockDestroy), HLE(scePthreadRwlockRdlock),
    HLE(scePthreadRwlockWrlock), HLE(scePthreadRwlockTryrdlock),
    HLE(scePthreadRwlockTrywrlock), HLE(scePthreadRwlockUnlock),
    HLE(sched_yield),
)

HLE_LIBRARY(hle_posix, "libScePosix",
    HLE(pthread_attr_init), HLE(pthread_attr_destroy), HLE(pthread_attr_setstacksize),
    HLE(pthread_create), HLE(pthread_join), HLE_AS("pthread_exit", ps4_scePthreadExit),
    HLE_AS("pthread_self", ps4_scePthreadSelf), HLE(pthread_cancel),
    HLE(pthread_getschedparam), HLE(pthread_setschedparam),
    HLE(sched_get_priority_max), HLE(sched_get_priority_min), HLE(sched_yield),
    HLE(pthread_key_create), HLE(pthread_key_delete),
    HLE_AS("pthread_getspecific", ps4_scePthreadGetspecific), HLE(pthread_setspecific),
    HLE(pthread_mutex_init), HLE(pthread_mutex_destroy), HLE(pthread_mutex_lock),
    HLE(pthread_mutex_trylock), HLE(pthread_mutex_unlock),
    HLE(pthread_mutexattr_init), HLE(pthread_mutexattr_destroy),
    HLE(pthread_mutexattr_settype), HLE(pthread_mutexattr_setprotocol),
    HLE(pthread_cond_init), HLE(pthread_cond_destroy), HLE(pthread_cond_wait),
    HLE(pthread_cond_timedwait), HLE(pthread_cond_broadcast),
)
