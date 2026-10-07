/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Fault handling: the SysV red zone across a handled fault, the cost of a
 * handled fault, and the per-thread emergency stack.
 *
 * 1. Red zone. A SysV leaf function (asm) fills the 128 bytes below rsp, then
 *    writes to a write-protected page; the handler unprotects and retries.
 *    Windows 10 builds the exception frame below rsp: with a clean AVX state
 *    the red zone survives, but with live upper-ymm state the XSAVE area
 *    reaches rsp-0x10 and destroys it (checked as the known platform
 *    behaviour); Linux always skips it. The same store wrapped in
 *    `lea rsp,[rsp-128]` ... `lea rsp,[rsp+128]` (the trampoline fix, see
 *    docs/m3/s.md) keeps the red zone intact everywhere.
 * 2. Cost of a handled write-protection fault (what GPU page tracking pays),
 *    of the rsp adjustment, and of the emergency-stack switch.
 * 3. Emergency stack: a fault taken with only 4 KiB left on a 16 KiB stack
 *    that has a no-access page below it, handled by a handler with a 64 KiB
 *    frame. Without the emergency stack the process dies silently.
 * 4. Another thread gets its emergency stack lazily and can free it.
 * 5. Thread exit: scePthreadExit from inside a fiber returns to the thread,
 *    the join gets the value, and a second join fails cleanly. */
#include <stdio.h>
#include <string.h>

#include "hle/hle.h"
#include "log.h"
#include "platform.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sys/mman.h>
#endif

static int g_failures;
#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                         \
        }                                                                         \
    } while (0)

#ifdef _WIN32
#define FN_DEF(n) ".def " n "; .scl 2; .type 32; .endef\n"
#define TEB_SAVE "pushq %gs:0x8\n pushq %gs:0x10\n movq %rdx, %gs:0x8\n movq %rcx, %gs:0x10\n"
#define TEB_RESTORE "popq %gs:0x10\n popq %gs:0x8\n"
#else
#define FN_DEF(n) ".type " n ", @function\n"
#define TEB_SAVE ""
#define TEB_RESTORE ""
#endif

/* rz_probe(page, mode): fill [rsp-128, rsp) with a pattern, store 1 to *page
 * and return how many of the 16 red-zone qwords still hold the pattern.
 * mode bit 0: lower rsp by 128 around the store (the trampoline fix);
 * mode bit 1: dirty the upper halves of the ymm registers first (AVX state
 * in use, as in AVX game code; needs AVX).
 * run_on_stack(fn, arg, top, limit): call fn(arg, 0) with rsp = top; on
 * Windows the TEB stack bounds become [limit, top) meanwhile, as fiber.c does. */
enum { RZ_WRAP = 1, RZ_AVX = 2 };
SYSV int rz_probe(volatile uint64_t *page, int mode);
SYSV int run_on_stack(SYSV int (*fn)(volatile uint64_t *, int), volatile uint64_t *arg,
                      void *top, void *limit);
