/* SPDX-License-Identifier: GPL-2.0-or-later */
/* GPU bridge (workstream GI): backend init, the sotcgpu call sequence from
 * memory, VideoOut, GnmDriver and the PM4 queues, page tracking (protection
 * combined with the guest's, faults, backing writes, sparse reads) and the
 * callbacks (flip_done, eop_interrupt), against the recording null sotcgpu.
 *
 * The recording build is compiled into this test, so the sotcgpu_* symbols
 * resolve here and the linked library (null or Vulkan) is not used. */
#define SOTCGPU_NULL_RECORDING
#include "../../gpu/null/sotcgpu_null.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_bridge.h"
#include "hle/hle.h"
#include "hle/pm4.h"
#include "log.h"
#include "platform.h"

static int g_failures;
#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                         \
        }                                                                         \
    } while (0)

typedef SYSV uint64_t (*gfn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t, uint64_t);
static uint64_t call_(const char *lib, const char *name, const uint64_t *a) {
    gfn f = (gfn)hle_lookup(lib, name);
    if (!f) {
        fprintf(stderr, "FAIL missing %s\n", name);
        g_failures++;
        return 0;
    }
    return f(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]);
}
#define KERN(name, ...) ((int32_t)call_("libkernel", name, (const uint64_t[10]){__VA_ARGS__}))
#define VO(name, ...) ((int32_t)call_("libSceVideoOut", name, (const uint64_t[10]){__VA_ARGS__}))
#define GNM(name, ...) ((int32_t)call_("libSceGnmDriver", name, (const uint64_t[10]){__VA_ARGS__}))
#define P(x) ((uint64_t)(uintptr_t)(x))

uint64_t pm4_debug_irq_count(void);

#define FAKE_WINDOW ((void *)(uintptr_t)0x5d1ull)
static void *fake_window(void) { return FAKE_WINDOW; }

static bool on_fault(plat_fault *f) {
    return f->kind == PLAT_FAULT_ACCESS && gpu_bridge_on_fault(f->addr, f->is_write, f->rip);
}

static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

/* Index of the first record of `kind` at or after `from` (matching a if
 * match_a), or -1. */
static int find_rec(int from, sotcgpu_rec_kind kind, bool match_a, uint64_t a) {
    int n = sotcgpu_rec_state.count;
    for (int i = from; i < n && i < SOTCGPU_REC_MAX; i++)
        if (sotcgpu_rec_state.recs[i].kind == kind && (!match_a || sotcgpu_rec_state.recs[i].a == a))
            return i;
    return -1;
}

static bool wait_for(bool (*cond)(void *), void *ctx) {
    for (int i = 0; i < 2000; i++) {
        if (cond(ctx)) return true;
        plat_sleep_us(1000);
    }
    return cond(ctx);
}

static int host_prot_at(uint64_t a) { return plat_query_prot((void *)(uintptr_t)a, NULL); }

/* SceVideoOutFlipStatus (64 bytes). */
typedef struct flip_status {
    uint64_t count, process_time, tsc;
    int64_t flip_arg;
    uint64_t submit_tsc, reserved0;
    int32_t gc_queue_num, flip_pending_num, current_buffer;
    uint32_t reserved1;
} flip_status;

static int32_t g_vo;
static bool flips_at_least(void *n) {
    flip_status st;
    memset(&st, 0, sizeof st);
    VO("sceVideoOutGetFlipStatus", (uint64_t)g_vo, P(&st));
    return st.count >= (uint64_t)(uintptr_t)n;
}

static volatile int g_done_calls;
static void on_done(void *arg) {
    (void)arg;
    __atomic_add_fetch(&g_done_calls, 1, __ATOMIC_SEQ_CST);
}
static bool done_at_least(void *n) { return g_done_calls >= (int)(uintptr_t)n; }

static uint64_t g_irq_before;
static bool irq_raised(void *unused) {
    (void)unused;
    return pm4_debug_irq_count() > g_irq_before;
}

