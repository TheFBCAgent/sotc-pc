/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Windows: guest TLS, threads, sync, time and sleep. */
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x86intrin.h>

#include "log.h"
#include "platform.h"

/* timeBeginPeriod lives in winmm. CMakeLists.txt is shared, so the library is
 * requested here (honoured by lld-link and link.exe). */
#pragma comment(lib, "winmm.lib")
__declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int period);

static void timing_init(void);

/* ---- guest TLS --------------------------------------------------------- */

/* The TEB holds 64 inline TLS slots at gs:[0x1480 + 8 * slot]. A rewritten
 * `mov reg, fs:[0]` becomes `mov reg, gs:[0x1480 + 8 * slot]`, the same
 * length, with no stack use (so the guest's red zone is never touched). */
static DWORD g_tls_slot = TLS_OUT_OF_INDEXES;

bool plat_tls_init(void) {
    /* main.c calls plat_tls_init first thing at startup, so the process-wide
     * timer setup lives here. */
    timing_init();
    g_tls_slot = TlsAlloc();
    if (g_tls_slot == TLS_OUT_OF_INDEXES || g_tls_slot >= 64) {
        log_error("no inline TEB TLS slot available (got %lu)", (unsigned long)g_tls_slot);
        return false;
    }
    return true;
}

uint32_t plat_tls_patch_disp(int32_t fs_disp) {
    /* Only fs:[0] (the TCB self pointer) maps onto a single TEB slot read. */
    return fs_disp == 0 ? 0x1480u + 8u * g_tls_slot : 0;
}

void plat_set_guest_tcb(void *tcb) {
    TlsSetValue(g_tls_slot, tcb);
}

/* ---- threads ----------------------------------------------------------- */

struct plat_thread {
    HANDLE handle;
    plat_thread_fn fn;
    void *arg;
};

static DWORD WINAPI thread_main(LPVOID p) {
    plat_thread *t = p;
    t->fn(t->arg);
    return 0;
}

plat_thread *plat_thread_create(plat_thread_fn fn, void *arg, size_t stack_size) {
    plat_thread *t = calloc(1, sizeof(*t));
    t->fn = fn;
    t->arg = arg;
    /* Commit the whole stack up front, as the PS4 kernel does. Guest code
     * moves rsp down by large frames without touching every page in between
     * (no __chkstk), which would skip Windows' guard page and fault with no
     * room left to dispatch the exception. */
    t->handle = CreateThread(NULL, stack_size, thread_main, t, 0, NULL);
    if (!t->handle) {
        free(t);
        return NULL;
    }
    return t;
}

void plat_thread_join(plat_thread *t) {
    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
    free(t);
}

void plat_thread_detach(plat_thread *t) {
    CloseHandle(t->handle);
    /* `t` is still read by thread_main; it is leaked deliberately (tiny). */
}

void plat_thread_set_name(const char *name) {
    wchar_t wide[64];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 64);
    wide[63] = 0;
    SetThreadDescription(GetCurrentThread(), wide);
}

/* ---- precise sleep (workstream P, M3) ------------------------------------
 * - timeBeginPeriod(1) (timing_init) makes Sleep() and the millisecond kernel
 *   waits (condition variables, WaitForSingleObject) wake within ~1 ms instead
 *   of the default 15.6 ms tick.
 * - A wait of PLAT_TIMER_MIN_NS or more sleeps on a per-thread
 *   high-resolution waitable timer (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
 *   Windows 10 1803+) aimed PLAT_TIMER_SLACK_NS early, then spins (yielding)
 *   on the monotonic clock for the rest. Shorter waits only spin. */
#define PLAT_TIMER_MIN_NS 1000000ull
#define PLAT_TIMER_SLACK_NS 300000ull /* initial guess; see timer_slack_ns */

static DWORD g_timer_fls = FLS_OUT_OF_INDEXES;
static bool g_timer_highres = true;

static void WINAPI timer_fls_free(void *h) {
    if (h) CloseHandle(h);
}

/* The calling thread's timer, created on first use and closed when the thread
 * exits (FLS destructor). The runtime's fibers don't use Windows fibers, so
 * FLS is per thread here. */
static HANDLE thread_timer(void) {
    if (g_timer_fls == FLS_OUT_OF_INDEXES) return NULL;
    HANDLE h = FlsGetValue(g_timer_fls);
    if (h) return h;
    if (g_timer_highres)
        h = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                   TIMER_ALL_ACCESS);
    if (!h) {
        g_timer_highres = false; /* before 1803: a plain timer follows timeBeginPeriod */
        h = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    }
    if (h) FlsSetValue(g_timer_fls, h);
    return h;
}

