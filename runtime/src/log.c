/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "platform.h"

static int g_level = 2;
static plat_mutex *g_lock;
static uint64_t g_start_ns;

void log_init(void) {
    const char *env = getenv("SOTC_LOG");
    if (env) g_level = atoi(env);
    g_lock = plat_mutex_new();
    g_start_ns = plat_monotonic_ns();
}

int log_level(void) { return g_level; }

static void vwrite(int level, const char *fmt, va_list ap) {
    static const char *names[] = {"ERROR", "WARN ", "INFO ", "DEBUG", "TRACE"};
    double t = (double)(plat_monotonic_ns() - g_start_ns) / 1e9;
    if (g_lock) plat_mutex_lock(g_lock);
    fprintf(stderr, "[%9.4f] %s t%-6llu ", t, names[level < 0 ? 0 : level > 4 ? 4 : level],
            (unsigned long long)plat_thread_id());
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    if (g_lock) plat_mutex_unlock(g_lock);
}

void log_write(int level, const char *fmt, ...) {
    if (level > g_level) return;
    va_list ap;
    va_start(ap, fmt);
    vwrite(level, fmt, ap);
    va_end(ap);
}

void log_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vwrite(0, fmt, ap);
    va_end(ap);
    exit(1);
}
