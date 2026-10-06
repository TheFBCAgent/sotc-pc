/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceFiber: ping-pong switching on guest-style stacks, context-less fibers,
 * resume after ReturnToThread (also on another thread), FPU state, errors.
 * Fibers call a host (MS ABI on Windows) function with a 128 KiB frame, which
 * goes through __chkstk and needs the TEB stack bounds swapped. */
#include <stdio.h>
#include <string.h>

#include "hle/hle.h"
#include "tls.h"

#ifdef _WIN32
#include <intrin.h>
#endif

static int g_failures;
#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                         \
        }                                                                         \
    } while (0)

#define ERR_NULL ((int32_t)0x80590001)
#define ERR_INVALID ((int32_t)0x80590004)
#define ERR_PERMISSION ((int32_t)0x80590005)
#define ERR_STATE ((int32_t)0x80590006)

typedef struct __attribute__((aligned(8))) { uint8_t b[256]; } sce_fiber;
typedef struct __attribute__((aligned(8))) {
    uint64_t size;
    void *entry;
    uint64_t arg_on_initialize;
    void *addr_context;
    uint64_t size_context;
    char name[32];
    uint64_t margin;
    uint8_t pad[48];
} sce_fiber_info;
typedef SYSV void (*entry_fn)(uint64_t, uint64_t);

static SYSV int32_t (*fInit)(sce_fiber *, const char *, entry_fn, uint64_t, void *, uint64_t,
                             const void *, uint32_t);
static SYSV int32_t (*fOpt)(uint32_t *);
static SYSV int32_t (*fFinalize)(sce_fiber *);
static SYSV int32_t (*fRun)(sce_fiber *, uint64_t, uint64_t *);
static SYSV int32_t (*fSwitch)(sce_fiber *, uint64_t, uint64_t *);
static SYSV int32_t (*fSelf)(sce_fiber **);
static SYSV int32_t (*fReturn)(uint64_t, uint64_t *);
static SYSV int32_t (*fInfo)(sce_fiber *, sce_fiber_info *);
static SYSV int32_t (*fRename)(sce_fiber *, const char *);

#define STACK_SIZE (512u << 10)

/* Host function with a large frame: __chkstk probes it on Windows. */
__attribute__((noinline)) static uint64_t big_frame(uint64_t seed) {
    volatile uint8_t buf[128 * 1024];
    buf[0] = (uint8_t)seed;
    buf[sizeof(buf) - 1] = (uint8_t)(seed >> 8);
    for (size_t i = 4096; i < sizeof(buf); i += 4096) buf[i] = (uint8_t)i;
    return buf[0] + buf[sizeof(buf) - 1] + buf[8192];
}

/* Inside a fiber with its own stack, the TEB must describe that stack. */
static void check_teb(uint8_t *stack) {
#ifdef _WIN32
    CHECK(__readgsqword(0x8) == (uint64_t)(uintptr_t)(stack + STACK_SIZE));
    CHECK(__readgsqword(0x10) == (uint64_t)(uintptr_t)stack);
#else
    (void)stack;
#endif
    volatile uint8_t probe;
    CHECK((uint8_t *)&probe > stack && (uint8_t *)&probe < stack + STACK_SIZE);
}

static uint32_t get_mxcsr(void) {
    uint32_t v;
    __asm__ volatile("stmxcsr %0" : "=m"(v));
    return v;
}
static void set_mxcsr(uint32_t v) { __asm__ volatile("ldmxcsr %0" : : "m"(v)); }

/* ---- ping-pong -------------------------------------------------------------- */
#define ROUNDS 1000
static sce_fiber g_a, g_b;
static uint8_t *g_stack_a, *g_stack_b;
static int g_a_rounds, g_b_rounds;

static SYSV void entry_a(uint64_t init, uint64_t run) {
    CHECK(init == 0xA);
    CHECK(run == 100);
    uint64_t local = init * 7 + run; /* lives across switches */
    for (int i = 0; i < ROUNDS; i++) {
        check_teb(g_stack_a);
        CHECK(big_frame(i) == (uint8_t)i + (uint8_t)(i >> 8) + 0);
        uint64_t got = 0;
        CHECK(fSwitch(&g_b, 1000 + i, &got) == SCE_OK);
        CHECK(got == 2000u + i);
        sce_fiber *self = NULL;
        CHECK(fSelf(&self) == SCE_OK && self == &g_a);
        g_a_rounds++;
    }
    CHECK(local == 0xA * 7 + 100);
    fReturn(42, NULL);
    CHECK(!"fiber A resumed after its last ReturnToThread");
}

