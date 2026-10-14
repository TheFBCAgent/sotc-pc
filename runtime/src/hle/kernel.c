/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libkernel: process info, time, debugging and other small services. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../log.h"
#include "../runtime.h"
#include "../tls.h"
#include "hle.h"
#include "caller_trace.h"
#include "../prof.h"

typedef struct ps4_timespec { int64_t tv_sec, tv_nsec; } ps4_timespec;
typedef struct ps4_timeval { int64_t tv_sec, tv_usec; } ps4_timeval;

/* ---- data imports ------------------------------------------------------ */

static uint64_t ps4___stack_chk_guard = 0x2a5f8d3c9b16e047ull;
static const char *ps4___progname = "eboot.bin";

static SYSV void ps4___stack_chk_fail(void) {
    log_fatal("guest stack smashing detected (__stack_chk_fail)");
}

static SYSV int *ps4___error(void) { return hle_errno(); }

static SYSV void ps4__exit(int code) {
    log_info("guest called _exit(%d)", code);
    fflush(NULL);
    _Exit(code);
}

/* ---- process ----------------------------------------------------------- */

static SYSV void *ps4_sceKernelGetProcParam(void) { return g_rt.procparam; }

static SYSV int32_t ps4_sceKernelIsNeoMode(void) {
    /* PS4 Pro. Off by default until the renderer can cope with Neo resolutions. */
    const char *env = getenv("SOTC_NEO");
    return env && *env == '1';
}

static SYSV int32_t ps4_sceKernelIsAddressSanitizerEnabled(void) { return 0; }
static SYSV int32_t ps4_getpagesize(void) { return 16384; }

static SYSV int32_t ps4___elf_phdr_match_addr(void *phdr_info, uint64_t addr) {
    (void)phdr_info; (void)addr;
    return 0;
}

static SYSV void ps4___pthread_cxa_finalize(void *phdr_info) { (void)phdr_info; }

typedef struct module_info_for_unwind {
    uint64_t st_size;
    char name[256];
    uint64_t eh_frame_hdr_addr;
    uint64_t eh_frame_addr;
    uint64_t eh_frame_size;
    uint64_t seg0_addr;
    uint64_t seg0_size;
} module_info_for_unwind;

static SYSV int32_t ps4_sceKernelGetModuleInfoForUnwind(uint64_t addr, int32_t flags,
                                                        module_info_for_unwind *info) {
    if (flags >= 3) return SCE_KERNEL_ERROR(PS4_EINVAL);
    if (!info) return SCE_KERNEL_ERROR(PS4_EFAULT);
    if (info->st_size < sizeof(*info)) return SCE_KERNEL_ERROR(PS4_EINVAL);
    const guest_module *m = loader_module_at((void *)(uintptr_t)addr);
    if (!m) return SCE_KERNEL_ERROR(PS4_ESRCH);
    uint64_t size = info->st_size;
    memset(info, 0, sizeof(*info));
    info->st_size = size;
    strncpy(info->name, m->name, sizeof(info->name) - 1);
    info->eh_frame_hdr_addr = (uint64_t)(uintptr_t)m->eh_frame_hdr;
    info->eh_frame_addr = (uint64_t)(uintptr_t)m->eh_frame;
    info->eh_frame_size = m->eh_frame_size;
    info->seg0_addr = (uint64_t)(uintptr_t)m->base;
    info->seg0_size = m->size;
    return SCE_OK;
}

/* ---- time -------------------------------------------------------------- */

static uint64_t g_process_start_ns;

static void fill_timespec(int32_t clock, ps4_timespec *ts) {
    /* 0 = CLOCK_REALTIME; everything else is treated as monotonic. */
    uint64_t ns = clock == 0 ? plat_realtime_ns() : plat_monotonic_ns();
    ts->tv_sec = (int64_t)(ns / 1000000000);
    ts->tv_nsec = (int64_t)(ns % 1000000000);
}

static SYSV int32_t ps4_clock_gettime(int32_t clock, ps4_timespec *ts) {
    if (!ts) { *hle_errno() = PS4_EFAULT; return -1; }
    fill_timespec(clock, ts);
    return 0;
}

static SYSV int32_t ps4_sceKernelClockGettime(int32_t clock, ps4_timespec *ts) {
    if (!ts) return SCE_KERNEL_ERROR(PS4_EFAULT);
    fill_timespec(clock, ts);
    return SCE_OK;
}

static SYSV int32_t ps4_gettimeofday(ps4_timeval *tv, void *tz) {
    (void)tz;
    if (tv) {
        uint64_t ns = plat_realtime_ns();
        tv->tv_sec = (int64_t)(ns / 1000000000);
        tv->tv_usec = (int64_t)(ns % 1000000000 / 1000);
    }
    return 0;
}

static SYSV int32_t ps4_sceKernelGettimeofday(ps4_timeval *tv) {
    if (!tv) return SCE_KERNEL_ERROR(PS4_EFAULT);
    ps4_gettimeofday(tv, NULL);
    return SCE_OK;
}

static SYSV int32_t ps4_nanosleep(const ps4_timespec *req, ps4_timespec *rem) {
    CALLER_TRACE("nanosleep");
    if (!req) { *hle_errno() = PS4_EFAULT; return -1; }
    PROF_ZONE(z, "guest sleep");
    plat_sleep_us((uint64_t)req->tv_sec * 1000000 + (uint64_t)req->tv_nsec / 1000);
    PROF_ZONE_END(z);
    if (rem) rem->tv_sec = rem->tv_nsec = 0;
    return 0;
}

