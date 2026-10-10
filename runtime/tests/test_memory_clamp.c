/* SPDX-License-Identifier: GPL-2.0-or-later */
/* hle_memory_clamp_range and its per-thread cache (workstream P1): hits,
 * misses, invalidation after unmap/remap (also from another thread), a
 * randomized comparison with a model, and a concurrent map/unmap stress.
 * Session 33: the lock-free snapshot (built after CLAMP_SNAPSHOT_WALKS locked
 * walks at one generation): alternating regions, generation bumps, the image. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_bridge.h"
#include "hle/hle.h"
#include "platform.h"
#include "runtime.h"

static int g_failures;
#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                         \
        }                                                                         \
    } while (0)

typedef SYSV int32_t (*map_flex_fn)(void **, uint64_t, int32_t, int32_t);
typedef SYSV int32_t (*reserve_fn)(void **, uint64_t, int32_t, uint64_t);
typedef SYSV int32_t (*munmap_fn)(void *, uint64_t);

#define RW 0x33
#define FIXED 0x10
#define MiB (1ull << 20)
#define GiB (1ull << 30)
#define BASE 0x1100000000ull /* inside the arena, away from other tests' maps */

static map_flex_fn map_flex;
static reserve_fn reserve;
static munmap_fn unmap;

static bool map_at(uint64_t addr, uint64_t len) {
    void *p = (void *)(uintptr_t)addr;
    return map_flex(&p, len, RW, FIXED) == 0 && (uint64_t)(uintptr_t)p == addr;
}

static uint64_t clamp(uint64_t addr, uint64_t size) { return hle_memory_clamp_range(addr, size); }

/* Enough locked walks at the current generation for this thread to build its
 * snapshot (memory.c's CLAMP_SNAPSHOT_WALKS is 256). */
static bool build_snapshot(void) {
    for (int i = 0; i < 300 && !hle_memory_clamp_has_snapshot(); i++) clamp(BASE - 0x1000, 0x10);
    return hle_memory_clamp_has_snapshot();
}

/* ---- another thread unmaps, then this thread must see it ------------------ */
typedef struct { uint64_t addr, len; } range_arg;
static void unmap_thread(void *arg) {
    range_arg *r = arg;
    unmap((void *)(uintptr_t)r->addr, r->len);
}

/* ---- stress: a writer maps/unmaps while a reader clamps ------------------- */
#define STRESS_ADDR (BASE + 0x40000000ull)
#define STRESS_LEN (64 * 1024ull)
static atomic_int g_stop;
static atomic_int g_bad;
static void reader_thread(void *arg) {
    (void)arg;
    while (!atomic_load(&g_stop)) {
        uint64_t n = clamp(STRESS_ADDR + 0x4000, 0x8000);
        if (n != 0 && n != 0x8000) atomic_fetch_add(&g_bad, 1);
        n = clamp(STRESS_ADDR, 2 * GiB);
        if (n != 0 && n != STRESS_LEN) atomic_fetch_add(&g_bad, 1);
    }
}

/* ---- randomized model: 16 one-MiB slots, each its own region or a hole ---- */
#define SLOTS 16
#define SLOT MiB
static uint64_t model_clamp(const bool *mapped, uint64_t addr, uint64_t size) {
    const bool unbounded = size >= GiB;
    uint64_t total = 0;
    for (uint64_t a = addr; total < size;) {
        uint64_t s = (a - BASE) / SLOT;
        if (a < BASE || s >= SLOTS || !mapped[s]) break;
        uint64_t end = BASE + (s + 1) * SLOT;
        total += end - a;
        a = end;
        if (unbounded) break;
    }
    if (unbounded && total > 128 * MiB) total = 128 * MiB; /* default unbounded cap */
    return total < size ? total : size;
}

static uint32_t g_rng = 12345;
static uint32_t rnd(void) {
    g_rng = g_rng * 1103515245u + 12345u;
    return g_rng >> 8;
}

/* One map or unmap per map_every steps on average, a clamp otherwise.
 * Returns how many clamps the snapshot answered (0 with map_every 4: a
 * generation rarely lasts 256 walks). Leaves every slot unmapped. */