static void timing_stats_init(void);

static INIT_ONCE g_timing_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK timing_init_once(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once, (void)param, (void)ctx;
    timeBeginPeriod(1); /* never ended: it lasts as long as the process */
    g_timer_fls = FlsAlloc(timer_fls_free);
    timing_stats_init();
    return TRUE;
}

/* Runs at startup (plat_tls_init) and, for tests that skip that, on the
 * first sleep. */
static void timing_init(void) { InitOnceExecuteOnce(&g_timing_once, timing_init_once, NULL, NULL); }

/* How late timer wakeups run (a moving average shared by all threads; racy
 * updates are harmless). The timer aims that much plus a margin early, so
 * sleeps end on the spin, not late, while the spin stays short when the
 * timer is accurate. Measured on the dev machine (Win10 19045): 0.1-0.6 ms. */
static _Atomic uint64_t g_timer_late_ns = PLAT_TIMER_SLACK_NS;

static void timer_observe(uint64_t late_ns) {
    if (late_ns > 4000000) late_ns = 4000000; /* preempted: don't let one outlier dominate */
    uint64_t old = atomic_load_explicit(&g_timer_late_ns, memory_order_relaxed);
    atomic_store_explicit(&g_timer_late_ns, (old * 15 + late_ns) / 16, memory_order_relaxed);
}

static uint64_t timer_slack_ns(void) {
    uint64_t est = atomic_load_explicit(&g_timer_late_ns, memory_order_relaxed);
    uint64_t slack = est + est / 2 + 100000;
    return slack < 200000 ? 200000 : slack > 2000000 ? 2000000 : slack;
}

/* Spin until `deadline`, handing the core to other ready threads meanwhile. */
static void spin_until(uint64_t deadline) {
    while (plat_monotonic_ns() < deadline) {
        if (!SwitchToThread()) _mm_pause();
    }
}

static void sleep_until(uint64_t deadline) {
    timing_init();
    for (;;) {
        uint64_t now = plat_monotonic_ns();
        if (now >= deadline) return;
        uint64_t left = deadline - now;
        if (left < PLAT_TIMER_MIN_NS) {
            spin_until(deadline);
            return;
        }
        uint64_t slack = timer_slack_ns();
        if (left <= slack) {
            spin_until(deadline);
            return;
        }
        uint64_t nap = left - slack;
        HANDLE t = thread_timer();
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(nap / 100); /* relative, in 100 ns units */
        if (t && SetWaitableTimerEx(t, &due, 0, NULL, NULL, NULL, 0)) {
            WaitForSingleObject(t, INFINITE);
            uint64_t woke = plat_monotonic_ns(), target = now + nap;
            timer_observe(woke > target ? woke - target : 0);
        } else {
            Sleep((DWORD)(nap / 1000000));
        }
    }
}

/* ---- wait accounting (SOTC_TIMING=1) ------------------------------------
 * Per call site (the return address into the runtime), count the sleeps,
 * yields and condition waits and the wall time spent in them, and dump the
 * table to the log every 10 s. Off by default: one well-predicted branch per
 * call. Resolve `sotc-runtime.exe+off` with
 * `llvm-symbolizer --obj=build/sotc-runtime.exe <0x140000000 + off>`. */
enum { TS_SLEEP, TS_YIELD, TS_CWAIT, TS_CWAIT_TIMED, TS_KINDS };
static const char *const ts_kind_name[TS_KINDS] = {"sleep", "yield", "cond", "cond-timed"};
#define TS_SLOTS 256
#define TS_DUMP_NS 10000000000ull
typedef struct ts_slot {
    _Atomic uint64_t key; /* return address | kind << 60 */
    _Atomic uint64_t count, ns, late_ns;
} ts_slot;
static ts_slot g_ts[TS_SLOTS];
static bool g_ts_on;
static _Atomic uint64_t g_ts_next_dump;
static uint64_t g_ts_start;

static void timing_stats_init(void) {
    const char *e = getenv("SOTC_TIMING");
    g_ts_on = e && *e && *e != '0';
    g_ts_start = plat_monotonic_ns();
    atomic_store(&g_ts_next_dump, g_ts_start + TS_DUMP_NS);
}