static SYSV void entry_b(uint64_t init, uint64_t run) {
    CHECK(init == 0xB);
    uint64_t local = run * 3;
    for (int i = 0;; i++) {
        CHECK(run == 1000u + i);
        check_teb(g_stack_b);
        big_frame(run);
        CHECK(fSwitch(&g_a, 2000 + i, &run) == SCE_OK);
        g_b_rounds++;
        if (i == 0) CHECK(local == 3000);
    }
}

static void test_ping_pong(void) {
    g_stack_a = plat_map(0, STACK_SIZE, PLAT_PROT_R | PLAT_PROT_W, false);
    g_stack_b = plat_map(0, STACK_SIZE, PLAT_PROT_R | PLAT_PROT_W, false);
    CHECK(g_stack_a && g_stack_b);
    _Alignas(8) uint32_t opt = 0; /* SceFiberOptParam is 8-aligned */
    CHECK(fOpt(&opt) == SCE_OK);
    CHECK(fInit(&g_a, "ping", entry_a, 0xA, g_stack_a, STACK_SIZE, &opt, 0x4508001) == SCE_OK);
    CHECK(fInit(&g_b, "pong", entry_b, 0xB, g_stack_b, STACK_SIZE, NULL, 0x4508001) == SCE_OK);

    uint64_t ret = 0;
    uint64_t teb_base = 0;
#ifdef _WIN32
    teb_base = __readgsqword(0x8);
#endif
    CHECK(fRun(&g_a, 100, &ret) == SCE_OK);
    CHECK(ret == 42);
    CHECK(g_a_rounds == ROUNDS);
    CHECK(g_b_rounds == ROUNDS - 1);
#ifdef _WIN32
    CHECK(__readgsqword(0x8) == teb_base);
#endif
    (void)teb_base;
    CHECK(big_frame(3) == 3);

    /* A is Idle (resumable), B is Idle with a saved context. */
    CHECK(fFinalize(&g_b) == SCE_OK);
    CHECK(fFinalize(&g_b) == ERR_STATE);
    CHECK(fRun(&g_b, 0, NULL) == ERR_STATE);
}

/* ---- context-less fibers ----------------------------------------------------- */
static sce_fiber g_c, g_c2;
static int g_c_entries, g_c2_entries;

static SYSV void entry_c2(uint64_t init, uint64_t run) {
    g_c2_entries++;
    CHECK(run == 77);
    big_frame(run);
    fReturn(run + 1, NULL);
}

static SYSV void entry_c(uint64_t init, uint64_t run) {
    g_c_entries++;
    CHECK(big_frame(run) == (uint8_t)run + (uint8_t)(run >> 8));
    CHECK(fRun(&g_c, 0, NULL) == ERR_PERMISSION);
    if (run == 2) {
        /* Context-less to context-less: C is abandoned, C2 reuses the stack. */
        fSwitch(&g_c2, 77, NULL);
        CHECK(!"context-less fiber resumed");
    }
    uint64_t again = 0;
    fReturn(run * 10, &again);
    CHECK(!"context-less fiber resumed");
}

static void test_contextless(void) {
    CHECK(fInit(&g_c, "nostack", entry_c, 0, NULL, 0, NULL, 0) == SCE_OK);
    CHECK(fInit(&g_c2, "nostack2", entry_c2, 0, NULL, 0, NULL, 0) == SCE_OK);
    uint64_t ret = 0;
    CHECK(fRun(&g_c, 1, &ret) == SCE_OK && ret == 10);
    CHECK(fRun(&g_c, 3, &ret) == SCE_OK && ret == 30); /* restarts at entry */
    CHECK(g_c_entries == 2);
    CHECK(fRun(&g_c, 2, &ret) == SCE_OK && ret == 78);
    CHECK(g_c_entries == 3 && g_c2_entries == 1);
    CHECK(fFinalize(&g_c) == SCE_OK);
    CHECK(fFinalize(&g_c2) == SCE_OK);
}

/* ---- resume after ReturnToThread, on another thread, FPU state --------------- */
static sce_fiber g_d;
static uint8_t *g_stack_d;
static uint32_t g_d_mxcsr;

static SYSV void entry_d(uint64_t init, uint64_t run) {
    g_d_mxcsr = get_mxcsr(); /* SDK >= 3.50: fresh fibers get 0x9fc0 */
    set_mxcsr(0x1f80 | 0x6000); /* round toward zero, only inside this fiber */
    uint64_t next = 0;
    CHECK(fReturn(run + 1, &next) == SCE_OK);
    check_teb(g_stack_d);
    big_frame(next);
    CHECK((get_mxcsr() & ~0x3fu) == (0x1f80 | 0x6000));
    CHECK(fReturn(next + 1, &next) == SCE_OK);
    fReturn(next + 1, NULL);
}