__asm__(".att_syntax prefix\n"
        ".text\n"
        ".globl rz_probe\n" FN_DEF("rz_probe")
        ".p2align 4\n"
        "rz_probe:\n"
        "movabsq $0x5a5a5a5a5a5a5a00, %rax\n"
        "xorl %ecx, %ecx\n"
        /* Named labels: clang-cl 18 crashes on numeric backward references. */
        ".Lrz_fill: leaq (%rax,%rcx), %rdx\n"
        "movq %rdx, -128(%rsp,%rcx,8)\n"
        "incl %ecx\n"
        "cmpl $16, %ecx\n"
        "jb .Lrz_fill\n"
        "testl $2, %esi\n"
        "jz .Lrz_store\n"
        "vxorps %ymm1, %ymm1, %ymm1\n"
        "vcmpeqps %ymm1, %ymm1, %ymm1\n" /* all ones, upper half included */
        ".Lrz_store: testl $1, %esi\n"
        "jnz .Lrz_wrap\n"
        "movq $1, (%rdi)\n"
        "jmp .Lrz_check\n"
        ".Lrz_wrap: leaq -128(%rsp), %rsp\n"
        "movq $1, (%rdi)\n"
        "leaq 128(%rsp), %rsp\n"
        ".Lrz_check: testl $2, %esi\n"
        "jz .Lrz_count\n"
        "vzeroupper\n"
        ".Lrz_count: xorl %r8d, %r8d\n"
        "xorl %ecx, %ecx\n"
        ".Lrz_loop: leaq (%rax,%rcx), %rdx\n"
        "cmpq %rdx, -128(%rsp,%rcx,8)\n"
        "jne .Lrz_next\n"
        "incl %r8d\n"
        ".Lrz_next: incl %ecx\n"
        "cmpl $16, %ecx\n"
        "jb .Lrz_loop\n"
        "movl %r8d, %eax\n"
        "ret\n"

        ".globl run_on_stack\n" FN_DEF("run_on_stack")
        ".p2align 4\n"
        "run_on_stack:\n"
        "pushq %rbp\n"
        "pushq %rbx\n"
        TEB_SAVE
        "movq %rsp, %rbx\n"
        "movq %rdx, %rsp\n"
        "movq %rdi, %rax\n"
        "movq %rsi, %rdi\n"
        "xorl %esi, %esi\n"
        "callq *%rax\n"
        "movq %rbx, %rsp\n"
        TEB_RESTORE
        "popq %rbx\n"
        "popq %rbp\n"
        "ret\n");

static uint8_t *g_page;
static int g_faults;
static bool g_heavy;
static bool g_on_estack;
static int64_t g_ctx_gap; /* faulting rsp minus the address of the saved xmm0 */

static bool handler(plat_fault *f) {
    if (f->kind != PLAT_FAULT_ACCESS || f->addr < (uint64_t)(uintptr_t)g_page ||
        f->addr >= (uint64_t)(uintptr_t)g_page + 0x1000) {
        fprintf(stderr, "unexpected fault kind %d at rip %#llx addr %#llx rsp %p\n", f->kind,
                (unsigned long long)f->rip, (unsigned long long)f->addr, (void *)f->rsp);
        return false;
    }
    g_faults++;
    g_on_estack = plat_fault_on_emergency_stack();
    /* Where the OS put the saved context, relative to the faulting rsp. */
    g_ctx_gap = (int64_t)((uint8_t *)f->rsp - (uint8_t *)f->xmm);
    if (g_heavy) {
        /* What a crash report or a page-tracking handler might use. */
        char big[64 * 1024];
        snprintf(big, sizeof(big), "fault at %#llx rip %#llx", (unsigned long long)f->addr,
                 (unsigned long long)f->rip);
        big[sizeof(big) - 1] = big[0];
        __asm__ volatile("" : : "r"(big) : "memory"); /* keep the whole frame */
    }
    plat_protect(g_page, 0x1000, PLAT_PROT_R | PLAT_PROT_W);
    return true;
}

static bool cpu_has_avx(void) {
    unsigned a, b, c, d;
    __asm__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!(c & (1u << 27)) || !(c & (1u << 28))) return false; /* OSXSAVE, AVX */
    unsigned lo, hi;
    __asm__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (lo & 6) == 6;
}

static int fault_probe(int mode) {
    int before = g_faults;
    plat_protect(g_page, 0x1000, PLAT_PROT_R);
    int intact = rz_probe((volatile uint64_t *)g_page, mode);
    CHECK(g_faults == before + 1);
    return intact;
}

static void test_red_zone(void) {
    int bare = fault_probe(0), wrapped = fault_probe(RZ_WRAP);
    printf("red zone (16 qwords) after a handled fault: bare store %d intact, rsp lowered by "
           "128 %d; saved xmm0 at rsp-%#llx\n", bare, wrapped, (long long)g_ctx_gap);
    CHECK(wrapped == 16);
    CHECK(g_on_estack);
    if (!cpu_has_avx()) {
        printf("no AVX: skipping the dirty-ymm case\n");
        CHECK(bare == 16);
        return;
    }
    int avx_bare = fault_probe(RZ_AVX), avx_wrapped = fault_probe(RZ_AVX | RZ_WRAP);
    printf("  with dirty upper ymm (AVX state in use): bare store %d, rsp lowered by 128 %d; "
           "saved xmm0 at rsp-%#llx\n", avx_bare, avx_wrapped, (long long)g_ctx_gap);
    CHECK(avx_wrapped == 16);
#ifdef _WIN32
    /* Windows 10 19045 (measured, docs/m3/s.md): the kernel leaves 0x128
     * bytes below rsp alone while the AVX state is in its init state, but
     * with live upper-ymm state its XSAVE area reaches up to rsp-0x10 and
     * wipes 14 of the 16 qwords. If these flip, Windows changed. */
    CHECK(bare == 16);
    CHECK(avx_bare < 16);
#else
    /* Linux places the signal frame below the red zone (or on the altstack). */
    CHECK(bare == 16);
    CHECK(avx_bare == 16);
#endif
}

