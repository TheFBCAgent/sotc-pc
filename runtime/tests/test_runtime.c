/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Runtime unit tests that need no game files. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "platform.h"
#include "runtime.h"

static int g_failures;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            g_failures++;                                                             \
        }                                                                             \
    } while (0)

/* A rewritten `mov rax, fs:[0]` must return the TCB set for this thread. */
static void test_tls_rewrite(void) {
    static const uint8_t code[] = {0x64, 0x48, 0x8B, 0x04, 0x25, 0, 0, 0, 0, 0xC3};
    uint8_t *buf = plat_map(0, 0x10000, PLAT_PROT_R | PLAT_PROT_W, false);
    CHECK(buf != NULL);
    if (!buf) return;
    CHECK((uint64_t)(uintptr_t)buf < PLAT_GUEST_LIMIT);
    memcpy(buf, code, sizeof(code));
    uint32_t disp = plat_tls_patch_disp(0);
    buf[0] = 0x65;
    memcpy(buf + 5, &disp, 4);
    CHECK(plat_protect(buf, 0x10000, PLAT_PROT_R | PLAT_PROT_X));
    plat_flush_icache(buf, sizeof(code));

    static uint64_t fake_tcb[8];
    fake_tcb[0] = (uint64_t)(uintptr_t)fake_tcb; /* tcb->self */
    plat_set_guest_tcb(fake_tcb);
    SYSV uint64_t (*fn)(void) = (SYSV uint64_t (*)(void))(void *)buf;
    CHECK(fn() == (uint64_t)(uintptr_t)fake_tcb);
    plat_unmap(buf, 0x10000);
}

static void test_sse4a_semantics(void) {
    uint64_t v = 0x123456789ABCDEF0ull;
    sse4a_extrq(&v, 8, 4);
    CHECK(v == 0xEF);
    v = 0x123456789ABCDEF0ull;
    sse4a_extrq(&v, 0, 0); /* len 0 means 64 */
    CHECK(v == 0x123456789ABCDEF0ull);
    v = ~0ull;
    sse4a_insertq(&v, 0, 8, 8);
    CHECK(v == 0xFFFFFFFFFFFF00FFull);
    v = 0;
    sse4a_insertq(&v, 0xABCD, 16, 48);
    CHECK(v == 0xABCD000000000000ull);
}

/* On CPUs with SSE4a, the emulation must match the real instructions. */
__attribute__((target("sse4a"))) static void test_sse4a_vs_hardware(void) {
    if (!plat_cpu_has_sse4a()) {
        printf("  (no SSE4a on this CPU: hardware comparison skipped)\n");
        return;
    }
    uint64_t seed = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 20000; i++) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        uint64_t a = seed, b = seed * 0x2545F4914F6CDD1Dull;
        uint8_t len = (uint8_t)(b & 63), idx = (uint8_t)((b >> 8) & 63);
        if (len + idx > 64 || (len == 0 && idx != 0)) continue; /* undefined on hardware */

        uint64_t ctl[2] = {(uint64_t)len | ((uint64_t)idx << 8), 0}, x[2] = {a, 0};
        __asm__("movdqu %1, %%xmm0\n movdqu %2, %%xmm1\n extrq %%xmm1, %%xmm0\n movq %%xmm0, %0"
                : "=r"(x[0]) : "m"(x), "m"(ctl) : "xmm0", "xmm1");
        uint64_t emu = a;
        sse4a_extrq(&emu, len, idx);
        CHECK(emu == x[0]);

        uint64_t src[2] = {b ^ a, (uint64_t)len | ((uint64_t)idx << 8)}, d[2] = {a, 0};
        __asm__("movdqu %1, %%xmm0\n movdqu %2, %%xmm1\n insertq %%xmm1, %%xmm0\n movq %%xmm0, %0"
                : "=r"(d[0]) : "m"(d), "m"(src) : "xmm0", "xmm1");
        emu = a;
        sse4a_insertq(&emu, b ^ a, len, idx);
        CHECK(emu == d[0]);
        if (g_failures) {
            fprintf(stderr, "  a=%#llx b=%#llx len=%u idx=%u\n", (unsigned long long)a,
                    (unsigned long long)b, len, idx);
            return;
        }
    }
}

static void test_reserve_commit(void) {
    const uint64_t base = 0x3000000000ull, size = 0x1000000;
    uint8_t *p = plat_reserve(base, size);
    CHECK(p == (uint8_t *)(uintptr_t)base);
    if (!p) return;
    CHECK(plat_commit(p + 0x10000, 0x10000, PLAT_PROT_R | PLAT_PROT_W));
    p[0x10000] = 42;
    CHECK(plat_commit(p + 0x10000, 0x10000, PLAT_PROT_R | PLAT_PROT_W));
    CHECK(p[0x10000] == 0); /* recommitted pages come back zeroed */
    plat_decommit(p + 0x10000, 0x10000);
    plat_unmap(p, size);
}

static void test_mutex_cond(void) {
    plat_mutex *m = plat_mutex_new();
    plat_cond *c = plat_cond_new();
    plat_mutex_lock(m);
    plat_mutex_lock(m); /* recursive */
    plat_mutex_unlock(m);
    uint64_t t0 = plat_monotonic_ns();
    CHECK(!plat_cond_wait(c, m, 20000)); /* times out */
    CHECK(plat_monotonic_ns() - t0 >= 10000000ull);
    plat_mutex_unlock(m);
    plat_cond_free(c);
    plat_mutex_free(m);
}

int main(void) {
    log_init();
    CHECK(plat_tls_init());
    test_tls_rewrite();
    test_sse4a_semantics();
    test_sse4a_vs_hardware();
    test_reserve_commit();
    test_mutex_cond();
    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all runtime tests passed\n");
    return 0;
}
