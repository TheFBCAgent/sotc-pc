/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libkernel memory: direct/flexible memory, virtual ranges, mmap.
 *
 * Guest mappings live in one large arena reserved at startup (below 1 TiB,
 * where shadPS4 places user memory). Mapping = commit, unmapping = decommit,
 * so Windows only charges commit for what the game really maps.
 *
 * Simplification for now: direct memory is not aliased. Each map of a
 * physical range gets its own fresh pages, so mapping the same physical
 * memory at two addresses does not share contents. */
#include <stdlib.h>
#include <string.h>

#include "../log.h"
#include "../platform.h"
#include "hle.h"

#define ARENA_BASE 0x1000000000ull    /* 64 GiB */
#define ARENA_SIZE 0x4000000000ull    /* 256 GiB of address space */
#define PAGE 0x4000ull                 /* PS4 page size */
#define DMEM_TOTAL (5248ull << 20)     /* as shadPS4: ORBIS_KERNEL_TOTAL_MEM */
#define FMEM_TOTAL (448ull << 20)

/* PS4 protection bits. */
enum { P_CPU_READ = 1, P_CPU_WRITE = 2, P_CPU_EXEC = 4, P_GPU_READ = 0x10, P_GPU_WRITE = 0x20 };
/* Map flags (FreeBSD-derived). */
enum { F_SHARED = 1, F_PRIVATE = 2, F_FIXED = 0x10, F_NO_OVERWRITE = 0x80, F_ANON = 0x1000 };

enum region_kind { R_FLEXIBLE, R_DIRECT, R_RESERVED, R_MMAP };

typedef struct region {
    uint64_t start, size;
    int32_t prot;
    enum region_kind kind;
    uint64_t phys;
    char name[32];
} region;

static plat_mutex *g_lock;
static uint8_t *g_arena;
static uint64_t g_arena_next;
static uint64_t g_dmem_next, g_dmem_used, g_fmem_used;
static region *g_regions;
static uint32_t g_region_count, g_region_cap;

static uint64_t align_up(uint64_t v, uint64_t a) { return a ? (v + a - 1) / a * a : v; }

static int host_prot(int32_t prot) {
    int p = 0;
    if (prot & P_CPU_READ) p |= PLAT_PROT_R;
    if (prot & P_CPU_WRITE) p |= PLAT_PROT_W;
    if (prot & P_CPU_EXEC) p |= PLAT_PROT_X;
    /* GPU-visible memory must stay CPU-accessible: our GPU emulation reads it. */
    if (prot & (P_GPU_READ | P_GPU_WRITE)) p |= PLAT_PROT_R | PLAT_PROT_W;
    return p;
}

void hle_memory_init(void) {
    g_lock = plat_mutex_new();
    g_arena = plat_reserve(ARENA_BASE, ARENA_SIZE);
    if (!g_arena) log_fatal("cannot reserve the guest memory arena at %#llx", ARENA_BASE);
    g_arena_next = ARENA_BASE;
    log_info("memory: arena %#llx-%#llx", ARENA_BASE, ARENA_BASE + ARENA_SIZE);
}

static bool in_arena(uint64_t addr, uint64_t size) {
    return addr >= ARENA_BASE && addr + size <= ARENA_BASE + ARENA_SIZE;
}

/* Remove [start, start+size) from the region list, splitting as needed. */
static void regions_remove(uint64_t start, uint64_t size) {
    uint64_t end = start + size;
    for (uint32_t i = 0; i < g_region_count;) {
        region *r = &g_regions[i];
        uint64_t rs = r->start, re = r->start + r->size;
        if (re <= start || rs >= end) { i++; continue; }
        if (rs < start && re > end) { /* split in two */
            region tail = *r;
            tail.start = end;
            tail.size = re - end;
            r->size = start - rs;
            if (g_region_count == g_region_cap) {
                g_region_cap = g_region_cap ? g_region_cap * 2 : 64;
                g_regions = realloc(g_regions, g_region_cap * sizeof(region));
                r = &g_regions[i];
            }
            memmove(&g_regions[i + 2], &g_regions[i + 1], (g_region_count - i - 1) * sizeof(region));
            g_regions[i + 1] = tail;
            g_region_count++;
            return;
        }
        if (rs < start) { r->size = start - rs; i++; continue; }
        if (re > end) { r->size = re - end; r->start = end; i++; continue; }
        memmove(r, r + 1, (g_region_count - i - 1) * sizeof(region));
        g_region_count--;
    }
}