int main(void) {
    log_init();
    hle_init();
    hle_memory_init();
    plat_install_fault_handler(on_fault);

    /* A mapping made before the GPU starts is replayed to sotcgpu at init. */
    uint64_t pre = 0;
    CHECK(KERN("sceKernelMapFlexibleMemory", P(&pre), 0x10000, 3, 0) == SCE_OK);
    CHECK(!gpu_bridge_active());

    /* ---- init: SOTC_GPU=vulkan with a (fake) window ---- */
    set_env("SOTC_GPU", "vulkan");
    gpu_bridge_set_window_provider(fake_window);
    sotcgpu_rec_state.init_result = 0;
    gpu_bridge_init();
    CHECK(gpu_bridge_active());
    CHECK(sotcgpu_rec_state.count >= 1 && sotcgpu_rec_state.recs[0].kind == REC_INIT);
    CHECK(sotcgpu_rec_state.cfg.width == 1920 && sotcgpu_rec_state.cfg.height == 1080);
    CHECK(sotcgpu_rec_state.cfg.sdl_window == FAKE_WINDOW);
    CHECK(find_rec(1, REC_MAP, true, pre) > 0);
    const sotcgpu_host *host = sotcgpu_rec_state.host;
    CHECK(host != NULL);
    if (!host) return 1;
    gpu_bridge_init(); /* idempotent */
    CHECK(find_rec(1, REC_INIT, false, 0) < 0);

    /* ---- map notifications ---- */
    int64_t phys = 0;
    uint64_t d = 0;
    const uint64_t dsize = 0x40000;
    CHECK(KERN("sceKernelAllocateDirectMemory", 0, 0x100000000ull, dsize, 0x10000, 0, P(&phys)) ==
          SCE_OK);
    CHECK(KERN("sceKernelMapDirectMemory", P(&d), dsize, 0x33, 0, (uint64_t)phys, 0) == SCE_OK);
    int map_rec = find_rec(0, REC_MAP, true, d);
    CHECK(map_rec > 0 && sotcgpu_rec_state.recs[map_rec].b == dsize);
    CHECK(pre + 0x10000 == d); /* contiguous with `pre` (first gaps in the arena) */
    memset((void *)(uintptr_t)d, 0x11, dsize);

    /* ---- write protection + backing write (direct memory: backing view) ---- */
    CHECK(host->protect(d, 0x1000, true, false));
    CHECK(host_prot_at(d) == PLAT_PROT_R);
    CHECK(host->write_backing(d + 8, "ABCD", 4));
    CHECK(memcmp((void *)(uintptr_t)(d + 8), "ABCD", 4) == 0);
    CHECK(host_prot_at(d) == PLAT_PROT_R); /* still watched */

    /* A CPU write to the watched page faults into sotcgpu, which unprotects. */
    sotcgpu_rec_state.fault_unprotects = true;
    int before = sotcgpu_rec_state.count;
    *(volatile uint32_t *)(uintptr_t)(d + 16) = 0x1234;
    CHECK(*(volatile uint32_t *)(uintptr_t)(d + 16) == 0x1234);
    int fault_rec = find_rec(before, REC_FAULT, false, 0);
    CHECK(fault_rec >= 0 && sotcgpu_rec_state.recs[fault_rec].a == d + 16 &&
          sotcgpu_rec_state.recs[fault_rec].flag);
    CHECK(host_prot_at(d) == (PLAT_PROT_R | PLAT_PROT_W));

    /* ---- read protection, sparse reads, guest mprotect combined ---- */
    CHECK(host->protect(d + 0x1000, 0x1000, false, false));
    CHECK(host_prot_at(d + 0x1000) == PLAT_PROT_NONE);
    uint8_t buf[16];
    host->read_sparse(buf, d + 0x1000 - 4, 8);
    CHECK(buf[0] == 0x11 && buf[7] == 0x11);
    CHECK(host_prot_at(d + 0x1000) == PLAT_PROT_NONE);
    /* The guest makes the range read-only: the GPU's restriction survives. */
    CHECK(KERN("sceKernelMprotect", d, 0x4000, 1) == SCE_OK);
    CHECK(host_prot_at(d) == PLAT_PROT_R);
    CHECK(host_prot_at(d + 0x1000) == PLAT_PROT_NONE);
    /* The GPU lifts its restriction: the guest's read-only remains. */
    CHECK(host->protect(d + 0x1000, 0x1000, true, true));
    CHECK(host_prot_at(d + 0x1000) == PLAT_PROT_R);
    CHECK(KERN("sceKernelMprotect", d, 0x4000, 3) == SCE_OK);
    CHECK(host_prot_at(d + 0x1000) == (PLAT_PROT_R | PLAT_PROT_W));

    /* ---- private (flexible) memory: temporary unprotect ---- */
    memset((void *)(uintptr_t)pre, 0x22, 0x10000);
    CHECK(host->protect(pre, 0x2000, false, false));
    CHECK(host->write_backing(pre + 0xffc, "WXYZ", 4)); /* spans two pages */
    CHECK(host_prot_at(pre) == PLAT_PROT_NONE && host_prot_at(pre + 0x1000) == PLAT_PROT_NONE);
    host->read_sparse(buf, pre + 0xffc, 6);
    CHECK(memcmp(buf, "WXYZ\x22\x22", 6) == 0);
    CHECK(host->protect(pre, 0x2000, true, true));
    CHECK(memcmp((void *)(uintptr_t)(pre + 0xffc), "WXYZ", 4) == 0);

    /* ---- holes, clamp_range, is_gpu_mapping ---- */
    host->read_sparse(buf, d + dsize - 8, 16);
    CHECK(buf[0] == 0x11 && buf[7] == 0x11 && buf[8] == 0 && buf[15] == 0);
    CHECK(!host->write_backing(d + dsize - 4, "12345678", 8));
    CHECK(host->clamp_range(d, 0x100000) == dsize);
    CHECK(host->clamp_range(pre, 0x100000) == 0x10000 + dsize);
    /* unbounded descriptors (>= 1 GiB) stop at the end of their own mapping */
    CHECK(host->clamp_range(pre, 0xfffffff0ull) == 0x10000);
    CHECK(host->clamp_range(d + dsize, 0x1000) == 0);
    CHECK(host->is_gpu_mapping(d, dsize));
    CHECK(!host->is_gpu_mapping(d, dsize + 1));
    CHECK(!host->is_gpu_mapping(PLAT_GUEST_LIMIT - 0x1000, 0x2000));

    /* ---- unmap: notification, and a split keeps the GPU's restriction ---- */
    CHECK(host->protect(d + 0x20000, 0x1000, true, false));
    before = sotcgpu_rec_state.count;
    CHECK(KERN("sceKernelMunmap", d + 0x10000, 0x10000) == SCE_OK);
    int unmap_rec = find_rec(before, REC_UNMAP, true, d + 0x10000);
    CHECK(unmap_rec >= 0 && sotcgpu_rec_state.recs[unmap_rec].b == 0x10000);
    CHECK(host_prot_at(d + 0x20000) == PLAT_PROT_R);
    CHECK(host_prot_at(d + 0x21000) == (PLAT_PROT_R | PLAT_PROT_W));
    CHECK(host_prot_at(d + 0x10000) < 0);
    CHECK(host->clamp_range(d, dsize) == 0x10000);
    CHECK(host->protect(d + 0x20000, 0x1000, true, true));

    /* ---- VideoOut: register buffers, flip, flip_done ---- */
    static uint8_t attr[40];
    static uint64_t bufs[2];
    bufs[0] = d;
    bufs[1] = d + 0x30000;
    g_vo = VO("sceVideoOutOpen", 255, 0, 0, 0);
    CHECK(g_vo > 0);
    VO("sceVideoOutSetBufferAttribute", P(attr), 0x80000000u, 1, 0, 1920, 1080, 1920);
    before = sotcgpu_rec_state.count;
    CHECK(VO("sceVideoOutRegisterBuffers", (uint64_t)g_vo, 0, P(bufs), 2, P(attr)) == 0);
    int reg_rec = find_rec(before, REC_REGISTER_BUFFERS, false, 0);
    CHECK(reg_rec >= 0 && sotcgpu_rec_state.recs[reg_rec].a == 0 &&
          sotcgpu_rec_state.recs[reg_rec].b == 2);
    CHECK(sotcgpu_rec_state.buffers[0] == d && sotcgpu_rec_state.buffers[1] == d + 0x30000);

    CHECK(VO("sceVideoOutSubmitFlip", (uint64_t)g_vo, 1, 1, 77) == SCE_OK);
    int flip_rec = find_rec(reg_rec, REC_FLIP, false, 0);
    CHECK(flip_rec > reg_rec && sotcgpu_rec_state.recs[flip_rec].a == 1 &&
          sotcgpu_rec_state.recs[flip_rec].b == 77 && !sotcgpu_rec_state.recs[flip_rec].flag);
    CHECK(wait_for(flips_at_least, (void *)1));
    flip_status st;
    VO("sceVideoOutGetFlipStatus", (uint64_t)g_vo, P(&st));
    CHECK(st.count == 1 && st.flip_arg == 77 && st.current_buffer == 1 &&
          st.flip_pending_num == 0 && st.gc_queue_num == 0);

    /* ---- PM4 queues forward to sotcgpu; `done` runs on the event thread ---- */
    static uint32_t cmd[64];
    before = sotcgpu_rec_state.count;
    pm4_submit(PM4_QUEUE_GFX, cmd, 4, NULL, 0, on_done, NULL);
    pm4_submit(5, cmd, 8, NULL, 0, on_done, NULL);
    int gfx_rec = find_rec(before, REC_SUBMIT_GFX, false, 0);
    int cmp_rec = find_rec(before, REC_SUBMIT_COMPUTE, true, 5);
    CHECK(gfx_rec >= 0 && sotcgpu_rec_state.recs[gfx_rec].a == P(cmd) &&
          sotcgpu_rec_state.recs[gfx_rec].b == 4);
    CHECK(cmp_rec > gfx_rec && sotcgpu_rec_state.recs[cmp_rec].c == 8);
    CHECK(wait_for(done_at_least, (void *)2));

    /* ---- GnmDriver SubmitAndFlip: the EOP flip is armed, then the buffers ---- */
    memset(cmd, 0, sizeof cmd);
    cmd[0] = 0xc03e1000u;  /* 64-dword NOP */
    cmd[1] = 0x68750777u;  /* prepareFlip */
    static void *dcbs[1];
    static uint32_t sizes[1] = {sizeof cmd};
    dcbs[0] = cmd;
    before = sotcgpu_rec_state.count;
    CHECK(GNM("sceGnmSubmitAndFlipCommandBuffers", 1, P(dcbs), P(sizes), 0, 0, (uint64_t)g_vo, 0,
              0, 99) == SCE_OK);
    gfx_rec = find_rec(before, REC_SUBMIT_GFX, false, 0);
    flip_rec = find_rec(before, REC_FLIP, false, 0);
    CHECK(flip_rec >= 0 && gfx_rec > flip_rec);
    CHECK(flip_rec >= 0 && sotcgpu_rec_state.recs[flip_rec].a == 0 &&
          sotcgpu_rec_state.recs[flip_rec].b == 99 && sotcgpu_rec_state.recs[flip_rec].flag);
    CHECK(wait_for(flips_at_least, (void *)2));
    VO("sceVideoOutGetFlipStatus", (uint64_t)g_vo, P(&st));
    CHECK(st.count == 2 && st.flip_arg == 99 && st.current_buffer == 0 && st.gc_queue_num == 0 &&
          st.flip_pending_num == 0);
    before = sotcgpu_rec_state.count;
    CHECK(GNM("sceGnmSubmitDone", 0) == SCE_OK);
    CHECK(find_rec(before, REC_SUBMIT_DONE, false, 0) >= 0);

    /* ---- eop_interrupt reaches the GnmDriver EOP path ---- */
    g_irq_before = pm4_debug_irq_count();
    host->eop_interrupt();
    CHECK(wait_for(irq_raised, NULL));

    CHECK(VO("sceVideoOutClose", (uint64_t)g_vo) == SCE_OK);
    CHECK(sotcgpu_rec_state.count < SOTCGPU_REC_MAX);

    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("gpu bridge tests passed (%d sotcgpu calls recorded)\n", sotcgpu_rec_state.count);
    return 0;
}