static int random_phase(int steps, uint32_t map_every) {
    bool mapped[SLOTS] = {0};
    int from_snapshot = 0;
    for (int i = 0; i < steps && !g_failures; i++) {
        uint32_t op = rnd() % map_every;
        uint32_t s = rnd() % SLOTS;
        if (op == 0 && !mapped[s]) {
            CHECK(map_at(BASE + s * SLOT, SLOT));
            mapped[s] = true;
        } else if (op == 0 && mapped[s]) {
            CHECK(unmap((void *)(uintptr_t)(BASE + s * SLOT), SLOT) == 0);
            mapped[s] = false;
        } else {
            uint64_t addr = BASE + s * SLOT + (rnd() % (SLOT / 0x1000)) * 0x1000;
            if (rnd() % 8 == 0) addr += SLOT - 0x1000 + rnd() % 0x2000; /* around a slot edge */
            uint64_t size = (rnd() % 4 == 0) ? 2 * GiB : (uint64_t)(rnd() % (3 * SLOT)) + 1;
            from_snapshot += hle_memory_clamp_has_snapshot();
            uint64_t want = model_clamp(mapped, addr, size), got = clamp(addr, size);
            if (got != want) {
                fprintf(stderr, "random step %d: clamp(%#llx, %#llx) = %#llx, want %#llx\n", i,
                        (unsigned long long)addr, (unsigned long long)size,
                        (unsigned long long)got, (unsigned long long)want);
                g_failures++;
            }
        }
    }
    for (uint32_t s = 0; s < SLOTS; s++)
        if (mapped[s]) unmap((void *)(uintptr_t)(BASE + s * SLOT), SLOT);
    return from_snapshot;
}

