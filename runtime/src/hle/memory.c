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
    bool view; /* backed by a view of physical memory (aliasable) */
    char name[32];
} region;

/* Each region is one mapping unit (one host allocation or section view);
 * reserved regions are bookkeeping only. Sorted by start, non-overlapping. */
static plat_mutex *g_lock;
static uint64_t g_dmem_used, g_fmem_used;
/* Allocated direct memory blocks, sorted by start (the game never releases). */
typedef struct dblock { uint64_t start, end; int32_t type; } dblock;
static dblock *g_dblocks;
static uint32_t g_dblock_count, g_dblock_cap;
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
    if (!plat_phys_init(DMEM_TOTAL)) log_fatal("cannot create the direct memory backing");
    if (!plat_arena_reserve(ARENA_BASE, ARENA_SIZE))
        log_fatal("cannot reserve the guest memory arena at %#llx", ARENA_BASE);
    log_info("memory: arena %#llx-%#llx", ARENA_BASE, ARENA_BASE + ARENA_SIZE);
}

static bool in_arena(uint64_t addr, uint64_t size) {
    return addr >= ARENA_BASE && addr + size <= ARENA_BASE + ARENA_SIZE;
}

static void regions_insert(const region *r) {
    if (g_region_count == g_region_cap) {
        g_region_cap = g_region_cap ? g_region_cap * 2 : 64;
        g_regions = realloc(g_regions, g_region_cap * sizeof(region));
    }
    uint32_t i = 0;
    while (i < g_region_count && g_regions[i].start < r->start) i++;
    memmove(&g_regions[i + 1], &g_regions[i], (g_region_count - i) * sizeof(region));
    g_regions[i] = *r;
    g_region_count++;
}

static void regions_erase(uint32_t i) {
    memmove(&g_regions[i], &g_regions[i + 1], (g_region_count - i - 1) * sizeof(region));
    g_region_count--;
}

/* Back region r with host memory. */
static bool unit_map(region *r, bool commit) {
    if (r->kind == R_RESERVED) return true;
    int prot = host_prot(r->prot);
    if (!in_arena(r->start, r->size)) {
        r->view = false;
        return plat_map(r->start, r->size, prot, true) != NULL;
    }
    r->view = r->kind == R_DIRECT && r->start % PLAT_VIEW_ALIGN == 0 &&
              r->size % PLAT_VIEW_ALIGN == 0 && r->phys % PLAT_VIEW_ALIGN == 0;
    if (r->kind == R_DIRECT && !r->view) {
        static int warned;
        if (!warned++)
            log_warn("memory: direct map %#llx+%#llx phys %#llx is not 64 KiB aligned; "
                     "it will not alias other mappings", (unsigned long long)r->start,
                     (unsigned long long)r->size, (unsigned long long)r->phys);
    }
    return plat_arena_map(r->start, r->size, prot, r->view ? (int64_t)r->phys : -1, commit);
}

static void unit_unmap(const region *r) {
    if (r->kind == R_RESERVED) return;
    if (!in_arena(r->start, r->size)) plat_unmap((void *)(uintptr_t)r->start, r->size);
    else plat_arena_unmap(r->start, r->size, r->view);
}

/* Map a piece of a unit being split as a unit of its own. Private memory
 * (and views that can't stay views) keep their contents through a copy. */
static void remap_piece(const region *whole, uint64_t start, uint64_t size, const void *saved) {
    region piece = *whole;
    piece.start = start;
    piece.size = size;
    piece.phys = whole->phys + (start - whole->start);
    if (saved) {
        int32_t prot = piece.prot;
        piece.prot = P_CPU_READ | P_CPU_WRITE;
        if (unit_map(&piece, true)) {
            memcpy((void *)(uintptr_t)start, saved, size);
            plat_protect((void *)(uintptr_t)start, size, host_prot(prot));
        }
        piece.prot = prot;
    } else if (!unit_map(&piece, false)) { /* a view piece: pages already committed */
        log_error("memory: remapping %#llx+%#llx failed", (unsigned long long)start,
                  (unsigned long long)size);
    }
    regions_insert(&piece);
}

/* Remove everything in [start, start+size), splitting units at the edges.
 * Caller holds g_lock. */