static void regions_add(uint64_t start, uint64_t size, int32_t prot, enum region_kind kind,
                        uint64_t phys, const char *name) {
    regions_remove(start, size);
    if (g_region_count == g_region_cap) {
        g_region_cap = g_region_cap ? g_region_cap * 2 : 64;
        g_regions = realloc(g_regions, g_region_cap * sizeof(region));
    }
    uint32_t i = 0;
    while (i < g_region_count && g_regions[i].start < start) i++;
    memmove(&g_regions[i + 1], &g_regions[i], (g_region_count - i) * sizeof(region));
    region *r = &g_regions[i];
    r->start = start;
    r->size = size;
    r->prot = prot;
    r->kind = kind;
    r->phys = phys;
    strncpy(r->name, name ? name : "", sizeof(r->name) - 1);
    r->name[sizeof(r->name) - 1] = 0;
    g_region_count++;
}

static const region *region_at(uint64_t addr) {
    for (uint32_t i = 0; i < g_region_count; i++)
        if (addr >= g_regions[i].start && addr < g_regions[i].start + g_regions[i].size)
            return &g_regions[i];
    return NULL;
}

/* Pick an address: a fixed request, or the next aligned spot in the arena. */
static uint64_t choose_address(uint64_t hint, uint64_t size, uint64_t align, bool fixed) {
    if (fixed) return hint;
    if (align < PAGE) align = PAGE;
    uint64_t addr = align_up(hint && in_arena(hint, size) && hint > g_arena_next ? hint
                                                                               : g_arena_next,
                             align);
    if (!in_arena(addr, size)) return 0;
    g_arena_next = addr + size;
    return addr;
}

/* Back [addr, addr+size) with fresh zeroed pages. */
static bool commit_range(uint64_t addr, uint64_t size, int prot) {
    if (in_arena(addr, size)) return plat_commit((void *)(uintptr_t)addr, size, prot);
    return plat_map(addr, size, prot, true) != NULL;
}

static int32_t map_common(void **addr_io, uint64_t len, int32_t prot, int32_t flags,
                          uint64_t align, enum region_kind kind, uint64_t phys, const char *name) {
    if (!addr_io) return SCE_KERNEL_ERROR(PS4_EFAULT);
    if (len == 0 || len % PAGE) return SCE_KERNEL_ERROR(PS4_EINVAL);
    plat_mutex_lock(g_lock);
    uint64_t want = (uint64_t)(uintptr_t)*addr_io;
    uint64_t addr = choose_address(want, len, align, flags & F_FIXED);
    bool ok = addr && (kind == R_RESERVED || commit_range(addr, len, host_prot(prot)));
    if (ok) {
        regions_add(addr, len, prot, kind, phys, name);
        if (kind == R_FLEXIBLE) g_fmem_used += len;
    }
    plat_mutex_unlock(g_lock);
    log_debug("map %s %#llx+%#llx prot %#x flags %#x -> %s", name ? name : "",
              (unsigned long long)addr, (unsigned long long)len, prot, flags, ok ? "ok" : "FAILED");
    if (!ok) return SCE_KERNEL_ERROR(PS4_ENOMEM);
    *addr_io = (void *)(uintptr_t)addr;
    return SCE_OK;
}

/* ---- direct memory ----------------------------------------------------- */

static SYSV uint64_t ps4_sceKernelGetDirectMemorySize(void) { return DMEM_TOTAL; }

static SYSV int32_t ps4_sceKernelAllocateDirectMemory(int64_t start, int64_t end, uint64_t len,
                                                     uint64_t align, int32_t type,
                                                     int64_t *phys_out) {
    if (start < 0 || end < 0 || !phys_out || len == 0 || len % PAGE)
        return SCE_KERNEL_ERROR(PS4_EINVAL);
    plat_mutex_lock(g_lock);
    uint64_t phys = align_up(g_dmem_next > (uint64_t)start ? g_dmem_next : (uint64_t)start,
                             align ? align : PAGE);
    bool ok = phys + len <= (uint64_t)end && phys + len <= DMEM_TOTAL;
    if (ok) {
        g_dmem_next = phys + len;
        g_dmem_used += len;
    }
    plat_mutex_unlock(g_lock);
    log_debug("allocate direct %#llx bytes type %d -> %s %#llx", (unsigned long long)len, type,
              ok ? "phys" : "FAILED", (unsigned long long)phys);
    if (!ok) return SCE_KERNEL_ERROR(PS4_EAGAIN);
    *phys_out = (int64_t)phys;
    return SCE_OK;
}

