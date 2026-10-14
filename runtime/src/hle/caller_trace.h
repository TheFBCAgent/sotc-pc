/* Caller trace (debug, SOTC_CALLER_TRACE=1): counts the guest call sites of a few HLE functions
 * (frame pacing candidates: sleeps, vblank and flip waits, event-queue waits, submits) and logs
 * them once a second as `caller: <fn> eboot+0x<offset> <n>/s (tid ...)`. Session 69: to find
 * where the game paces its frames (Pro Performance vs Cinematic). Free when unset. */
#pragma once

#include <stdbool.h>

extern bool g_caller_trace_on;

void caller_trace_init(void);
void caller_trace_note(const char *fn, void *ret);

/* Use at the top of a SYSV HLE function the guest calls directly. */
#define CALLER_TRACE(fn)                                                                        \
    do {                                                                                        \
        if (g_caller_trace_on) caller_trace_note((fn), __builtin_return_address(0));           \
    } while (0)
