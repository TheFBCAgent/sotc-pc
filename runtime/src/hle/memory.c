/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libkernel memory: direct/flexible memory, virtual ranges, mmap.
 *
 * Guest mappings live in one large arena reserved at startup (below 1 TiB,
 * where shadPS4 places user memory). Mapping = commit, unmapping = decommit,
 * so Windows only charges commit for what the game really maps.
 *
 * Direct memory is aliased through views of one physical section (64 KiB
 * aligned maps; others get private pages).
 *
 * M3: every map, unmap and protection change is reported to the GPU bridge
 * (gpu_bridge.h), which keeps the guest's protection per page and combines
 * it with the GPU's page tracking, and GPU-visible maps are reported to
 * sotcgpu (outside g_lock). The queries at the end serve sotcgpu's host
 * callbacks. Lock order: g_lock, then the bridge's tracking lock. */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../gpu_bridge.h"
#include "../log.h"
#include "../platform.h"
#include "../runtime.h"
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
/* Bumped (under g_lock) whenever g_regions changes; validates the lock-free
 * per-thread cache of hle_memory_clamp_range. Starts at 1 so a zeroed cache
 * never matches. */
static _Atomic uint64_t g_regions_gen = 1;

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
    atomic_fetch_add_explicit(&g_regions_gen, 1, memory_order_release);
}

static void regions_erase(uint32_t i) {
    memmove(&g_regions[i], &g_regions[i + 1], (g_region_count - i - 1) * sizeof(region));
    g_region_count--;
    atomic_fetch_add_explicit(&g_regions_gen, 1, memory_order_release);
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
    if (piece.kind != R_RESERVED) gpu_bridge_guest_remap(start, size, host_prot(piece.prot));
    regions_insert(&piece);
}

/* Remove everything in [start, start+size), splitting units at the edges.
 * Caller holds g_lock. The bridge's tracking lock is held throughout so the
 * GPU can't re-protect pages while they are copied and re-mapped. */