static SYSV int32_t ps4_sceKernelAvailableDirectMemorySize(uint64_t start, uint64_t end,
                                                          uint64_t align, uint64_t *phys_out,
                                                          uint64_t *size_out) {
    (void)align;
    uint64_t from = g_dmem_next > start ? g_dmem_next : start;
    uint64_t limit = end < DMEM_TOTAL ? end : DMEM_TOTAL;
    if (phys_out) *phys_out = from;
    if (size_out) *size_out = limit > from ? limit - from : 0;
    return SCE_OK;
}

static SYSV int32_t ps4_sceKernelMapDirectMemory(void **addr, uint64_t len, int32_t prot,
                                                 int32_t flags, int64_t phys, uint64_t align) {
    return map_common(addr, len, prot, flags, align, R_DIRECT, (uint64_t)phys, "direct");
}

/* ---- flexible memory and virtual ranges ---------------------------------- */

static SYSV int32_t ps4_sceKernelMapNamedFlexibleMemory(void **addr, uint64_t len, int32_t prot,
                                                       int32_t flags, const char *name) {
    return map_common(addr, len, prot, flags, 0, R_FLEXIBLE, 0, name);
}

static SYSV int32_t ps4_sceKernelMapFlexibleMemory(void **addr, uint64_t len, int32_t prot,
                                                  int32_t flags) {
    return map_common(addr, len, prot, flags, 0, R_FLEXIBLE, 0, "flexible");
}

static SYSV int32_t ps4_sceKernelAvailableFlexibleMemorySize(uint64_t *size) {
    if (!size) return SCE_KERNEL_ERROR(PS4_EINVAL);
    *size = FMEM_TOTAL > g_fmem_used ? FMEM_TOTAL - g_fmem_used : 0;
    return SCE_OK;
}

static SYSV int32_t ps4_sceKernelReserveVirtualRange(void **addr, uint64_t len, int32_t flags,
                                                    uint64_t align) {
    return map_common(addr, len, 0, flags, align, R_RESERVED, 0, "reserved");
}

static SYSV int32_t ps4_sceKernelMunmap(void *addr, uint64_t len) {
    uint64_t a = (uint64_t)(uintptr_t)addr;
    if (len == 0 || a % PAGE) return SCE_KERNEL_ERROR(PS4_EINVAL);
    len = align_up(len, PAGE);
    plat_mutex_lock(g_lock);
    if (in_arena(a, len)) plat_decommit(addr, len);
    else plat_unmap(addr, len);
    regions_remove(a, len);
    plat_mutex_unlock(g_lock);
    return SCE_OK;
}

static SYSV int32_t ps4_sceKernelMprotect(const void *addr, uint64_t len, int32_t prot) {
    uint64_t a = (uint64_t)(uintptr_t)addr & ~(PAGE - 1);
    len = align_up(len + ((uint64_t)(uintptr_t)addr - a), PAGE);
    return plat_protect((void *)(uintptr_t)a, len, host_prot(prot)) ? SCE_OK
                                                                     : SCE_KERNEL_ERROR(PS4_EINVAL);
}

static SYSV int32_t ps4_sceKernelMlock(const void *addr, uint64_t len) {
    (void)addr; (void)len;
    return SCE_OK;
}

static SYSV int32_t ps4_msync(void *addr, uint64_t len, int32_t flags) {
    (void)addr; (void)len; (void)flags;
    return 0;
}

/* ---- queries ----------------------------------------------------------- */

typedef struct virtual_query_info {
    uint64_t start, end, offset;
    int32_t protection, memory_type;
    uint8_t flags; /* is_flexible:1 is_direct:1 is_stack:1 is_pooled:1 is_committed:1 */
    char name[32];
} virtual_query_info;
_Static_assert(sizeof(virtual_query_info) == 72, "virtual_query_info layout");