static void test_fault_cost(void) {
    enum { N = 20000 };
    int before = g_faults;
    uint64_t t0 = plat_monotonic_ns();
    for (int i = 0; i < N; i++) {
        plat_protect(g_page, 0x1000, PLAT_PROT_R);
        rz_probe((volatile uint64_t *)g_page, RZ_WRAP);
    }
    uint64_t t1 = plat_monotonic_ns();
    for (int i = 0; i < N; i++) {
        plat_protect(g_page, 0x1000, PLAT_PROT_R);
        plat_protect(g_page, 0x1000, PLAT_PROT_R | PLAT_PROT_W);
        rz_probe((volatile uint64_t *)g_page, RZ_WRAP);
    }
    uint64_t t2 = plat_monotonic_ns();
    CHECK(g_faults - before == N);
    double with_fault = (double)(t1 - t0) / N, no_fault = (double)(t2 - t1) / N;
    /* What the trampoline's rsp adjustment adds to an access that does not fault. */
    enum { M = 5000000 };
    plat_protect(g_page, 0x1000, PLAT_PROT_R | PLAT_PROT_W);
    uint64_t t3 = plat_monotonic_ns();
    for (int i = 0; i < M; i++) rz_probe((volatile uint64_t *)g_page, 0);
    uint64_t t4 = plat_monotonic_ns();
    for (int i = 0; i < M; i++) rz_probe((volatile uint64_t *)g_page, RZ_WRAP);
    uint64_t t5 = plat_monotonic_ns();
    printf("store without a fault: %.2f ns bare, %.2f ns with rsp lowered around it\n",
           (double)(t4 - t3) / M, (double)(t5 - t4) / M);
    printf("handled write fault: %.2f us each (protect + fault + handler unprotect); "
           "the same without the fault: %.2f us; fault alone ~%.2f us\n",
           with_fault / 1000, no_fault / 1000, (with_fault - no_fault) / 1000);
}

#ifdef _WIN32
/* platform_win_fault.c's stack switch, timed on its own. */
bool call_on_stack(bool (*fn)(void *), void *arg, void *top, void *limit);
static __attribute__((noinline)) bool nop_fn(void *arg) { return arg != NULL; }

static void test_switch_cost(void) {
    enum { N = 1000000 };
    static _Alignas(16) uint8_t stack[64 * 1024];
    uint64_t t0 = plat_monotonic_ns();
    int ok = 0;
    for (int i = 0; i < N; i++) ok += call_on_stack(nop_fn, stack, stack + sizeof(stack), stack);
    uint64_t t1 = plat_monotonic_ns();
    CHECK(ok == N);
    printf("emergency-stack switch (incl. TEB bounds swap): %.1f ns per fault\n",
           (double)(t1 - t0) / N);
}
#endif

static void test_emergency_stack(void) {
    /* A 16 KiB stack (like the game's job fibers) above a no-access page. */
    size_t size = 0x5000;
#ifdef _WIN32
    uint8_t *block = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD old;
    VirtualProtect(block, 0x1000, PAGE_NOACCESS, &old);
#else
    uint8_t *block = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mprotect(block, 0x1000, PROT_NONE);
#endif
    uint8_t *limit = block + 0x1000;
    g_heavy = true;
    g_on_estack = false;
    int before = g_faults;
    plat_protect(g_page, 0x1000, PLAT_PROT_R);
    /* 4 KiB left: room for the kernel's exception frame and the dispatcher,
     * not for a 64 KiB handler frame. */
    int intact = run_on_stack(rz_probe, (volatile uint64_t *)g_page, limit + 0x1000, limit);
    g_heavy = false;
    CHECK(g_faults == before + 1);
    CHECK(g_on_estack);
    printf("fault with 4 KiB of a 16 KiB stack left, 64 KiB handler frame: handled "
           "(red zone %d/16)\n", intact);
#ifdef _WIN32
    VirtualFree(block, 0, MEM_RELEASE);
#else
    munmap(block, size);
#endif
}

