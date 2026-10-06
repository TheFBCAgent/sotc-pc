/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Windows: guest TLS, threads, sync, time and sleep. */
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdlib.h>
#include <x86intrin.h>

#include "log.h"
#include "platform.h"

/* ---- guest TLS --------------------------------------------------------- */

/* The TEB holds 64 inline TLS slots at gs:[0x1480 + 8 * slot]. A rewritten
 * `mov reg, fs:[0]` becomes `mov reg, gs:[0x1480 + 8 * slot]`, the same
 * length, with no stack use (so the guest's red zone is never touched). */
static DWORD g_tls_slot = TLS_OUT_OF_INDEXES;

bool plat_tls_init(void) {
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

void plat_yield(void) { SwitchToThread(); }

void plat_sleep_us(uint64_t us) {
    /* Sleep() has ~1 ms resolution; good enough until timing matters. */
    Sleep((DWORD)((us + 999) / 1000));
}

uint64_t plat_thread_id(void) { return GetCurrentThreadId(); }

/* ---- sync -------------------------------------------------------------- */

struct plat_mutex { CRITICAL_SECTION cs; };
struct plat_cond { CONDITION_VARIABLE cv; };

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
    return c;
}
void plat_cond_free(plat_cond *c) { free(c); }

bool plat_cond_wait(plat_cond *c, plat_mutex *m, uint64_t timeout_us) {
    DWORD ms = timeout_us == UINT64_MAX ? INFINITE : (DWORD)((timeout_us + 999) / 1000);
    return SleepConditionVariableCS(&c->cv, &m->cs, ms) != 0;
}
void plat_cond_signal(plat_cond *c) { WakeConditionVariable(&c->cv); }
void plat_cond_broadcast(plat_cond *c) { WakeAllConditionVariable(&c->cv); }

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