static SYSV int32_t ps4_sceKernelVirtualQuery(const void *addr, int32_t flags,
                                              virtual_query_info *info, uint64_t info_size) {
    if (!info || info_size < sizeof(*info)) return SCE_KERNEL_ERROR(PS4_EFAULT);
    uint64_t a = (uint64_t)(uintptr_t)addr;
    plat_mutex_lock(g_lock);
    const region *r = region_at(a);
    if (!r && (flags & 1)) /* SCE_KERNEL_VQ_FIND_NEXT */
        for (uint32_t i = 0; i < g_region_count; i++)
            if (g_regions[i].start > a) { r = &g_regions[i]; break; }
    int32_t ret = SCE_KERNEL_ERROR(PS4_EACCES);
    if (r && r->kind != R_RESERVED) {
        memset(info, 0, sizeof(*info));
        info->start = r->start;
        info->end = r->start + r->size;
        info->offset = r->kind == R_DIRECT ? r->phys : 0;
        info->protection = r->prot;
        info->flags = (uint8_t)((r->kind == R_FLEXIBLE ? 1 : 0) | (r->kind == R_DIRECT ? 2 : 0) |
                                0x10);
        strncpy(info->name, r->name, sizeof(info->name) - 1);
        ret = SCE_OK;
    }
    plat_mutex_unlock(g_lock);
    return ret;
}

/* ---- mmap and batch map ------------------------------------------------- */

static SYSV void *ps4_mmap(void *addr, uint64_t len, int32_t prot, int32_t flags, int32_t fd,
                           int64_t offset) {
    void *where = addr;
    uint64_t size = align_up(len, PAGE);
    bool file = !(flags & F_ANON) && fd >= 0;
    /* Map writable first so file contents can be copied in. */
    int32_t map_prot = file ? prot | P_CPU_READ | P_CPU_WRITE : prot;
    if (map_common(&where, size, map_prot, flags, 0, R_MMAP, 0, file ? "file" : "anon") != SCE_OK) {
        *hle_errno() = PS4_ENOMEM;
        return (void *)-1;
    }
    if (file) {
        if (hle_file_pread(fd, where, len, offset) < 0) log_warn("mmap: reading fd %d failed", fd);
        if (map_prot != prot) ps4_sceKernelMprotect(where, size, prot);
    }
    return where;
}

typedef struct batch_map_entry {
    void *start;
    uint64_t offset;
    uint64_t length;
    int8_t protection;
    int8_t type;
    int16_t reserved;
    int32_t operation;
} batch_map_entry;

static SYSV int32_t ps4_sceKernelBatchMap(batch_map_entry *entries, int32_t count,
                                          int32_t *processed, int32_t flags) {
    int32_t ret = SCE_OK, i = 0;
    for (; i < count && ret == SCE_OK; i++) {
        batch_map_entry *e = &entries[i];
        switch (e->operation) {
        case 0: ret = ps4_sceKernelMapDirectMemory(&e->start, e->length, e->protection,
                                                   flags | F_FIXED, (int64_t)e->offset, 0); break;
        case 1: ret = ps4_sceKernelMunmap(e->start, e->length); break;
        case 2: ret = ps4_sceKernelMprotect(e->start, e->length, e->protection); break;
        case 3: ret = ps4_sceKernelMapNamedFlexibleMemory(&e->start, e->length, e->protection,
                                                          flags | F_FIXED, "batch"); break;
        default:
            log_warn("sceKernelBatchMap: operation %d not implemented", e->operation);
            ret = SCE_KERNEL_ERROR(PS4_EINVAL);
        }
    }
    if (processed) *processed = ret == SCE_OK ? i : i - 1;
    return ret;
}

HLE_LIBRARY(hle_kernel_mem, "libkernel",
    HLE(sceKernelGetDirectMemorySize),
    HLE(sceKernelAllocateDirectMemory),
    HLE(sceKernelAvailableDirectMemorySize),
    HLE(sceKernelMapDirectMemory),
    HLE(sceKernelMapNamedFlexibleMemory),
    HLE(sceKernelMapFlexibleMemory),
    HLE(sceKernelAvailableFlexibleMemorySize),
    HLE(sceKernelReserveVirtualRange),
    HLE(sceKernelMunmap),
    HLE(sceKernelMprotect),
    HLE(sceKernelMlock),
    HLE(msync),
    HLE(sceKernelVirtualQuery),
    HLE(mmap),
    HLE(sceKernelBatchMap),
)