static void thread_fn(void *arg) {
    (void)arg;
    /* No plat_fault_thread_init: the stack is allocated on the first fault. */
    plat_protect(g_page, 0x1000, PLAT_PROT_R);
    int intact = rz_probe((volatile uint64_t *)g_page, RZ_WRAP);
    CHECK(intact == 16);
    CHECK(g_on_estack);
    plat_fault_thread_exit();
    CHECK(plat_fault_thread_init());
    plat_fault_thread_exit();
}

/* ---- thread exit: pthread_exit from inside a fiber, join twice ----------- */

static SYSV int32_t (*pCreate)(void **, void **, SYSV void *(*)(void *), void *, const char *);
static SYSV int32_t (*pJoin)(void *, void **);
static SYSV void (*pExit)(void *);
static SYSV int32_t (*fInit)(void *, const char *, SYSV void (*)(uint64_t, uint64_t), uint64_t,
                             void *, uint64_t, const void *, uint32_t);
static SYSV int32_t (*fRun)(void *, uint64_t, uint64_t *);

static struct __attribute__((aligned(8))) { uint8_t b[256]; } g_fiber;
static _Alignas(16) uint8_t g_fiber_stack[16384];

static SYSV void fiber_exits(uint64_t init_arg, uint64_t run_arg) {
    (void)init_arg;
    pExit((void *)(uintptr_t)(0x1000 + run_arg));
}

static SYSV void *thread_with_fiber(void *arg) {
    (void)arg;
    CHECK(fInit(&g_fiber, "exits", fiber_exits, 0, g_fiber_stack, sizeof(g_fiber_stack), NULL,
                0) == 0);
    /* The red line above the stack signature. */
    CHECK(((uint64_t *)g_fiber_stack)[1] == 0xdeadbeefdeadbeefull);
    fRun(&g_fiber, 0x234, NULL);
    CHECK(!"sceFiberRun returned");
    return NULL;
}

static void test_thread_exit(void) {
    hle_init();
    hle_thread_init();
    *(void **)&pCreate = hle_lookup("libkernel", "scePthreadCreate");
    *(void **)&pJoin = hle_lookup("libkernel", "scePthreadJoin");
    *(void **)&pExit = hle_lookup("libkernel", "scePthreadExit");
    *(void **)&fInit = hle_lookup("libSceFiber", "_sceFiberInitializeImpl");
    *(void **)&fRun = hle_lookup("libSceFiber", "sceFiberRun");
    CHECK(pCreate && pJoin && pExit && fInit && fRun);
    if (!pCreate || !pJoin || !pExit || !fInit || !fRun) return;
    void *t = NULL, *ret = NULL;
    CHECK(pCreate(&t, NULL, thread_with_fiber, NULL, "fiber-exit") == 0);
    CHECK(pJoin(t, &ret) == 0);
    CHECK(ret == (void *)0x1234);
    CHECK(pJoin(t, &ret) != 0); /* already joined: an error, not a crash */
    printf("pthread_exit from a fiber: joined, value %p\n", ret);
}

int main(void) {
    log_init();
    plat_install_fault_handler(handler);
    g_page = plat_map(0, 0x1000, PLAT_PROT_R | PLAT_PROT_W, false);
    CHECK(g_page != NULL);
    if (!g_page) return 1;
    CHECK(!plat_fault_on_emergency_stack());

    test_red_zone();
    test_fault_cost();
#ifdef _WIN32
    test_switch_cost();
#endif
    test_emergency_stack();

    int before = g_faults;
    g_on_estack = false;
    plat_thread *t = plat_thread_create(thread_fn, NULL, 1 << 20);
    plat_thread_join(t);
    CHECK(g_faults == before + 1);

    test_thread_exit();

    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_fault: ok\n");
    return 0;
}