static void ts_describe(uint64_t addr, char *buf, size_t n) {
    HMODULE mod = NULL;
    char path[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)addr, &mod) &&
        GetModuleFileNameA(mod, path, sizeof(path))) {
        const char *base = strrchr(path, '\\');
        snprintf(buf, n, "%s+%#llx", base ? base + 1 : path,
                 (unsigned long long)(addr - (uint64_t)(uintptr_t)mod));
    } else {
        snprintf(buf, n, "%#llx", (unsigned long long)addr);
    }
}

static void ts_dump(uint64_t now) {
    static int idx[TS_SLOTS]; /* only one dumper at a time (CAS on g_ts_next_dump) */
    int n = 0;
    for (int i = 0; i < TS_SLOTS; i++)
        if (atomic_load(&g_ts[i].key)) idx[n++] = i;
    for (int i = 1; i < n; i++) /* by total wall time, descending */
        for (int j = i; j > 0 && atomic_load(&g_ts[idx[j]].ns) > atomic_load(&g_ts[idx[j - 1]].ns);
             j--) {
            int t = idx[j];
            idx[j] = idx[j - 1];
            idx[j - 1] = t;
        }
    log_info("timing: waits at %.1f s (cumulative, top 24 call sites by wall time)",
             (double)(now - g_ts_start) / 1e9);
    for (int i = 0; i < n && i < 24; i++) {
        ts_slot *s = &g_ts[idx[i]];
        uint64_t key = atomic_load(&s->key), cnt = atomic_load(&s->count);
        char where[96];
        ts_describe(key & ((1ull << 60) - 1), where, sizeof(where));
        log_info("timing:   %-10s %-30s n=%-9llu total=%8.3f s mean=%9.1f us late=%7.1f us",
                 ts_kind_name[key >> 60], where, (unsigned long long)cnt,
                 (double)atomic_load(&s->ns) / 1e9,
                 cnt ? (double)atomic_load(&s->ns) / 1e3 / (double)cnt : 0.0,
                 cnt ? (double)atomic_load(&s->late_ns) / 1e3 / (double)cnt : 0.0);
    }
}

static void ts_record(int kind, void *ret, uint64_t t0, uint64_t t1, uint64_t deadline) {
    uint64_t key = (uint64_t)(uintptr_t)ret | (uint64_t)kind << 60;
    uint64_t h = (key * 0x9E3779B97F4A7C15ull) >> 56;
    for (int probe = 0; probe < TS_SLOTS; probe++) {
        ts_slot *s = &g_ts[(h + (uint64_t)probe) % TS_SLOTS];
        uint64_t k = atomic_load(&s->key);
        if (k == 0) {
            uint64_t expect = 0;
            if (!atomic_compare_exchange_strong(&s->key, &expect, key) && expect != key) continue;
        } else if (k != key) {
            continue;
        }
        atomic_fetch_add(&s->count, 1);
        atomic_fetch_add(&s->ns, t1 - t0);
        if (deadline && t1 > deadline) atomic_fetch_add(&s->late_ns, t1 - deadline);
        break;
    }
    uint64_t next = atomic_load(&g_ts_next_dump);
    if (t1 >= next && atomic_compare_exchange_strong(&g_ts_next_dump, &next, t1 + TS_DUMP_NS))
        ts_dump(t1);
}

#define TS_BEGIN() uint64_t ts_t0 = g_ts_on ? plat_monotonic_ns() : 0
#define TS_END(kind, deadline)                                                              \
    do {                                                                                    \
        if (g_ts_on)                                                                        \
            ts_record(kind, __builtin_return_address(0), ts_t0, plat_monotonic_ns(),        \
                      deadline);                                                            \
    } while (0)

void plat_sleep_until_ns(uint64_t deadline_ns) {
    TS_BEGIN();
    sleep_until(deadline_ns);
    TS_END(TS_SLEEP, deadline_ns);
}

void plat_sleep_us(uint64_t us) {
    TS_BEGIN();
    uint64_t deadline = us > (UINT64_MAX / 2) / 1000 ? UINT64_MAX / 2
                                                     : plat_monotonic_ns() + us * 1000;
    sleep_until(deadline);
    TS_END(TS_SLEEP, deadline);
}

void plat_yield(void) {
    TS_BEGIN();
    SwitchToThread();
    TS_END(TS_YIELD, 0);
}

uint64_t plat_thread_id(void) { return GetCurrentThreadId(); }

/* ---- sync -------------------------------------------------------------- */

struct plat_mutex { CRITICAL_SECTION cs; };
/* `seq` counts signals and broadcasts, so the sub-millisecond tail of a timed
 * wait can see a wakeup without sleeping on the condition variable, whose
 * timeout has only millisecond granularity. */
