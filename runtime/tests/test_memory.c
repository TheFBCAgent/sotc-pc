/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Guest memory: direct memory allocation, aliasing of physical memory,
 * partial unmaps, address reuse and protection. */
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define t_open _open
#define t_read _read
#define t_close _close
#define T_RDONLY (_O_RDONLY | _O_BINARY)
#else
#include <fcntl.h>
#include <unistd.h>
#define t_open open
#define t_read read
#define t_close close
#define T_RDONLY O_RDONLY
#endif

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

typedef SYSV int32_t (*alloc_fn)(int64_t, int64_t, uint64_t, uint64_t, int32_t, int64_t *);
typedef SYSV int32_t (*avail_fn)(uint64_t, uint64_t, uint64_t, uint64_t *, uint64_t *);
typedef SYSV int32_t (*map_direct_fn)(void **, uint64_t, int32_t, int32_t, int64_t, uint64_t);
typedef SYSV int32_t (*map_flex_fn)(void **, uint64_t, int32_t, int32_t);
typedef SYSV int32_t (*reserve_fn)(void **, uint64_t, int32_t, uint64_t);
typedef SYSV int32_t (*munmap_fn)(void *, uint64_t);
typedef SYSV int32_t (*mprotect_fn)(const void *, uint64_t, int32_t);
typedef struct vq_info { /* SceKernelVirtualQueryInfo */
    uint64_t start, end, offset;
    int32_t protection, memory_type;
    uint8_t flags; /* is_flexible:1 is_direct:1 is_stack:1 is_pooled:1 is_committed:1 */
    char name[32];
} vq_info;
typedef SYSV int32_t (*vquery_fn)(const void *, int32_t, vq_info *, uint64_t);

#define RW 0x33 /* CPU and GPU read/write */
#define FIXED 0x10
#define MiB (1ull << 20)

