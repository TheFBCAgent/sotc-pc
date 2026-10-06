/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SOTC_LOG_H
#define SOTC_LOG_H

/* Levels: 0 error, 1 warn, 2 info, 3 debug, 4 trace. SOTC_LOG=<n> sets it. */
void log_init(void);
int log_level(void);
void log_write(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
__attribute__((noreturn)) void log_fatal(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

#define log_error(...) log_write(0, __VA_ARGS__)
#define log_warn(...) log_write(1, __VA_ARGS__)
#define log_info(...) log_write(2, __VA_ARGS__)
#define log_debug(...) do { if (log_level() >= 3) log_write(3, __VA_ARGS__); } while (0)
#define log_trace(...) do { if (log_level() >= 4) log_write(4, __VA_ARGS__); } while (0)

#endif
