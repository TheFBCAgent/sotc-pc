/* Caller trace (debug, SOTC_CALLER_TRACE=1); see caller_trace.h. */
#include "caller_trace.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../log.h"
#include "../platform.h"
#include "../runtime.h"

bool g_caller_trace_on;

enum { CT_SLOTS = 256 };

typedef struct ct_slot {
    const char *fn;
    uintptr_t ret;
    uint64_t tid;
    uint32_t count;
} ct_slot;

static plat_mutex *g_lock;
static ct_slot g_slots[CT_SLOTS];
static uint64_t g_window_start_ns;
static uint32_t g_dropped;

void caller_trace_init(void) {
    const char *v = getenv("SOTC_CALLER_TRACE");
    if (!v || *v != '1') return;
    g_lock = plat_mutex_new();
    g_window_start_ns = plat_monotonic_ns();
    g_caller_trace_on = true;
    log_info("caller trace on (SOTC_CALLER_TRACE=1)");
}

static void flush_locked(uint64_t now) {
    const double secs = (double)(now - g_window_start_ns) / 1e9;
    const uintptr_t base = (uintptr_t)g_rt.image;
    for (int i = 0; i < CT_SLOTS; i++) {
        ct_slot *s = &g_slots[i];
        if (!s->fn) continue;
        if (s->ret >= base && s->ret < base + g_rt.image_size)
            log_info("caller: %s image+0x%llx %.1f/s (tid %llu)", s->fn,
                     (unsigned long long)(s->ret - base), s->count / secs,
                     (unsigned long long)s->tid);
        else
            log_info("caller: %s host %p %.1f/s (tid %llu)", s->fn, (void *)s->ret,
                     s->count / secs, (unsigned long long)s->tid);
    }
    if (g_dropped) log_info("caller: %u calls dropped (table full)", g_dropped);
    memset(g_slots, 0, sizeof g_slots);
    g_dropped = 0;
    g_window_start_ns = now;
}

void caller_trace_note(const char *fn, void *ret) {
    const uint64_t tid = plat_thread_id();
    const uint64_t now = plat_monotonic_ns();
    uintptr_t h = ((uintptr_t)ret ^ (uintptr_t)fn ^ (uintptr_t)tid) * 0x9E3779B97F4A7C15ull;
    plat_mutex_lock(g_lock);
    for (int probe = 0; probe < CT_SLOTS; probe++) {
        ct_slot *s = &g_slots[(h + (uintptr_t)probe) % CT_SLOTS];
        if (!s->fn) {
            *s = (ct_slot){.fn = fn, .ret = (uintptr_t)ret, .tid = tid, .count = 1};
            goto done;
        }
        if (s->fn == fn && s->ret == (uintptr_t)ret && s->tid == tid) {
            s->count++;
            goto done;
        }
    }
    g_dropped++;
done:
    if (now - g_window_start_ns >= 1000000000ull) flush_locked(now);
    plat_mutex_unlock(g_lock);
}