static void unmap_range(uint64_t start, uint64_t size) {
    uint64_t end = start + size;
    for (uint32_t i = 0; i < g_region_count;) {
        region r = g_regions[i];
        uint64_t rs = r.start, re = r.start + r.size;
        if (re <= start || rs >= end) { i++; continue; }
        uint64_t lo_size = rs < start ? start - rs : 0, hi_size = re > end ? re - end : 0;
        /* Pieces that can't be remapped as views need their bytes saved. */
        void *lo_saved = NULL, *hi_saved = NULL;
        if (r.kind != R_RESERVED) {
            bool lo_view = r.view && start % PLAT_VIEW_ALIGN == 0;
            bool hi_view = r.view && end % PLAT_VIEW_ALIGN == 0;
            if ((lo_size && !lo_view) || (hi_size && !hi_view))
                plat_protect((void *)(uintptr_t)rs, r.size, PLAT_PROT_R | PLAT_PROT_W);
            if (lo_size && !lo_view) {
                lo_saved = malloc(lo_size);
                memcpy(lo_saved, (void *)(uintptr_t)rs, lo_size);
            }
            if (hi_size && !hi_view) {
                hi_saved = malloc(hi_size);
                memcpy(hi_saved, (void *)(uintptr_t)end, hi_size);
            }
        }
        if (lo_saved || hi_saved)
            log_debug("memory: splitting %s unit %#llx+%#llx around %#llx+%#llx (copying %#llx bytes)",
                      r.view ? "view" : "private", (unsigned long long)rs,
                      (unsigned long long)r.size, (unsigned long long)start,
                      (unsigned long long)size, (unsigned long long)((lo_saved ? lo_size : 0) +
                                                                     (hi_saved ? hi_size : 0)));
        regions_erase(i);
        unit_unmap(&r);
        if (lo_size) remap_piece(&r, rs, lo_size, lo_saved);
        if (hi_size) remap_piece(&r, end, hi_size, hi_saved);
        free(lo_saved);
        free(hi_saved);
        i = 0; /* the list changed; rescan */
    }
}

/* The fault handler asks this before reporting an access violation. Splitting
 * a view briefly unmaps it under g_lock while other threads may still touch
 * the rest; waiting for the lock and retrying the access fixes that race.
 * True if `addr` is mapped readable memory now (so the access should retry). */
static const region *region_at(uint64_t addr);
bool hle_memory_fault_retry(uint64_t addr) {
    if (!g_lock || !in_arena(addr, 1)) return false;
    plat_mutex_lock(g_lock); /* waits for an in-progress split */
    const region *r = region_at(addr);
    bool mapped = r && r->kind != R_RESERVED && (host_prot(r->prot) & PLAT_PROT_R);
    plat_mutex_unlock(g_lock);
    return mapped;
}

static const region *region_at(uint64_t addr) {
    for (uint32_t i = 0; i < g_region_count; i++)
        if (addr >= g_regions[i].start && addr < g_regions[i].start + g_regions[i].size)
            return &g_regions[i];
    return NULL;
}

/* Pick an address: a fixed request, or the first aligned gap at or after the
 * hint (the arena start without one), like the PS4 kernel's search. */
static uint64_t choose_address(uint64_t hint, uint64_t size, uint64_t align, bool fixed) {
    if (fixed) return hint;
    if (align < PAGE) align = PAGE;
    uint64_t at = align_up(hint && in_arena(hint, size) ? hint : ARENA_BASE, align);
    for (uint32_t i = 0; i < g_region_count; i++) {
        const region *r = &g_regions[i];
        if (r->start + r->size <= at) continue;
        if (r->start >= at + size) break;
        at = align_up(r->start + r->size, align);
    }
    return in_arena(at, size) ? at : 0;
}