static void unmap_range(uint64_t start, uint64_t size) {
    uint64_t end = start + size;
    gpu_bridge_track_lock();
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
        if (r.kind != R_RESERVED)
            gpu_bridge_guest_unmap(rs > start ? rs : start, (re < end ? re : end) - (rs > start ? rs : start));
        if (lo_size) remap_piece(&r, rs, lo_size, lo_saved);
        if (hi_size) remap_piece(&r, end, hi_size, hi_saved);
        free(lo_saved);
        free(hi_saved);
        i = 0; /* the list changed; rescan */
    }
    gpu_bridge_track_unlock();
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
    uint64_t want = (uint64_t)(uintptr_t)*addr_io;
    bool fixed = flags & F_FIXED;
    /* MAP_FIXED replaces what was there: the GPU forgets it first (as
     * shadPS4's UnmapMemory), outside g_lock since sotcgpu may call back. */
    if (fixed) gpu_bridge_notify_unmap(want, len);
    plat_mutex_lock(g_lock);
    uint64_t addr = choose_address(want, len, align, fixed);
    bool ok = addr != 0;
    if (ok && fixed) unmap_range(addr, len); /* MAP_FIXED replaces what was there */
    if (ok) {
        region r = {.start = addr, .size = len, .prot = prot, .kind = kind, .phys = phys};
        strncpy(r.name, name ? name : "", sizeof(r.name) - 1);
        ok = unit_map(&r, true);
        if (ok) {
            if (kind != R_RESERVED) gpu_bridge_guest_map(addr, len, host_prot(prot));
            regions_insert(&r);
            if (kind == R_FLEXIBLE) g_fmem_used += len;
        }
    }
    plat_mutex_unlock(g_lock);
    log_debug("map %s %#llx+%#llx prot %#x flags %#x -> %s", name ? name : "",
              (unsigned long long)addr, (unsigned long long)len, prot, flags, ok ? "ok" : "FAILED");
    if (!ok) return SCE_KERNEL_ERROR(PS4_ENOMEM);
    if (kind != R_RESERVED) gpu_bridge_notify_map(addr, len);
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
    gpu_bridge_notify_unmap(a, len);
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
        ok &= gpu_bridge_guest_protect(s0, e0 - s0, host_prot(prot)); /* keeps GPU tracking */
        if (s0 == r->start && e0 == r->start + r->size) r->prot = prot;
    }
    plat_mutex_unlock(g_lock);
    if (!any) ok = gpu_bridge_guest_protect(a, len, host_prot(prot));
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
    /* Reserved ranges are reported too, uncommitted and with their protection
     * hidden (shadPS4 does the same). SotC's heap walks a range with this and
     * ignores the result: an error left its stale `end` in place, and the walk
     * spun forever on the address after it (the title-screen freeze). */
    int32_t ret = SCE_KERNEL_ERROR(PS4_EACCES);
    if (r) {
        const bool committed = r->kind != R_RESERVED;
        memset(info, 0, sizeof(*info));
        info->start = r->start;
        info->end = r->start + r->size;
        info->offset = r->kind == R_DIRECT ? r->phys : 0;
        info->protection = committed ? r->prot : 0;
        info->flags = (uint8_t)((r->kind == R_FLEXIBLE ? 1 : 0) | (r->kind == R_DIRECT ? 2 : 0) |
                                (committed ? 0x10 : 0));
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

/* ---- queries for sotcgpu's host callbacks (gpu_bridge.c) ----------------- */

/* First region that ends after addr. Caller holds g_lock. */
static uint32_t region_lower(uint64_t addr) {
    uint32_t lo = 0, hi = g_region_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g_regions[mid].start + g_regions[mid].size <= addr) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* The span of guest memory starting at addr: a region, the loaded image
 * (mapped by the loader, not through here), or a hole up to the next one. */
typedef struct span { uint64_t end; const region *r; bool mapped; } span;

static span span_at(uint64_t a) {
    uint32_t i = region_lower(a);
    if (i < g_region_count && g_regions[i].start <= a) {
        const region *r = &g_regions[i];
        return (span){r->start + r->size, r, r->kind != R_RESERVED};
    }
    uint64_t next = i < g_region_count ? g_regions[i].start : PLAT_GUEST_LIMIT;
    uint64_t img = (uint64_t)(uintptr_t)g_rt.image, img_end = img + g_rt.image_size;
    if (g_rt.image && a >= img && a < img_end)
        return (span){img_end < next ? img_end : next, NULL, true};
    if (g_rt.image && img > a && img < next) next = img;
    return (span){next, NULL, false};
}

/* Copy between a host buffer and one mapped span piece, ignoring protection:
 * views through the physical backing view, the rest via the bridge. */
static bool span_copy(const span *sp, uint64_t a, void *host, uint64_t n, bool to_guest) {
    uint8_t *backing = sp->r && sp->r->view ? plat_phys_backing() : NULL;
    if (backing) {
        uint8_t *phys = backing + sp->r->phys + (a - sp->r->start);
        if (to_guest) memcpy(phys, host, n);
        else memcpy(host, phys, n);
        return true;
    }
    return gpu_bridge_force_copy(a, host, n, to_guest);
}

/* shadPS4 Memory::TryWriteBacking: false if part of the range has no memory. */
bool hle_memory_write_backing(uint64_t addr, const void *src, uint64_t size) {
    bool ok = true;
    plat_mutex_lock(g_lock);
    for (uint64_t a = addr; size;) {
        span sp = span_at(a);
        uint64_t n = sp.end - a < size ? sp.end - a : size;
        if (!sp.mapped || !span_copy(&sp, a, (void *)src, n, true)) ok = false;
        a += n;
        src = (const uint8_t *)src + n;
        size -= n;
    }
    plat_mutex_unlock(g_lock);
    return ok;
}

uint64_t hle_memory_backing_ptr(uint64_t addr, uint64_t size) {
    uint8_t *backing = plat_phys_backing();
    if (!backing || !size) return 0;
    uint64_t r = 0;
    plat_mutex_lock(g_lock);
    span sp = span_at(addr);
    if (sp.mapped && sp.r && sp.r->view && size <= sp.end - addr)
        r = (uint64_t)(uintptr_t)(backing + sp.r->phys + (addr - sp.r->start));
    plat_mutex_unlock(g_lock);
    return r;
}

/* shadPS4 Memory::CopySparseMemory: holes read as zero. */
void hle_memory_read_sparse(void *dst, uint64_t addr, uint64_t size) {
    plat_mutex_lock(g_lock);
    for (uint64_t a = addr; size;) {
        span sp = span_at(a);
        uint64_t n = sp.end - a < size ? sp.end - a : size;
        if (!sp.mapped || !span_copy(&sp, a, dst, n, false)) memset(dst, 0, n);
        a += n;
        dst = (uint8_t *)dst + n;
        size -= n;
    }
    plat_mutex_unlock(g_lock);
}

/* Cap for "unbounded" buffer descriptors, in bytes (see below).
 * SOTC_UNBOUNDED_CAP_MB overrides the default; 0 turns the cap off. */
static uint64_t unbounded_cap(void) {
    static uint64_t cap = UINT64_MAX;
    if (cap == UINT64_MAX) {
        const char *e = getenv("SOTC_UNBOUNDED_CAP_MB");
        uint64_t mb = e ? strtoull(e, NULL, 10) : 128;
        cap = mb ? mb << 20 : UINT64_MAX - 1;
    }
    return cap;
}

/* shadPS4 Memory::ClampRangeSize. Requests of 1 GiB or more (shadPS4's
 * MinSizeToClamp) are "unbounded" buffer descriptors (num_records = ~0), and
 * SotC binds thousands of them, many based near the start of its single
 * 3+ GiB direct pool. Clamped to the mapping as shadPS4 does, they made
 * 2-3 GiB ranges resident in the buffer cache's sparse arena: on a 6 GB card
 * that over-commits VRAM (300 ms frames, then a TDR / "Device lost") or fails
 * with ErrorOutOfDeviceMemory. Such a descriptor is therefore bounded by its
 * own mapping and by unbounded_cap(); reads past the cap return zero (robust
 * buffer access). M4: replace with real residency management (AjncSystem #11). */
/* The GPU thread calls this for every buffer bind (~3-4% of its time when it
 * took g_lock and searched each time). Each thread caches the mapped run its
 * last miss walked: [lo, span_hi) is the first span (what an unbounded
 * request may use), [lo, run_hi) the contiguous mapped spans after it. The
 * cache is valid while g_regions_gen is unchanged, so a hit answers as the
 * locked walk would have at that generation; any map or unmap bumps it. */
typedef struct clamp_cache { uint64_t gen, lo, span_hi, run_hi; } clamp_cache;
static _Thread_local clamp_cache t_clamp;

uint64_t hle_memory_clamp_range(uint64_t addr, uint64_t size) {
    const bool unbounded = size >= (1ull << 30);
    uint64_t total = 0;
    const clamp_cache *c = &t_clamp;
    if (c->gen == atomic_load_explicit(&g_regions_gen, memory_order_acquire) && addr >= c->lo) {
        if (unbounded && addr < c->span_hi) {
            total = c->span_hi - addr;
            goto done;
        }
        if (!unbounded && addr < c->run_hi && size <= c->run_hi - addr) return size;
    }
    plat_mutex_lock(g_lock);
    clamp_cache fill = {atomic_load_explicit(&g_regions_gen, memory_order_relaxed), addr, 0, 0};
    for (uint64_t a = addr; total < size && a < PLAT_GUEST_LIMIT;) {
        span sp = span_at(a);
        if (!sp.mapped) break;
        if (a == addr) {
            if (sp.r) fill.lo = sp.r->start; /* image spans start at addr (conservative) */
            fill.span_hi = sp.end;
        }
        total += sp.end - a;
        a = sp.end;
        fill.run_hi = a;
        if (unbounded) break;
    }
    plat_mutex_unlock(g_lock);
    if (fill.span_hi) t_clamp = fill;
done:
    if (unbounded && total > unbounded_cap()) total = unbounded_cap();
    return total < size ? total : size;
}

bool hle_memory_is_gpu_mapping(uint64_t addr, uint64_t size) {
    if (addr >= PLAT_GUEST_LIMIT || size > PLAT_GUEST_LIMIT - addr) return false;
    return hle_memory_clamp_range(addr, size) == size;
}

void hle_memory_for_each_mapping(void (*fn)(uint64_t, uint64_t, void *), void *ctx) {
    if (!g_lock) return;
    plat_mutex_lock(g_lock);
    uint32_t n = 0;
    uint64_t *snap = malloc((size_t)(g_region_count ? g_region_count : 1) * 2 * sizeof(uint64_t));
    for (uint32_t i = 0; snap && i < g_region_count; i++)
        if (g_regions[i].kind != R_RESERVED) {
            snap[2 * n] = g_regions[i].start;
            snap[2 * n + 1] = g_regions[i].size;
            n++;
        }
    plat_mutex_unlock(g_lock);
    for (uint32_t i = 0; i < n; i++) fn(snap[2 * i], snap[2 * i + 1], ctx);
    free(snap);
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
