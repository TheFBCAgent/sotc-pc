/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libkernel odds and ends: time-zone conversion, module info by address,
 * module segment info, and libc's heap trace info.
 *
 * Semantics follow shadPS4 (GPL-2.0-or-later): kernel/time.cpp,
 * kernel/process.cpp, core/module.h and libc_internal/libc_internal_memory.cpp.
 * Argument layouts of the time-zone and segment-info calls were checked
 * against their call sites in the game's libc.prx (see docs/m2/a.md). */
#include <string.h>
#include <time.h>

#include "../runtime.h"
#include "hle.h"

/* ---- time zones ---------------------------------------------------------- */

/* SceKernelTimesec */
typedef struct ps4_timesec {
    int64_t t;
    uint32_t west_sec; /* standard offset from UTC in seconds (east positive) */
    uint32_t dst_sec;  /* daylight-saving shift in effect, seconds */
} ps4_timesec;

static bool host_localtime(int64_t t, struct tm *out) {
    time_t tt = (time_t)t;
#ifdef _WIN32
    return localtime_s(out, &tt) == 0;
#else
    return localtime_r(&tt, out) != NULL;
#endif
}

static bool host_gmtime(int64_t t, struct tm *out) {
    time_t tt = (time_t)t;
#ifdef _WIN32
    return gmtime_s(out, &tt) == 0;
#else
    return gmtime_r(&tt, out) != NULL;
#endif
}

/* Days since 1970-01-01 for a broken-down civil date (proleptic Gregorian). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static int64_t tm_to_seconds(const struct tm *tm) {
    return days_from_civil(tm->tm_year + 1900, (unsigned)tm->tm_mon + 1, (unsigned)tm->tm_mday) *
               86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
}

/* Host zone at UTC time t: total offset (east positive) and whether DST is on. */
static void zone_at(int64_t t, int64_t *offset, bool *dst) {
    struct tm lt, gt;
    if (!host_localtime(t, &lt) || !host_gmtime(t, &gt)) {
        *offset = 0;
        *dst = false;
        return;
    }
    *offset = tm_to_seconds(&lt) - tm_to_seconds(&gt);
    *dst = lt.tm_isdst > 0;
}

/* The zone's DST shift (e.g. 3600), found by comparing January and July. */
static int64_t zone_dst_shift(int64_t t) {
    struct tm gt;
    if (!host_gmtime(t, &gt)) return 0;
    int64_t y = gt.tm_year + 1900;
    int64_t o1, o2;
    bool d1, d2;
    zone_at(days_from_civil(y, 1, 1) * 86400, &o1, &d1);
    zone_at(days_from_civil(y, 7, 1) * 86400, &o2, &d2);
    int64_t diff = o1 > o2 ? o1 - o2 : o2 - o1;
    return (d1 || d2) ? diff : 0;
}

static SYSV int32_t ps4_sceKernelConvertUtcToLocaltime(int64_t utc, int64_t *local, ps4_timesec *ts,
                                                       uint32_t *dst_sec) {
    int64_t offset;
    bool dst;
    zone_at(utc, &offset, &dst);
    int64_t shift = dst ? zone_dst_shift(utc) : 0;
    if (local) *local = utc + offset;
    if (ts) {
        ts->t = utc;
        ts->west_sec = (uint32_t)(int32_t)(offset - shift);
        ts->dst_sec = (uint32_t)shift;
    }
    if (dst_sec) *dst_sec = (uint32_t)shift;
    return SCE_OK;
}

/* libc's mktime calls this with the tm_isdst hint, then re-checks the result
 * with ConvertUtcToLocaltime and adjusts by *dst_sec itself, so *dst_sec is
 * the zone's DST shift whether or not DST is in effect. */
static SYSV int32_t ps4_sceKernelConvertLocaltimeToUtc(int64_t local, int32_t isdst, int64_t *utc,
                                                       ps4_timesec *ts, int32_t *dst_sec) {
    (void)isdst;
    int64_t offset;
    bool dst;
    zone_at(local, &offset, &dst);       /* first guess: offset near `local` */
    zone_at(local - offset, &offset, &dst);
    int64_t shift = zone_dst_shift(local - offset);
    int64_t u = local - offset;
    if (utc) *utc = u;
    if (ts) {
        ts->t = u;
        ts->west_sec = (uint32_t)(int32_t)(offset - (dst ? shift : 0));
        ts->dst_sec = dst ? (uint32_t)shift : 0;
    }
    if (dst_sec) *dst_sec = (int32_t)shift;
    return SCE_OK;
}

/* ---- modules ------------------------------------------------------------- */

typedef struct ps4_module_segment {
    uint64_t address;
    uint32_t size;
    int32_t prot;
} ps4_module_segment;

