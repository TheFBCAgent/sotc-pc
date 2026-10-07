/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Workstream P (M3): sleep precision, deadline sleeps, timed condition waits
 * and AudioOut pacing.
 *
 * Bounds are generous: the machine may be busy (parallel builds, the game).
 * Each timing check takes the median of several tries, so one preempted try
 * doesn't fail the test, while the old behaviour (Sleep() rounding up to a
 * whole millisecond, or to the 15.6 ms tick) still does. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle/hle.h"

static int g_failures;
#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                         \
        }                                                                         \
    } while (0)

#define TRIES 9

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
static uint64_t median(uint64_t *v, int n) {
    qsort(v, (size_t)n, sizeof(*v), cmp_u64);
    return v[n / 2];
}

/* plat_sleep_us: never early; median overshoot below `slack_us`. */
static void check_sleep_us(uint64_t us, uint64_t slack_us) {
    uint64_t took[TRIES], min = UINT64_MAX;
    for (int i = 0; i < TRIES; i++) {
        uint64_t t0 = plat_monotonic_ns();
        plat_sleep_us(us);
        took[i] = (plat_monotonic_ns() - t0) / 1000;
        if (took[i] < min) min = took[i];
    }
    uint64_t med = median(took, TRIES);
    printf("sleep_us(%llu): median %llu us, min %llu us\n", (unsigned long long)us,
           (unsigned long long)med, (unsigned long long)min);
    CHECK(min >= us);
    CHECK(med <= us + slack_us);
}

static void test_sleep(void) {
    check_sleep_us(0, 200);
    check_sleep_us(50, 300);   /* spin path */
    check_sleep_us(500, 400);  /* spin path */
    check_sleep_us(1000, 700); /* timer + spin */
    check_sleep_us(2000, 700);
    check_sleep_us(5000, 700);
    check_sleep_us(16667, 900);
}

static void test_sleep_until(void) {
    /* A deadline in the past returns at once. */
    uint64_t t0 = plat_monotonic_ns();
    plat_sleep_until_ns(t0 - 1000000);
    plat_sleep_until_ns(0);
    CHECK(plat_monotonic_ns() - t0 < 1000000);

    /* A 60 Hz deadline loop: every wakeup is at or after its deadline, the
     * median lateness is small, and nothing drifts. */
    enum { TICKS = 30 };
    const uint64_t period = 1000000000ull / 60;
    uint64_t late[TICKS], start = plat_monotonic_ns(), next = start;
    for (int i = 0; i < TICKS; i++) {
        next += period;
        plat_sleep_until_ns(next);
        uint64_t now = plat_monotonic_ns();
        CHECK(now >= next);
        late[i] = (now - next) / 1000;
    }
    uint64_t total_us = (plat_monotonic_ns() - start) / 1000;
    uint64_t med = median(late, TICKS);
    printf("sleep_until: 30 ticks at 60 Hz took %llu us (expect 500000), median late %llu us, "
           "max %llu us\n",
           (unsigned long long)total_us, (unsigned long long)med,
           (unsigned long long)late[TICKS - 1]);
    CHECK(med <= 600);
    CHECK(total_us >= 500000 && total_us <= 500000 + 3000);
}

/* ---- timed condition waits ---------------------------------------------- */

static void check_cond_timeout(uint64_t us, uint64_t slack_us) {
    plat_mutex *m = plat_mutex_new();
    plat_cond *c = plat_cond_new();
    uint64_t took[TRIES], min = UINT64_MAX;
    int timeouts = 0;
    for (int i = 0; i < TRIES; i++) {
        plat_mutex_lock(m);
        uint64_t t0 = plat_monotonic_ns();
        bool woke = plat_cond_wait(c, m, us);
        took[i] = (plat_monotonic_ns() - t0) / 1000;
        plat_mutex_unlock(m);
        if (!woke) timeouts++;
        if (took[i] < min) min = took[i];
        CHECK(plat_mutex_trylock(m)); /* the mutex came back unlocked */
        plat_mutex_unlock(m);
    }
    uint64_t med = median(took, TRIES);
    printf("cond_wait(%llu us timeout): median %llu us, min %llu us, %d/%d timed out\n",
           (unsigned long long)us, (unsigned long long)med, (unsigned long long)min, timeouts,
           TRIES);
    /* Nobody signals: Windows may still wake a condition variable spuriously,
     * but nearly every try must time out, never early by much. */
    CHECK(timeouts >= TRIES - 1);
    CHECK(med >= us);
    CHECK(med <= us + slack_us);
    plat_cond_free(c);
    plat_mutex_free(m);
}

typedef struct signaller {
    plat_mutex *m;
    plat_cond *c;
    uint64_t delay_us;
    volatile int flag;
} signaller;