int main(void) {
    hle_init();
    hle_memory_init();
    map_flex = (map_flex_fn)hle_lookup("libkernel", "sceKernelMapFlexibleMemory");
    reserve = (reserve_fn)hle_lookup("libkernel", "sceKernelReserveVirtualRange");
    unmap = (munmap_fn)hle_lookup("libkernel", "sceKernelMunmap");
    CHECK(map_flex && reserve && unmap);
    if (g_failures) return 1;

    const uint64_t a = BASE, b = BASE + 4 * MiB;

    /* Holes clamp to zero (and must not leave a usable cache entry). */
    CHECK(clamp(a, 0x1000) == 0);
    CHECK(clamp(a, 0x1000) == 0);
    CHECK(clamp(a, 2 * GiB) == 0);

    /* Miss, then hits inside one region. */
    CHECK(map_at(a, 4 * MiB));
    CHECK(clamp(a + 0x1000, 0x1000) == 0x1000);
    CHECK(clamp(a + 0x1000, 0x1000) == 0x1000);
    CHECK(clamp(a, 4 * MiB) == 4 * MiB);
    CHECK(clamp(a + 4 * MiB - 0x10, 0x10) == 0x10);
    CHECK(clamp(a, 8 * MiB) == 4 * MiB);           /* runs into the hole */
    CHECK(clamp(a + MiB, 2 * GiB) == 3 * MiB);     /* unbounded: the mapping */
    CHECK(clamp(a + 4 * MiB, 0x1000) == 0);        /* just past the end */
    CHECK(clamp(a - 0x1000, 0x2000) == 0);         /* just before the start */
    CHECK(clamp(a + 0x1000, 0x1000) == 0x1000);

    /* An adjacent region: bounded requests run across, unbounded stop at the first. */
    CHECK(clamp(a, 0x1000) == 0x1000); /* cache filled before the map below */
    CHECK(map_at(b, 4 * MiB));
    CHECK(clamp(a + MiB, 6 * MiB) == 6 * MiB);
    CHECK(clamp(a + MiB, 7 * MiB + 1) == 7 * MiB);
    CHECK(clamp(a + 5 * MiB, 0x1000) == 0x1000);   /* inside the run's second span */
    CHECK(clamp(a + MiB, 2 * GiB) == 3 * MiB);
    CHECK(clamp(a + 5 * MiB, 2 * GiB) == 3 * MiB); /* run cache must not answer this */
    CHECK(clamp(a + MiB, 2 * GiB) == 3 * MiB);

    /* Invalidation: cached on this thread, unmapped by another. */
    CHECK(clamp(a + 0x2000, 0x1000) == 0x1000);
    range_arg ra = {a, 4 * MiB};
    plat_thread *t = plat_thread_create(unmap_thread, &ra, 256 * 1024);
    CHECK(t != NULL);
    plat_thread_join(t);
    CHECK(clamp(a + 0x2000, 0x1000) == 0);
    CHECK(clamp(a, 2 * GiB) == 0);
    CHECK(clamp(b, 0x1000) == 0x1000);

    /* Remap smaller at the same address. */
    CHECK(clamp(b + 0x1000, 0x1000) == 0x1000);
    CHECK(map_at(a, MiB));
    CHECK(clamp(a, 2 * MiB) == MiB);
    CHECK(clamp(a + 0x1000, 0x1000) == 0x1000);
    CHECK(clamp(a + MiB, 0x1000) == 0);
    CHECK(clamp(a, 2 * GiB) == MiB);

    /* Partial unmap (a split) inside the second region. */
    CHECK(clamp(b + 2 * MiB, 0x1000) == 0x1000);
    CHECK(unmap((void *)(uintptr_t)(b + MiB), MiB) == 0);
    CHECK(clamp(b + MiB + 0x1000, 0x1000) == 0);
    CHECK(clamp(b, 4 * MiB) == MiB);
    CHECK(clamp(b + 2 * MiB, 4 * MiB) == 2 * MiB);
    CHECK(clamp(b + 2 * MiB, 2 * GiB) == 2 * MiB);

    /* Reserved ranges are not mapped memory. */
    CHECK(unmap((void *)(uintptr_t)b, 4 * MiB) == 0);
    CHECK(unmap((void *)(uintptr_t)a, MiB) == 0);
    void *r = (void *)(uintptr_t)a;
    CHECK(reserve(&r, 4 * MiB, FIXED, 0) == 0);
    CHECK(clamp(a, 0x1000) == 0);
    CHECK(clamp(a, 2 * GiB) == 0);
    CHECK(unmap((void *)(uintptr_t)a, 4 * MiB) == 0);

    /* Snapshot: regions A | hole | B C (adjacent) | reserved R | D, queried in
     * alternation as the GPU thread does, then generation bumps. */
    {
        const uint64_t A = BASE, B = BASE + 2 * MiB, C = BASE + 3 * MiB, R = BASE + 5 * MiB,
                       D = BASE + 6 * MiB;
        CHECK(map_at(A, MiB) && map_at(B, MiB) && map_at(C, 2 * MiB) && map_at(D, MiB));
        void *rp = (void *)(uintptr_t)R;
        CHECK(reserve(&rp, MiB, FIXED, 0) == 0);
        CHECK(!hle_memory_clamp_has_snapshot());
        CHECK(build_snapshot());
        for (int i = 0; i < 3; i++) {
            CHECK(clamp(A + 0x10, 0x100) == 0x100);
            CHECK(clamp(B + 0x1000, 3 * MiB) == 3 * MiB - 0x1000); /* runs into C, stops at R */
            CHECK(clamp(D + MiB - 0x10, 0x20) == 0x10);
            CHECK(clamp(A + MiB, 0x10) == 0);                     /* the hole */
            CHECK(clamp(C - 0x10, 0x20) == 0x20);                 /* across B|C */
            CHECK(clamp(R, 0x10) == 0);                           /* reserved */
            CHECK(clamp(B + 0x1000, 2 * GiB) == MiB - 0x1000);    /* unbounded: B only */
            CHECK(clamp(C + MiB, 2 * GiB) == MiB);
            CHECK(clamp(A, 2 * GiB) == MiB);
            CHECK(clamp(A - 1, 0x10) == 0);
            CHECK(clamp(D, 0) == 0);
            CHECK(clamp(PLAT_GUEST_LIMIT, 0x10) == 0);
            CHECK(hle_memory_is_gpu_mapping(C, 2 * MiB));
            CHECK(!hle_memory_is_gpu_mapping(C, 2 * MiB + 1));
        }
        CHECK(hle_memory_clamp_has_snapshot());

        /* Bumps: an unmap on another thread, then a map filling the hole. */
        range_arg rc = {C + MiB, MiB};
        plat_thread *tc = plat_thread_create(unmap_thread, &rc, 256 * 1024);
        CHECK(tc != NULL);
        plat_thread_join(tc);
        CHECK(!hle_memory_clamp_has_snapshot());
        CHECK(clamp(B, 3 * MiB) == 2 * MiB);
        CHECK(clamp(C + MiB, 0x10) == 0);
        CHECK(build_snapshot());
        CHECK(clamp(B, 3 * MiB) == 2 * MiB);
        CHECK(clamp(C + MiB, 0x10) == 0);
        CHECK(clamp(C, 2 * GiB) == MiB);
        CHECK(map_at(A + MiB, MiB));
        CHECK(!hle_memory_clamp_has_snapshot());
        CHECK(clamp(A, 8 * MiB) == 4 * MiB); /* A, the new map, B, C */
        CHECK(build_snapshot());
        CHECK(clamp(A, 8 * MiB) == 4 * MiB);
        CHECK(clamp(A + 0x1000, 2 * GiB) == MiB - 0x1000);

        /* The loaded image (mapped by the loader, not a region): mapped where
         * no region covers it, contiguous with regions next to it. A reserved
         * region over part of it makes that part unmapped. */
        void *saved_image = g_rt.image;
        uint64_t saved_size = g_rt.image_size;
        const uint64_t I = BASE + 16 * MiB;
        g_rt.image = (void *)(uintptr_t)I;
        g_rt.image_size = 4 * MiB;
        CHECK(map_at(I + 4 * MiB, MiB)); /* adjacent after the image; bumps the generation */
        void *ir = (void *)(uintptr_t)(I + MiB);
        CHECK(reserve(&ir, MiB, FIXED, 0) == 0); /* hides [I+1M, I+2M) */
        for (int pass = 0; pass < 2; pass++) {
            if (pass) CHECK(build_snapshot());
            else CHECK(!hle_memory_clamp_has_snapshot());
            CHECK(clamp(I, 2 * MiB) == MiB);
            CHECK(clamp(I + MiB, 0x10) == 0);
            CHECK(clamp(I + 2 * MiB, 4 * MiB) == 3 * MiB); /* image, then the region */
            CHECK(clamp(I + 2 * MiB, 2 * GiB) == 2 * MiB); /* unbounded: the image span */
            CHECK(clamp(I + 5 * MiB, 0x10) == 0);
            CHECK(clamp(I - 0x10, 0x20) == 0);
        }
        CHECK(unmap((void *)(uintptr_t)(I + MiB), MiB) == 0);
        CHECK(unmap((void *)(uintptr_t)(I + 4 * MiB), MiB) == 0);
        g_rt.image = saved_image;
        g_rt.image_size = saved_size;

        CHECK(unmap((void *)(uintptr_t)A, 7 * MiB) == 0);
        CHECK(clamp(B, 0x10) == 0);
    }

    /* Randomized comparison with a model of separate one-MiB regions: maps
     * often (mostly locked walks), then rarely (mostly snapshot lookups). */
    CHECK(random_phase(4000, 4) == 0);
    CHECK(random_phase(40000, 2000) > 20000);

    /* Concurrent map/unmap: the reader only ever sees all or nothing, and after
     * the writer's last change it sees the final state. */
    plat_thread *rd = plat_thread_create(reader_thread, NULL, 256 * 1024);
    CHECK(rd != NULL);
    for (int i = 0; i < 2000; i++) {
        CHECK(map_at(STRESS_ADDR, STRESS_LEN));
        CHECK(unmap((void *)(uintptr_t)STRESS_ADDR, STRESS_LEN) == 0);
    }
    atomic_store(&g_stop, 1);
    plat_thread_join(rd);
    CHECK(atomic_load(&g_bad) == 0);
    CHECK(clamp(STRESS_ADDR + 0x4000, 0x8000) == 0);
    CHECK(map_at(STRESS_ADDR, STRESS_LEN));
    CHECK(clamp(STRESS_ADDR + 0x4000, 0x8000) == 0x8000);
    CHECK(unmap((void *)(uintptr_t)STRESS_ADDR, STRESS_LEN) == 0);

    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_memory_clamp: ok\n");
    return 0;
}