/* OrbisKernelModuleInfoEx (shadPS4 core/module.h). */
typedef struct ps4_module_info_ex {
    uint64_t st_size;
    char name[256];
    int32_t id;
    uint32_t tls_index;
    uint64_t tls_init_addr;
    uint32_t tls_init_size;
    uint32_t tls_size;
    uint32_t tls_offset;
    uint32_t tls_align;
    uint64_t init_proc_addr;
    uint64_t fini_proc_addr;
    uint64_t reserved1;
    uint64_t reserved2;
    uint64_t eh_frame_hdr_addr;
    uint64_t eh_frame_addr;
    uint32_t eh_frame_hdr_size;
    uint32_t eh_frame_size;
    ps4_module_segment segments[4];
    uint32_t segment_count;
} ps4_module_info_ex;
_Static_assert(sizeof(ps4_module_info_ex) == 0x1a8, "ModuleInfoEx layout");

static SYSV int32_t ps4_sceKernelGetModuleInfoFromAddr(uint64_t addr, int32_t flags,
                                                       ps4_module_info_ex *info) {
    if (flags >= 3) {
        if (info) memset(info, 0, sizeof(*info));
        return SCE_KERNEL_ERROR(PS4_EINVAL);
    }
    if (!info) return SCE_KERNEL_ERROR(PS4_EFAULT);
    const guest_module *m = loader_module_at((void *)(uintptr_t)addr);
    if (!m) return SCE_KERNEL_ERROR(PS4_ESRCH);
    memset(info, 0, sizeof(*info));
    info->st_size = sizeof(*info);
    strncpy(info->name, m->name, sizeof(info->name) - 1);
    info->id = (int32_t)(m - g_rt.modules) + 1;
    info->tls_index = m->tls_modid;
    info->tls_init_addr = (uint64_t)(uintptr_t)m->tls_image;
    info->tls_init_size = (uint32_t)m->tls_filesz;
    info->tls_size = (uint32_t)m->tls_memsz;
    info->tls_align = (uint32_t)m->tls_align;
    info->init_proc_addr = (uint64_t)(uintptr_t)m->init;
    info->eh_frame_hdr_addr = (uint64_t)(uintptr_t)m->eh_frame_hdr;
    info->eh_frame_addr = (uint64_t)(uintptr_t)m->eh_frame;
    info->eh_frame_hdr_size = (uint32_t)m->eh_frame_hdr_size;
    info->eh_frame_size = (uint32_t)m->eh_frame_size;
    /* The runtime keeps one range per module, not per segment. */
    info->segments[0].address = (uint64_t)(uintptr_t)m->base;
    info->segments[0].size = (uint32_t)m->size;
    info->segments[0].prot = PLAT_PROT_R | PLAT_PROT_X;
    info->segment_count = 1;
    return SCE_OK;
}

/* Called by libc's heap setup with a zeroed {u64 addr; u64 size}; libc
 * aborts (int 0x44) on a non-zero return. An empty range means "no segment"
 * and turns the feature off. Not in shadPS4 (stub in aerolib). */
typedef struct ps4_segment_range {
    uint64_t addr;
    uint64_t size;
} ps4_segment_range;

static SYSV int32_t ps4_sceKernelInternalMemoryGetModuleSegmentInfo(ps4_segment_range *info) {
    if (info) memset(info, 0, sizeof(*info));
    return SCE_OK;
}

/* ---- libSceLibcInternalExt ------------------------------------------------ */

/* libc passes a 32-byte block with `size` pre-set and reads the rest back. */
typedef struct ps4_heap_trace_info {
    uint64_t size;
    uint32_t flag;
    uint32_t get_segment_info; /* non-zero: libc calls GetModuleSegmentInfo */
    uint64_t *mspace_atomic_id_mask;
    uint64_t *mstate_table;
} ps4_heap_trace_info;

static uint64_t g_mspace_atomic_id_mask;
static uint64_t g_mstate_table[64];

static SYSV void ps4_sceLibcHeapGetTraceInfo(ps4_heap_trace_info *info) {
    if (!info) return;
    info->get_segment_info = 0;
    info->mspace_atomic_id_mask = &g_mspace_atomic_id_mask;
    info->mstate_table = g_mstate_table;
}

HLE_LIBRARY(hle_kernel_misc, "libkernel",
    HLE(sceKernelConvertUtcToLocaltime),
    HLE(sceKernelConvertLocaltimeToUtc),
    HLE(sceKernelGetModuleInfoFromAddr),
    HLE(sceKernelInternalMemoryGetModuleSegmentInfo),
)

HLE_LIBRARY(hle_libc_internal_ext, "libSceLibcInternalExt",
    HLE(sceLibcHeapGetTraceInfo),
)