static void other_thread(void *arg) {
    guest_tcb *tcb = tls_create(NULL);
    tls_set_current(tcb);
    uint64_t ret = 0;
    CHECK(fRun(&g_d, 20, &ret) == SCE_OK && ret == 21);
    *(int *)arg = 1;
    tls_set_current(NULL);
    tls_destroy(tcb);
}

static void test_resume(void) {
    g_stack_d = plat_map(0, STACK_SIZE, PLAT_PROT_R | PLAT_PROT_W, false);
    CHECK(fInit(&g_d, "resume", entry_d, 0, g_stack_d, STACK_SIZE, NULL, 0x3500000) == SCE_OK);
    uint32_t host_mxcsr = get_mxcsr();
    uint64_t ret = 0;
    CHECK(fRun(&g_d, 1, &ret) == SCE_OK && ret == 2);
    CHECK(g_d_mxcsr == 0x9fc0);
    CHECK((get_mxcsr() & ~0x3fu) == (host_mxcsr & ~0x3fu));

    /* D is Idle with a saved context: resume it
     * from a different host thread, then back here. */
    int done = 0;
    plat_thread *t = plat_thread_create(other_thread, &done, 1u << 20);
    plat_thread_join(t);
    CHECK(done == 1);
    CHECK(fRun(&g_d, 30, &ret) == SCE_OK && ret == 31);
}

/* ---- argument checks --------------------------------------------------------- */
static SYSV void entry_never(uint64_t a, uint64_t b) { CHECK(!"entry_never ran"); }

static void test_errors(void) {
    sce_fiber f;
    uint8_t *stack = g_stack_a;
    CHECK(fInit(NULL, "x", entry_never, 0, NULL, 0, NULL, 0) == ERR_NULL);
    CHECK(fInit(&f, "x", entry_never, 0, stack, 256, NULL, 0) == (int32_t)0x80590003);
    CHECK(fInit(&f, "x", entry_never, 0, stack, 1000, NULL, 0) == ERR_INVALID);
    CHECK(fInit(&f, "x", entry_never, 0, stack + 8, 4096, NULL, 0) == (int32_t)0x80590002);
    _Alignas(8) uint32_t bad_opt = 1;
    CHECK(fInit(&f, "x", entry_never, 0, NULL, 0, &bad_opt, 0) == ERR_INVALID);
    CHECK(fInit(&f, "a-very-long-fiber-name-that-is-truncated", entry_never, 5, stack, 4096,
                NULL, 0) == SCE_OK);

    sce_fiber_info info;
    memset(&info, 0, sizeof(info));
    CHECK(fInfo(&f, &info) == ERR_INVALID); /* size not set */
    info.size = sizeof(info);
    CHECK(fInfo(&f, &info) == SCE_OK);
    CHECK(strcmp(info.name, "a-very-long-fiber-name-that-is-") == 0);
    CHECK(info.entry == (void *)entry_never && info.arg_on_initialize == 5);
    CHECK(info.addr_context == stack && info.size_context == 4096);
    CHECK(fRename(&f, "renamed") == SCE_OK);
    CHECK(fInfo(&f, &info) == SCE_OK && strcmp(info.name, "renamed") == 0);

    CHECK(fSwitch(&f, 0, NULL) == ERR_PERMISSION); /* not inside a fiber */
    CHECK(fReturn(0, NULL) == ERR_PERMISSION);
    sce_fiber *self;
    CHECK(fSelf(&self) == ERR_PERMISSION);
    CHECK(fFinalize(&f) == SCE_OK);
    CHECK(fRun(&f, 0, NULL) == ERR_STATE);
    memset(&f, 0, sizeof(f));
    CHECK(fRun(&f, 0, NULL) == ERR_INVALID);
}

int main(void) {
    log_init();
    hle_init();
    guest_tcb *tcb = tls_create(NULL);
    tls_set_current(tcb);

#define LOOK(var, name) *(void **)&var = hle_lookup("libSceFiber", name)
    LOOK(fInit, "_sceFiberInitializeImpl");
    LOOK(fOpt, "sceFiberOptParamInitialize");
    LOOK(fFinalize, "sceFiberFinalize");
    LOOK(fRun, "sceFiberRun");
    LOOK(fSwitch, "sceFiberSwitch");
    LOOK(fSelf, "sceFiberGetSelf");
    LOOK(fReturn, "sceFiberReturnToThread");
    LOOK(fInfo, "sceFiberGetInfo");
    LOOK(fRename, "sceFiberRename");
    if (!fInit || !fOpt || !fFinalize || !fRun || !fSwitch || !fSelf || !fReturn || !fInfo ||
        !fRename) {
        printf("libSceFiber functions not registered\n");
        return 1;
    }

    test_ping_pong();
    test_contextless();
    test_resume();
    test_errors();
    CHECK(tcb->fiber == NULL);

    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("fiber tests passed\n");
    return 0;
}