struct plat_cond { CONDITION_VARIABLE cv; _Atomic uint64_t seq; };

plat_mutex *plat_mutex_new(void) {
    plat_mutex *m = malloc(sizeof(*m));
    InitializeCriticalSection(&m->cs);
    return m;
}
void plat_mutex_free(plat_mutex *m) { DeleteCriticalSection(&m->cs); free(m); }
void plat_mutex_lock(plat_mutex *m) { EnterCriticalSection(&m->cs); }
bool plat_mutex_trylock(plat_mutex *m) { return TryEnterCriticalSection(&m->cs) != 0; }
void plat_mutex_unlock(plat_mutex *m) { LeaveCriticalSection(&m->cs); }

plat_cond *plat_cond_new(void) {
    plat_cond *c = malloc(sizeof(*c));
    InitializeConditionVariable(&c->cv);
    atomic_init(&c->seq, 0);
    return c;
}
void plat_cond_free(plat_cond *c) { free(c); }

/* Timed waits end within ~0.1 ms of the timeout instead of up to 2 ms late
 * (the old round-up to whole milliseconds, plus the tick) or 16 ms late
 * (without timeBeginPeriod):
 * - while PLAT_CV_TAIL_NS or more remain, sleep on the condition variable for
 *   whole milliseconds, ending at least 1 ms before the deadline;
 * - for the rest, drop the mutex and spin (yielding) until a signal or
 *   broadcast bumps `seq` or the deadline passes, then retake the mutex.
 * A signal during the tail may also wake another waiter: a spurious wakeup,
 * which condition-variable users must tolerate anyway. */
#define PLAT_CV_TAIL_NS 2000000ull

static bool cond_wait_timed(plat_cond *c, plat_mutex *m, uint64_t deadline) {
    uint64_t seq = atomic_load(&c->seq);
    uint64_t now = plat_monotonic_ns();
    while (now < deadline && deadline - now >= PLAT_CV_TAIL_NS) {
        uint64_t ms = (deadline - now - 1000000) / 1000000;
        if (SleepConditionVariableCS(&c->cv, &m->cs, ms > 0x7fffffff ? 0x7fffffff : (DWORD)ms))
            return true;
        now = plat_monotonic_ns();
    }
    if (now >= deadline) return false;
    LeaveCriticalSection(&m->cs);
    bool woken;
    while (!(woken = atomic_load(&c->seq) != seq) && plat_monotonic_ns() < deadline) {
        if (!SwitchToThread()) _mm_pause();
    }
    EnterCriticalSection(&m->cs);
    return woken;
}

bool plat_cond_wait(plat_cond *c, plat_mutex *m, uint64_t timeout_us) {
    TS_BEGIN();
    bool ok;
    if (timeout_us == UINT64_MAX) {
        ok = SleepConditionVariableCS(&c->cv, &m->cs, INFINITE) != 0;
        TS_END(TS_CWAIT, 0);
    } else {
        uint64_t deadline = timeout_us > (UINT64_MAX / 2) / 1000
                                ? UINT64_MAX / 2
                                : plat_monotonic_ns() + timeout_us * 1000;
        ok = cond_wait_timed(c, m, deadline);
        TS_END(TS_CWAIT_TIMED, ok ? 0 : deadline);
    }
    return ok;
}
void plat_cond_signal(plat_cond *c) {
    atomic_fetch_add(&c->seq, 1);
    WakeConditionVariable(&c->cv);
}
void plat_cond_broadcast(plat_cond *c) {
    atomic_fetch_add(&c->seq, 1);
    WakeAllConditionVariable(&c->cv);
}

/* ---- time -------------------------------------------------------------- */

uint64_t plat_monotonic_ns(void) {
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    uint64_t ticks = (uint64_t)now.QuadPart, f = (uint64_t)freq.QuadPart;
    return ticks / f * 1000000000ull + ticks % f * 1000000000ull / f;
}

uint64_t plat_realtime_ns(void) {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ull) * 100; /* 1601 -> 1970 epoch, 100 ns units */
}

uint64_t plat_tsc(void) { return __rdtsc(); }

uint64_t plat_tsc_frequency(void) {
    static uint64_t freq;
    if (!freq) {
        uint64_t t0 = plat_monotonic_ns(), c0 = __rdtsc();
        Sleep(50);
        uint64_t t1 = plat_monotonic_ns(), c1 = __rdtsc();
        freq = (c1 - c0) * 1000000000ull / (t1 - t0); /* < 2^64 for any real CPU */
    }
    return freq;
}

#endif