int main(void) {
    hle_init();
    hle_memory_init();
    alloc_fn alloc = (alloc_fn)hle_lookup("libkernel", "sceKernelAllocateDirectMemory");
    avail_fn avail = (avail_fn)hle_lookup("libkernel", "sceKernelAvailableDirectMemorySize");
    map_direct_fn map_direct = (map_direct_fn)hle_lookup("libkernel", "sceKernelMapDirectMemory");
    map_flex_fn map_flex = (map_flex_fn)hle_lookup("libkernel", "sceKernelMapFlexibleMemory");
    reserve_fn reserve = (reserve_fn)hle_lookup("libkernel", "sceKernelReserveVirtualRange");
    munmap_fn unmap = (munmap_fn)hle_lookup("libkernel", "sceKernelMunmap");
    mprotect_fn protect = (mprotect_fn)hle_lookup("libkernel", "sceKernelMprotect");
    vquery_fn vquery = (vquery_fn)hle_lookup("libkernel", "sceKernelVirtualQuery");
    CHECK(alloc && avail && map_direct && map_flex && reserve && unmap && protect && vquery);
    if (g_failures) return 1;

    /* Allocation searches [start, end): a block near the top doesn't use up
     * the space below it (the game allocates its pools this way). */
    int64_t top = 0, low = 0, pool = 0;
    CHECK(alloc(0x147de0000, 0x148000000, 0x220000, 0x10000, 0, &top) == SCE_OK &&
          top == 0x147de0000);
    CHECK(alloc(0, 0x148000000, 16 * MiB, 0x10000, 0, &low) == SCE_OK && low == 0);
    CHECK(alloc(0, 0x148000000, 64 * MiB, 2 * MiB, 3, &pool) == SCE_OK && pool == 16 * MiB);
    uint64_t at = 0, size = 0;
    CHECK(avail(0, 0x148000000, 0x10000, &at, &size) == SCE_OK && at == 80 * MiB &&
          size == 0x147de0000 - 80 * MiB);
    int64_t none = 0;
    CHECK(alloc(0x147de0000, 0x148000000, 0x10000, 0, 0, &none) != SCE_OK);

    /* Two mappings of the same physical range alias each other. */
    void *a = NULL, *b = NULL;
    CHECK(map_direct(&a, 4 * MiB, RW, 0, pool, 0x10000) == SCE_OK && a);
    CHECK(map_direct(&b, 4 * MiB, RW, 0, pool, 0x10000) == SCE_OK && b && b != a);
    ((volatile uint32_t *)a)[100] = 0x12345678u;
    CHECK(((volatile uint32_t *)b)[100] == 0x12345678u);
    /* A third mapping at an offset sees the same bytes at the offset. */
    void *c = NULL;
    CHECK(map_direct(&c, 1 * MiB, RW, 0, pool + 1 * MiB, 0x10000) == SCE_OK);
    ((volatile uint8_t *)b)[1 * MiB + 5] = 0xAB;
    CHECK(((volatile uint8_t *)c)[5] == 0xAB);

    /* Data survives unmapping every view and mapping again. */
    CHECK(unmap(a, 4 * MiB) == SCE_OK && unmap(b, 4 * MiB) == SCE_OK && unmap(c, 1 * MiB) == 0);
    void *d = NULL;
    CHECK(map_direct(&d, 4 * MiB, RW, 0, pool, 0x10000) == SCE_OK);
    CHECK(((volatile uint32_t *)d)[100] == 0x12345678u);

    /* Partial unmap keeps both remaining pieces (views) with their data. */
    uint8_t *db = d;
    db[0] = 1;
    db[3 * MiB] = 3;
    CHECK(unmap(db + 1 * MiB, 1 * MiB) == SCE_OK);
    CHECK(db[0] == 1 && db[3 * MiB] == 3);
    /* ...and the hole can be mapped again, fixed. */
    void *hole = db + 1 * MiB;
    CHECK(map_direct(&hole, 1 * MiB, RW, FIXED, pool + 1 * MiB, 0) == SCE_OK &&
          hole == db + 1 * MiB);
    CHECK(((volatile uint8_t *)hole)[5] == 0xAB);

    /* Reserve a range, then map direct memory fixed inside it. */
    void *r = NULL;
    CHECK(reserve(&r, 8 * MiB, 0, 2 * MiB) == SCE_OK && r && ((uintptr_t)r & (2 * MiB - 1)) == 0);
    void *in = (uint8_t *)r + 2 * MiB;
    CHECK(map_direct(&in, 2 * MiB, RW, FIXED, pool, 0) == SCE_OK && in == (uint8_t *)r + 2 * MiB);
    CHECK(((volatile uint32_t *)in)[100] == 0x12345678u);
    /* VirtualQuery reports the reserved parts as uncommitted, with no
     * protection, instead of failing: SotC's heap walks ranges with it and
     * ignores errors (it spun forever at the title screen). */
    vq_info qi;
    memset(&qi, 0xCC, sizeof(qi));
    CHECK(vquery(r, 0, &qi, sizeof(qi)) == SCE_OK && qi.start == (uint64_t)(uintptr_t)r &&
          qi.end == (uint64_t)(uintptr_t)in && !(qi.flags & 0x10) && qi.protection == 0);
    CHECK(vquery(in, 0, &qi, sizeof(qi)) == SCE_OK && (qi.flags & 0x12) == 0x12 &&
          qi.end == (uint64_t)(uintptr_t)in + 2 * MiB && qi.offset == (uint64_t)pool);
    CHECK(vquery((uint8_t *)in + 2 * MiB, 0, &qi, sizeof(qi)) == SCE_OK && !(qi.flags & 0x10) &&
          qi.end == (uint64_t)(uintptr_t)r + 8 * MiB);
    CHECK(unmap(r, 8 * MiB) == SCE_OK);
    /* Free memory still fails without FIND_NEXT. */
    CHECK(vquery(r, 0, &qi, sizeof(qi)) != SCE_OK);

    /* Flexible memory: private, zeroed; a partial unmap keeps the rest's data
     * even at a 16 KiB (non-64 KiB) boundary. */
    void *f = NULL;
    CHECK(map_flex(&f, 256 * 1024, 3, 0) == SCE_OK && f);
    uint8_t *fb = f;
    CHECK(fb[0] == 0 && fb[200000] == 0);
    fb[0] = 7;
    fb[0x8000] = 8;
    CHECK(unmap(fb + 0x4000, 0x4000) == SCE_OK);
    CHECK(fb[0] == 7 && fb[0x8000] == 8);

    /* Freed address space gets reused. */
    void *e1 = NULL, *e2 = NULL;
    CHECK(map_flex(&e1, 1 * MiB, 3, 0) == SCE_OK);
    CHECK(unmap(e1, 1 * MiB) == SCE_OK);
    CHECK(map_flex(&e2, 1 * MiB, 3, 0) == SCE_OK && e2 == e1);

    /* A large view with its head unmapped keeps the rest (the game unmaps
     * the first 8 MiB of its 1 GiB pool mapping). */
    int64_t big = 0;
    CHECK(alloc(0, 0x148000000, 1024 * MiB, 0x10000, 0, &big) == SCE_OK);
    void *g = NULL;
    CHECK(map_direct(&g, 1024 * MiB, RW, 0, big, 0x10000) == SCE_OK);
    uint8_t *gb = g;
    gb[0x1b1ffff4] = 9;
    CHECK(unmap(gb, 8 * MiB) == SCE_OK);
    CHECK(gb[0x1b1ffff4] == 9);
    gb[1023 * MiB] = 1;
    CHECK(gb[1023 * MiB] == 1);

    /* The host kernel can read files straight into views, fresh or split. */
    {
        const char *tmp = "test_memory.tmp";
        FILE *fp = fopen(tmp, "wb");
        static uint8_t pattern[256 * 1024];
        for (size_t i = 0; i < sizeof pattern; i++) pattern[i] = (uint8_t)(i * 7);
        fwrite(pattern, 1, sizeof pattern, fp);
        fclose(fp);
        int fd = t_open(tmp, T_RDONLY);
        CHECK(fd >= 0);
        CHECK(t_read(fd, gb + 64 * MiB, sizeof pattern) == (int)sizeof pattern);
        CHECK(memcmp(gb + 64 * MiB, pattern, sizeof pattern) == 0);
        void *fresh = NULL;
        CHECK(map_direct(&fresh, 2 * MiB, RW, 0, pool + 8 * MiB, 0x10000) == SCE_OK);
        t_close(fd);
        fd = t_open(tmp, T_RDONLY);
        CHECK(t_read(fd, fresh, sizeof pattern) == (int)sizeof pattern);
        t_close(fd);
        remove(tmp);
    }

    /* Protection across two units. */
    CHECK(protect(d, 4 * MiB, 1) == SCE_OK);
    CHECK(protect(d, 4 * MiB, RW) == SCE_OK);

    if (g_failures) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("test_memory: ok\n");
    return 0;
}