static int32_t map_common(void **addr_io, uint64_t len, int32_t prot, int32_t flags,
                          uint64_t align, enum region_kind kind, uint64_t phys, const char *name) {
    if (!addr_io) return SCE_KERNEL_ERROR(PS4_EFAULT);
    if (len == 0 || len % PAGE) return SCE_KERNEL_ERROR(PS4_EINVAL);
    plat_mutex_lock(g_lock);
    uint64_t want = (uint64_t)(uintptr_t)*addr_io;
    bool fixed = flags & F_FIXED;
    uint64_t addr = choose_address(want, len, align, fixed);
    bool ok = addr != 0;
    if (ok && fixed) unmap_range(addr, len); /* MAP_FIXED replaces what was there */
    if (ok) {
        region r = {.start = addr, .size = len, .prot = prot, .kind = kind, .phys = phys};
        strncpy(r.name, name ? name : "", sizeof(r.name) - 1);
        ok = unit_map(&r, true);
        if (ok) {
            regions_insert(&r);
            if (kind == R_FLEXIBLE) g_fmem_used += len;
        }
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

/* First free gap in [start, end) that fits len at `align`; with len == 0,
 * the largest aligned gap instead. Returns the gap start in *phys_out and its
 * usable size. Caller holds g_lock. */
static uint64_t dmem_find(uint64_t start, uint64_t end, uint64_t len, uint64_t align,
                          uint64_t *phys_out) {
    if (end > DMEM_TOTAL) end = DMEM_TOTAL;
    if (align < PAGE) align = PAGE;
    uint64_t best = 0, best_at = 0, cur = start;
    for (uint32_t i = 0; i <= g_dblock_count; i++) {
        uint64_t gap_end = i < g_dblock_count ? g_dblocks[i].start : end;
        if (gap_end > end) gap_end = end;
        uint64_t at = align_up(cur, align);
        if (gap_end > at) {
            uint64_t size = gap_end - at;
            if (len && size >= len) { *phys_out = at; return size; }
            if (!len && size > best) { best = size; best_at = at; }
        }
        if (i < g_dblock_count && g_dblocks[i].end > cur) cur = g_dblocks[i].end;
        if (cur >= end) break;
    }
    *phys_out = best_at;
    return len ? 0 : best;
}

static SYSV int32_t ps4_sceKernelAllocateDirectMemory(int64_t start, int64_t end, uint64_t len,
                                                     uint64_t align, int32_t type,
                                                     int64_t *phys_out) {
    if (start < 0 || end <= start || !phys_out || len == 0 || len % PAGE || (align & (align - 1)))
        return SCE_KERNEL_ERROR(PS4_EINVAL);
    plat_mutex_lock(g_lock);
    uint64_t phys = 0;
    bool ok = dmem_find((uint64_t)start, (uint64_t)end, len, align, &phys) != 0;
    if (ok) {
        if (g_dblock_count == g_dblock_cap) {
            g_dblock_cap = g_dblock_cap ? g_dblock_cap * 2 : 64;
            g_dblocks = realloc(g_dblocks, g_dblock_cap * sizeof(dblock));
        }
        uint32_t i = 0;
        while (i < g_dblock_count && g_dblocks[i].start < phys) i++;
        memmove(&g_dblocks[i + 1], &g_dblocks[i], (g_dblock_count - i) * sizeof(dblock));
        g_dblocks[i] = (dblock){phys, phys + len, type};
        g_dblock_count++;
        g_dmem_used += len;
    }
    plat_mutex_unlock(g_lock);
    log_debug("allocate direct %#llx bytes in [%#llx, %#llx) align %#llx type %d -> %s %#llx",
              (unsigned long long)len, (unsigned long long)start, (unsigned long long)end,
              (unsigned long long)align, type, ok ? "phys" : "FAILED", (unsigned long long)phys);
    if (!ok) return SCE_KERNEL_ERROR(PS4_EAGAIN);
    *phys_out = (int64_t)phys;
    return SCE_OK;
}

/* The largest free block in [start, end), as shadPS4's PoolCommit-less query. */
static SYSV int32_t ps4_sceKernelAvailableDirectMemorySize(uint64_t start, uint64_t end,
                                                          uint64_t align, uint64_t *phys_out,
                                                          uint64_t *size_out) {
    if (!phys_out || !size_out || end <= start) return SCE_KERNEL_ERROR(PS4_EINVAL);
    plat_mutex_lock(g_lock);
    uint64_t phys = 0, size = dmem_find(start, end, 0, align, &phys);
    plat_mutex_unlock(g_lock);
    if (!size) return SCE_KERNEL_ERROR(PS4_ENOMEM);
    *phys_out = phys;
    *size_out = size;
    return SCE_OK;
}

static SYSV int32_t ps4_sceKernelMapDirectMemory(void **addr, uint64_t len, int32_t prot,
                                                 int32_t flags, int64_t phys, uint64_t align) {
    log_trace("map direct phys %#llx len %#llx", (unsigned long long)phys, (unsigned long long)len);
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
    unmap_range(a, len);
    plat_mutex_unlock(g_lock);
    log_debug("munmap %#llx+%#llx", (unsigned long long)a, (unsigned long long)len);
    return SCE_OK;
}

/* Protection changes apply per unit (host protection calls can't span
 * separate allocations). Ranges outside every region go straight through. */
static SYSV int32_t ps4_sceKernelMprotect(const void *addr, uint64_t len, int32_t prot) {
    uint64_t a = (uint64_t)(uintptr_t)addr & ~(PAGE - 1);
    len = align_up(len + ((uint64_t)(uintptr_t)addr - a), PAGE);
    uint64_t end = a + len;
    bool ok = true, any = false;
    plat_mutex_lock(g_lock);
    for (uint32_t i = 0; i < g_region_count; i++) {
        region *r = &g_regions[i];
        uint64_t s0 = r->start > a ? r->start : a, e0 = r->start + r->size < end ? r->start + r->size : end;
        if (s0 >= e0) continue;
        any = true;
        if (r->kind == R_RESERVED) continue;
        ok &= plat_protect((void *)(uintptr_t)s0, e0 - s0, host_prot(prot));
        if (s0 == r->start && e0 == r->start + r->size) r->prot = prot;
    }
    plat_mutex_unlock(g_lock);
    if (!any) ok = plat_protect((void *)(uintptr_t)a, len, host_prot(prot));
    return ok ? SCE_OK : SCE_KERNEL_ERROR(PS4_EINVAL);
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