static SYSV int32_t ps4_sceKernelUsleep(uint32_t us) {
    CALLER_TRACE("sceKernelUsleep");
    PROF_ZONE(z, "guest sleep");
    plat_sleep_us(us);
    PROF_ZONE_END(z);
    return SCE_OK;
}

static SYSV int32_t ps4_usleep(uint32_t us) {
    CALLER_TRACE("usleep");
    PROF_ZONE(z, "guest sleep");
    plat_sleep_us(us);
    PROF_ZONE_END(z);
    return 0;
}

static SYSV int32_t ps4_sceKernelSleep(uint32_t seconds) {
    plat_sleep_us((uint64_t)seconds * 1000000);
    return SCE_OK;
}

static SYSV uint64_t ps4_sceKernelGetProcessTime(void) {
    CALLER_TRACE("sceKernelGetProcessTime");
    return (plat_monotonic_ns() - g_process_start_ns) / 1000;
}

static SYSV uint64_t ps4_sceKernelReadTsc(void) { return plat_tsc(); }
static SYSV uint64_t ps4_sceKernelGetTscFrequency(void) { return plat_tsc_frequency(); }

static SYSV int32_t ps4_getrusage(int32_t who, void *usage) {
    (void)who;
    if (usage) memset(usage, 0, 144); /* struct rusage */
    return 0;
}

/* ---- misc -------------------------------------------------------------- */

/* The SDK version the eboot was built with (its procparam; the firmware
 * libSceNgs2 asks). */
static SYSV int32_t ps4_sceKernelGetCompiledSdkVersion(int32_t *version) {
    if (!version) return SCE_KERNEL_ERROR(PS4_EINVAL);
    *version = (int32_t)g_rt.sdk_version;
    return SCE_OK;
}

static SYSV int32_t ps4_sceKernelUuidCreate(uint8_t uuid[16]) {
    if (!uuid) return SCE_KERNEL_ERROR(PS4_EINVAL);
    uint64_t a = plat_tsc() * 0x9E3779B97F4A7C15ull, b = plat_realtime_ns() ^ (a >> 7);
    memcpy(uuid, &a, 8);
    memcpy(uuid + 8, &b, 8);
    uuid[6] = (uint8_t)((uuid[6] & 0x0F) | 0x40);
    uuid[8] = (uint8_t)((uuid[8] & 0x3F) | 0x80);
    return SCE_OK;
}

static SYSV int32_t ps4_sceKernelDebugRaiseException(int32_t error, int32_t code) {
    log_fatal("guest raised exception: error %#x code %#x", error, code);
}

static SYSV int32_t ps4_sceKernelDebugRaiseExceptionOnReleaseMode(int32_t error, int32_t code) {
    log_fatal("guest raised exception (release mode): error %#x code %#x", error, code);
}

static SYSV void ps4_sceKernelPrintBacktraceWithModuleInfo(void) {
    log_warn("guest requested a backtrace");
}

/* Signals: the game only installs handlers and masks; nothing is delivered. */
static SYSV int32_t ps4_zero(void) { return 0; }

static SYSV int32_t ps4_sigfillset(uint32_t *set) {
    if (set) memset(set, 0xFF, 16);
    return 0;
}

HLE_LIBRARY(hle_kernel, "libkernel",
    HLE_AS("__stack_chk_guard", &ps4___stack_chk_guard),
    HLE_AS("__progname", &ps4___progname),
    HLE(__stack_chk_fail),
    HLE(__error),
    HLE(_exit),
    HLE(sceKernelGetProcParam),
    HLE(sceKernelIsNeoMode),
    HLE(sceKernelIsAddressSanitizerEnabled),
    HLE(getpagesize),
    HLE(__elf_phdr_match_addr),
    HLE(__pthread_cxa_finalize),
    HLE(sceKernelGetModuleInfoForUnwind),
    HLE(clock_gettime),
    HLE(sceKernelClockGettime),
    HLE(gettimeofday),
    HLE(sceKernelGettimeofday),
    HLE(nanosleep),
    HLE_AS("_nanosleep", ps4_nanosleep),
    HLE(sceKernelUsleep),
    HLE(usleep),
    HLE(sceKernelSleep),
    HLE(sceKernelGetProcessTime),
    HLE(sceKernelReadTsc),
    HLE(sceKernelGetTscFrequency),
    HLE(getrusage),
    HLE(sceKernelUuidCreate),
    HLE(sceKernelGetCompiledSdkVersion),
    HLE(sceKernelDebugRaiseException),
    HLE(sceKernelDebugRaiseExceptionOnReleaseMode),
    HLE(sceKernelPrintBacktraceWithModuleInfo),
    HLE(sigfillset),
    HLE_AS("sigprocmask", ps4_zero),
    HLE_AS("_sigprocmask", ps4_zero),
    HLE_AS("signal", ps4_zero),
    HLE_AS("_is_signal_return", ps4_zero),
    HLE_AS("sigreturn", ps4_zero),
    HLE_AS("__tls_get_addr", ps4___tls_get_addr),
    HLE_AS("_sceKernelRtldSetApplicationHeapAPI", ps4__sceKernelRtldSetApplicationHeapAPI),
)

/* Called once before the guest starts. */
void hle_kernel_init(void) { g_process_start_ns = plat_monotonic_ns(); }
