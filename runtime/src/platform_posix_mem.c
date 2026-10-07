/* SPDX-License-Identifier: GPL-2.0-or-later */
/* POSIX: memory, the guest arena and physical memory. */
#ifndef _WIN32

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include "gpu_bridge.h"
#include "log.h"
#include "platform.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

/* ---- memory ------------------------------------------------------------ */

static int posix_prot(int prot) {
    int p = 0;
    if (prot & PLAT_PROT_R) p |= PROT_READ;
    if (prot & PLAT_PROT_W) p |= PROT_READ | PROT_WRITE;
    if (prot & PLAT_PROT_X) p |= PROT_READ | PROT_EXEC;
    return p;
}

static void *try_map(uint64_t addr, size_t size, int prot) {
    void *p = mmap((void *)(uintptr_t)addr, size, posix_prot(prot),
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return NULL;
    if ((uint64_t)(uintptr_t)p != addr) { /* old kernels ignore NOREPLACE */
        munmap(p, size);
        return NULL;
    }
    return p;
}

void *plat_map(uint64_t addr, size_t size, int prot, bool fixed) {
    if (fixed) return try_map(addr, size, prot);
    const uint64_t step = 0x10000;
    uint64_t start = addr ? (addr + step - 1) & ~(step - 1) : 0x10000000ull;
    for (uint64_t cur = start; cur + size <= PLAT_GUEST_LIMIT;) {
        void *p = try_map(cur, size, prot);
        if (p) return p;
        /* Skip ahead in growing strides so a crowded area is passed quickly. */
        cur += size > step ? ((size + step - 1) & ~(step - 1)) : step;
    }
    return NULL;
}

void plat_unmap(void *addr, size_t size) { munmap(addr, size); }

void *plat_reserve(uint64_t addr, size_t size) {
    void *p = mmap((void *)(uintptr_t)addr, size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

bool plat_commit(void *addr, size_t size, int prot) {
    return mmap(addr, size, posix_prot(prot), MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) !=
           MAP_FAILED;
}

void plat_decommit(void *addr, size_t size) {
    mmap(addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
}

/* Guest arena: physical memory is a memfd, views are MAP_SHARED mappings of
 * it, and free arena space is PROT_NONE reservations. */
static int g_phys_fd = -1;

bool plat_phys_init(uint64_t size) {
    g_phys_fd = memfd_create("sotc-phys", 0);
    if (g_phys_fd < 0 || ftruncate(g_phys_fd, (off_t)size) != 0) {
        log_error("memory: cannot create the physical memory file (%d)", errno);
        return false;
    }
    return true;
}

void *plat_arena_reserve(uint64_t addr, uint64_t size) { return plat_reserve(addr, size); }

bool plat_arena_map(uint64_t addr, uint64_t size, int prot, int64_t phys, bool commit) {
    (void)commit;
    void *p = phys >= 0 ? mmap((void *)(uintptr_t)addr, size, posix_prot(prot),
                               MAP_SHARED | MAP_FIXED, g_phys_fd, (off_t)phys)
                        : mmap((void *)(uintptr_t)addr, size, posix_prot(prot),
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    return p != MAP_FAILED;
}

void plat_arena_unmap(uint64_t addr, uint64_t size, bool is_view) {
    (void)is_view;
    plat_decommit((void *)(uintptr_t)addr, size);
}

bool plat_protect(void *addr, size_t size, int prot) {
    return mprotect(addr, size, posix_prot(prot)) == 0;
}

/* ---- extras for the GPU bridge (declared in gpu_bridge.h) ---------------- */

/* Host protection of the page at addr from /proc/self/maps; -1 if unmapped. */
int plat_query_prot(const void *addr, uint64_t *run_end) {
    uint64_t a = (uint64_t)(uintptr_t)addr;
    int result = -1;
    if (run_end) *run_end = 0;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return -1;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long long lo, hi;
        char perms[8];
        if (sscanf(line, "%llx-%llx %7s", &lo, &hi, perms) != 3) continue;
        if (a < lo || a >= hi) continue;
        result = (perms[0] == 'r' ? PLAT_PROT_R : 0) | (perms[1] == 'w' ? PLAT_PROT_W : 0) |
                 (perms[2] == 'x' ? PLAT_PROT_X : 0);
        if (run_end) *run_end = hi;
        break;
    }
    fclose(f);
    return result;
}

/* A second mapping of all physical memory (shadPS4 Memory::TryWriteBacking). */
static void *g_phys_backing;

void *plat_phys_backing(void) {
    if (g_phys_backing || g_phys_fd < 0) return g_phys_backing;
    off_t size = lseek(g_phys_fd, 0, SEEK_END);
    void *p = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, g_phys_fd, 0);
    if (p == MAP_FAILED) log_error("memory: cannot map the physical memory backing (%d)", errno);
    else g_phys_backing = p;
    return g_phys_backing;
}

void plat_flush_icache(void *addr, size_t size) {
    (void)addr; (void)size; /* x86 keeps instruction and data caches coherent */
}

#endif