static void signal_later(void *p) {
    signaller *s = p;
    plat_sleep_us(s->delay_us);
    plat_mutex_lock(s->m);
    s->flag = 1;
    plat_cond_signal(s->c);
    plat_mutex_unlock(s->m);
}

/* A signal arriving while the waiter is in the sub-millisecond tail (or on
 * the condition variable) ends the wait well before the timeout. */
static void check_cond_signal(uint64_t timeout_us, uint64_t delay_us) {
    signaller s = {plat_mutex_new(), plat_cond_new(), delay_us, 0};
    plat_mutex_lock(s.m);
    plat_thread *t = plat_thread_create(signal_later, &s, 64 * 1024);
    CHECK(t != NULL);
    uint64_t t0 = plat_monotonic_ns();
    uint64_t deadline = t0 + timeout_us * 1000;
    bool woke = false;
    while (!s.flag) { /* the usual predicate loop, tolerating spurious wakeups */
        uint64_t now = plat_monotonic_ns();
        if (now >= deadline) break;
        woke = plat_cond_wait(s.c, s.m, (deadline - now) / 1000);
    }
    uint64_t took = (plat_monotonic_ns() - t0) / 1000;
    plat_mutex_unlock(s.m);
    plat_thread_join(t);
    printf("cond_wait(%llu us) signalled after %llu us: woke=%d flag=%d after %llu us\n",
           (unsigned long long)timeout_us, (unsigned long long)delay_us, woke, s.flag,
           (unsigned long long)took);
    CHECK(s.flag == 1);
    CHECK(took < timeout_us); /* loose: thread start-up on a busy machine */
    plat_cond_free(s.c);
    plat_mutex_free(s.m);
}

static void test_cond(void) {
    check_cond_timeout(300, 400);   /* tail only */
    check_cond_timeout(1500, 600);  /* tail only */
    check_cond_timeout(3000, 900);  /* condition variable, then the tail */
    check_cond_timeout(10000, 1200);
    check_cond_signal(1800, 300);   /* lands in the tail */
    check_cond_signal(50000, 5000); /* lands on the condition variable */
}

/* ---- AudioOut pacing ---------------------------------------------------- */

typedef SYSV int32_t (*fn_v)(void);
typedef SYSV int32_t (*fn_i)(int32_t);
typedef SYSV int32_t (*fn_open)(int32_t, int32_t, int32_t, uint32_t, uint32_t, uint32_t);
typedef SYSV int32_t (*fn_ip)(int32_t, const void *);

static void test_audio_pacing(void) {
    fn_v init = (fn_v)hle_lookup("libSceAudioOut", "sceAudioOutInit");
    fn_open open = (fn_open)hle_lookup("libSceAudioOut", "sceAudioOutOpen");
    fn_ip output = (fn_ip)hle_lookup("libSceAudioOut", "sceAudioOutOutput");
    fn_i close = (fn_i)hle_lookup("libSceAudioOut", "sceAudioOutClose");
    CHECK(init && open && output && close);
    if (!init || !open || !output || !close) return;
    CHECK(init() == 0);
    int32_t h = open(1, 0, 0, 256, 48000, 1); /* 256 frames: 5.333 ms per buffer */
    CHECK(h > 0);
    static int16_t buf[256 * 2];
    /* Each Output returns on its buffer boundary: measure every return. */
    enum { N = 61 };
    uint64_t ret[N], late[N - 1];
    for (int i = 0; i < N; i++) {
        CHECK(output(h, buf) == 512);
        ret[i] = plat_monotonic_ns();
    }
    const uint64_t buf_ns = 256ull * 1000000000ull / 48000;
    for (int i = 1; i < N; i++) {
        uint64_t due = ret[0] + (uint64_t)i * buf_ns; /* the first buffer returns at once */
        late[i - 1] = ret[i] > due ? (ret[i] - due) / 1000 : 0;
    }
    uint64_t total_us = (ret[N - 1] - ret[0]) / 1000;
    uint64_t med = median(late, N - 1);
    printf("audioout: 60 outputs after the first took %llu us (expect ~320000), "
           "median late %llu us\n",
           (unsigned long long)total_us, (unsigned long long)med);
    CHECK(total_us >= 60 * buf_ns / 1000 - 300 && total_us <= 60 * buf_ns / 1000 + 3000);
    CHECK(med <= 600);
    CHECK(close(h) == 0);
}

int main(void) {
    log_init();
    plat_tls_init(); /* timeBeginPeriod + timers, as at startup */
    hle_init();
    test_sleep();
    test_sleep_until();
    test_cond();
    test_audio_pacing();
    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_timing: ok\n");
    return 0;
}
