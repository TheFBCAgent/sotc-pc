/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Null PM4 executor side effects and the VideoOut flip/vblank machinery. */
#include <stdio.h>
#include <string.h>

#include "hle/hle.h"
#include "hle/pm4.h"

/* Test hook in pm4.c, deliberately not in the shared pm4.h. */
uint64_t pm4_debug_irq_count(void);

static int g_failures;
#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                         \
        }                                                                         \
    } while (0)

#define PKT3(op, body_dwords) (0xC0000000u | ((uint32_t)((body_dwords) - 1) << 16) | ((op) << 8))
#define LO(p) ((uint32_t)(uintptr_t)(p))
#define HI(p) ((uint32_t)((uint64_t)(uintptr_t)(p) >> 32))

/* Guest-visible memory (GPU addresses must be below 1 TiB). */
static uint8_t *g_mem;
#define MEM_SIZE 0x10000

static void test_pm4(void) {
    uint32_t *cb = (uint32_t *)g_mem;              /* main command buffer */
    uint32_t *ib = (uint32_t *)(g_mem + 0x2000);   /* indirect buffer */
    uint8_t *lbl = g_mem + 0x8000;                 /* labels */
    uint32_t *l32 = (uint32_t *)lbl;
    uint64_t *l64 = (uint64_t *)lbl;
    uint32_t n = 0;

    /* A registered EOP equeue: a real one, since equeue.c rejects unknown handles. */
    typedef SYSV int32_t (*create_eq_fn)(equeue **, const char *);
    create_eq_fn create_eq = (create_eq_fn)hle_lookup("libkernel", "sceKernelCreateEqueue");
    equeue *eq = NULL;
    CHECK(create_eq && create_eq(&eq, "pm4") == SCE_OK && eq);
    CHECK(pm4_add_eop_equeue(eq, 0x40, NULL));
    uint64_t irq0 = pm4_debug_irq_count();

    cb[n++] = 0x80000000u; /* type-2 filler */
    cb[n++] = 0xFFFF1000u; /* one-dword NOP */
    /* WRITE_DATA two dwords to l32[0..1], dst_sel 5 (memory). */
    cb[n++] = PKT3(0x37, 5);
    cb[n++] = 5u << 8;
    cb[n++] = LO(&l32[0]);
    cb[n++] = HI(&l32[0]);
    cb[n++] = 0x11111111u;
    cb[n++] = 0x22222222u;
    /* EVENT_WRITE_EOP: 32-bit data to l32[2], interrupt on write confirm. */
    cb[n++] = PKT3(0x47, 5);
    cb[n++] = 0x504;
    cb[n++] = LO(&l32[2]);
    cb[n++] = (HI(&l32[2]) & 0xffff) | (2u << 24) | (1u << 29);
    cb[n++] = 0xEEEE0001u;
    cb[n++] = 0xDEADBEEFu;
    /* EVENT_WRITE_EOP: 64-bit data to l64[2], no interrupt. */
    cb[n++] = PKT3(0x47, 5);
    cb[n++] = 0x504;
    cb[n++] = LO(&l64[2]);
    cb[n++] = (HI(&l64[2]) & 0xffff) | (2u << 29);
    cb[n++] = 0x89ABCDEFu;
    cb[n++] = 0x01234567u;
    /* RELEASE_MEM: 64-bit data to l64[3], interrupt. */
    cb[n++] = PKT3(0x49, 6);
    cb[n++] = 0x514;
    cb[n++] = (2u << 24) | (2u << 29);
    cb[n++] = LO(&l64[3]);
    cb[n++] = HI(&l64[3]);
    cb[n++] = 0xAAAA5555u;
    cb[n++] = 0x0000BBBBu;
    /* EVENT_WRITE_EOS: signal fence to l32[8]. */
    cb[n++] = PKT3(0x48, 4);
    cb[n++] = 0x515;
    cb[n++] = LO(&l32[8]);
    cb[n++] = (HI(&l32[8]) & 0xffff) | (2u << 29);
    cb[n++] = 0x0E05u;
    /* DMA_DATA: fill 16 bytes at l32[12..15] with a dword. */
    cb[n++] = PKT3(0x50, 6);
    cb[n++] = (2u << 29) | (0u << 20);
    cb[n++] = 0x5A5A5A5Au;
    cb[n++] = 0;
    cb[n++] = LO(&l32[12]);
    cb[n++] = HI(&l32[12]);
    cb[n++] = 16;
    /* DMA_DATA: copy l32[0..1] to l32[16..17]. */
    cb[n++] = PKT3(0x50, 6);
    cb[n++] = (0u << 29) | (0u << 20);
    cb[n++] = LO(&l32[0]);
    cb[n++] = HI(&l32[0]);
    cb[n++] = LO(&l32[16]);
    cb[n++] = HI(&l32[16]);
    cb[n++] = 8;
    /* WAIT_REG_MEM on l32[2] == 0xEEEE0001 (already true). */
    cb[n++] = PKT3(0x3C, 6);
    cb[n++] = 3u | (1u << 4);
    cb[n++] = LO(&l32[2]);
    cb[n++] = HI(&l32[2]);
    cb[n++] = 0xEEEE0001u;
    cb[n++] = 0xFFFFFFFFu;
    cb[n++] = 10;
    /* MEM_SEMAPHORE signal (increment) on l64[14]. */
    cb[n++] = PKT3(0x39, 2);
    cb[n++] = LO(&l64[14]);
    cb[n++] = (HI(&l64[14]) & 0xff) | (6u << 29);
    /* WRITE_DATA to address 0: skipped, no crash. */
    cb[n++] = PKT3(0x37, 4);
    cb[n++] = 5u << 8;
    cb[n++] = 0;
    cb[n++] = 0;
    cb[n++] = 0x99999999u;
    /* Unpatched PrepareFlipInterruptLabel NOP: label l32[24] and an interrupt. */
    cb[n++] = PKT3(0x10, 6);
    cb[n++] = 0x68750781u;
    cb[n++] = LO(&l32[24]);
    cb[n++] = HI(&l32[24]);
    cb[n++] = 0xF11Fu;
    cb[n++] = 0x04;
    cb[n++] = 0x05;
    /* A draw (ignored). */
    cb[n++] = PKT3(0x2D, 2);
    cb[n++] = 3;
    cb[n++] = 2;
    /* INDIRECT_BUFFER to `ib`. */
    uint32_t m = 0;
    ib[m++] = PKT3(0x37, 4);
    ib[m++] = 5u << 8;
    ib[m++] = LO(&l32[20]);
    ib[m++] = HI(&l32[20]);
    ib[m++] = 0x1B1B1B1Bu;
    cb[n++] = PKT3(0x3F, 3);
    cb[n++] = LO(ib);
    cb[n++] = HI(ib) & 0xffff;
    cb[n++] = m;

    pm4_execute(cb, n, NULL, 0);

    CHECK(l32[0] == 0x11111111u && l32[1] == 0x22222222u);
    CHECK(l32[2] == 0xEEEE0001u);
    CHECK(l32[3] == 0); /* 32-bit EOP leaves the next dword alone */
    CHECK(l64[2] == 0x0123456789ABCDEFull);
    CHECK(l64[3] == 0x0000BBBBAAAA5555ull);
    CHECK(l32[8] == 0x0E05u);
    CHECK(l32[12] == 0x5A5A5A5Au && l32[15] == 0x5A5A5A5Au && l32[11] == 0 && l32[16] != 0x5A5A5A5Au);
    CHECK(l32[16] == 0x11111111u && l32[17] == 0x22222222u);
    CHECK(l64[14] == 1);
    CHECK(l32[24] == 0xF11Fu);
    CHECK(l32[20] == 0x1B1B1B1Bu);
    CHECK(pm4_debug_irq_count() - irq0 == 3); /* EOP, RELEASE_MEM, flip NOP */

    /* A packet overrunning the buffer stops the walk: the next write is skipped. */
    memset(lbl, 0, 0x100);
    n = 0;
    cb[n++] = PKT3(0x37, 4);
    cb[n++] = 5u << 8;
    cb[n++] = LO(&l32[0]);
    cb[n++] = HI(&l32[0]);
    cb[n++] = 0x77u;
    cb[n++] = PKT3(0x37, 200); /* claims 200 body dwords */
    cb[n++] = 5u << 8;
    pm4_execute(cb, n, NULL, 0);
    CHECK(l32[0] == 0x77u);

    /* A type-1 header is malformed: nothing after it runs. */
    n = 0;
    cb[n++] = 0x40000000u;
    cb[n++] = PKT3(0x37, 4);
    cb[n++] = 5u << 8;
    cb[n++] = LO(&l32[1]);
    cb[n++] = HI(&l32[1]);
    cb[n++] = 0x88u;
    pm4_execute(cb, n, NULL, 0);
    CHECK(l32[1] == 0);

    /* NULL / empty buffers are fine. */
    pm4_execute(NULL, 0, NULL, 0);

    CHECK(pm4_remove_eop_equeue(eq, 0x40));
    CHECK(!pm4_remove_eop_equeue(eq, 0x40));
}

/* The async queue blocks in WAIT_REG_MEM until the CPU writes the label after
 * submitting, then runs the rest of the buffer and the done callback. */
static volatile int g_done_calls;
static void on_done(void *arg) { (void)arg; g_done_calls++; }

static void test_async(void) {
    uint32_t *cb = (uint32_t *)(g_mem + 0xA000);
    volatile uint32_t *gate = (volatile uint32_t *)(g_mem + 0xB000);
    volatile uint32_t *out = gate + 1;
    *gate = 0;
    *out = 0;
    uint32_t n = 0;
    /* WAIT_REG_MEM memory space, function 3 (==), ref 1, mask ~0. */
    cb[n++] = PKT3(0x3C, 6);
    cb[n++] = (1u << 4) | 3u;
    cb[n++] = LO(gate);
    cb[n++] = HI(gate);
    cb[n++] = 1;
    cb[n++] = 0xffffffffu;
    cb[n++] = 4;
    cb[n++] = PKT3(0x37, 4);
    cb[n++] = 5u << 8;
    cb[n++] = LO(out);
    cb[n++] = HI(out);
    cb[n++] = 0x77u;
    g_done_calls = 0;
    pm4_submit(PM4_QUEUE_GFX, cb, n, NULL, 0, on_done, NULL);
    plat_sleep_us(200000); /* longer than the synchronous timeout: must still be waiting */
    CHECK(*out == 0 && g_done_calls == 0);
    *gate = 1;
    pm4_wait_idle();
    CHECK(*out == 0x77u && g_done_calls == 1);
}

/* ---- VideoOut ----------------------------------------------------------- */

typedef struct {
    uint64_t count, process_time, tsc;
    int64_t flip_arg;
    uint64_t submit_tsc, reserved0;
    int32_t gc_queue_num, flip_pending_num, current_buffer;
    uint32_t reserved1;
} flip_status;

typedef struct {
    uint64_t count, process_time, tsc, reserved;
    uint8_t flags, pad[7];
} vblank_status;

typedef struct {
    int32_t full_width, full_height, pane_width, pane_height;
    uint64_t refresh_rate;
    float screen_size_in_inch;
    uint16_t flags, reserved0;
    uint32_t reserved1[3];
} resolution_status;

#define VO(name) hle_lookup("libSceVideoOut", name)

static void test_videoout(void) {
    SYSV int32_t (*open_)(int32_t, int32_t, int32_t, const void *) = VO("sceVideoOutOpen");
    SYSV int32_t (*close_)(int32_t) = VO("sceVideoOutClose");
    SYSV void (*set_attr)(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) =
        VO("sceVideoOutSetBufferAttribute");
    SYSV int32_t (*reg)(int32_t, int32_t, void *const *, int32_t, const void *) =
        VO("sceVideoOutRegisterBuffers");
    SYSV int32_t (*flip)(int32_t, int32_t, int32_t, int64_t) = VO("sceVideoOutSubmitFlip");
    SYSV int32_t (*pending)(int32_t) = VO("sceVideoOutIsFlipPending");
    SYSV int32_t (*get_flip)(int32_t, flip_status *) = VO("sceVideoOutGetFlipStatus");
    SYSV int32_t (*get_vblank)(int32_t, vblank_status *) = VO("sceVideoOutGetVblankStatus");
    SYSV int32_t (*get_res)(int32_t, resolution_status *) = VO("sceVideoOutGetResolutionStatus");
    SYSV int32_t (*wait_vblank)(int32_t) = VO("sceVideoOutWaitVblank");
    SYSV int32_t (*add_flip)(equeue *, int32_t, void *) = VO("sceVideoOutAddFlipEvent");
    SYSV int32_t (*event_id)(const ps4_kevent *) = VO("sceVideoOutGetEventId");
    SYSV int32_t (*event_data)(const ps4_kevent *, int64_t *) = VO("sceVideoOutGetEventData");
    SYSV int32_t (*set_rate)(int32_t, int32_t) = VO("sceVideoOutSetFlipRate");

    CHECK(open_ && close_ && set_attr && reg && flip && pending && get_flip && get_vblank &&
          get_res && wait_vblank && add_flip && event_id && event_data && set_rate);
    CHECK(VO("sceVideoOutAdjustColor_") && VO("sceVideoOutModeSetAny_") &&
          VO("sceVideoOutConfigureOutputMode_") && VO("sceVideoOutGetDeviceCapabilityInfo_") &&
          VO("sceVideoOutColorSettingsSetGamma_") && VO("sceVideoOutSetWindowModeMargins") &&
          VO("sceVideoOutSubmitChangeBufferAttribute") && VO("sceVideoOutUnregisterBuffers") &&
          VO("sceVideoOutDeleteFlipEvent"));
    if (g_failures) return;

    int32_t h = open_(255, 0, 0, NULL);
    CHECK(h > 0);
    CHECK(open_(255, 0, 0, NULL) == (int32_t)0x80290009); /* busy */

    resolution_status res;
    CHECK(get_res(h, &res) == 0 && res.full_width == 1920 && res.full_height == 1080);

    uint8_t attr[40];
    set_attr(attr, 0x80000000u, 0, 0, 1920, 1080, 1920);
    void *bufs[2] = {g_mem + 0x4000, g_mem + 0x6000};
    CHECK(reg(h, 0, bufs, 2, attr) == 0); /* group 0 */
    CHECK(reg(h, 1, bufs, 1, attr) == (int32_t)0x80290010); /* slot occupied */
    typedef SYSV int32_t (*create_eq_fn)(equeue **, const char *);
    typedef SYSV int32_t (*wait_eq_fn)(equeue *, ps4_kevent *, int32_t, int32_t *, uint32_t *);
    create_eq_fn create_eq = (create_eq_fn)hle_lookup("libkernel", "sceKernelCreateEqueue");
    wait_eq_fn wait_eq = (wait_eq_fn)hle_lookup("libkernel", "sceKernelWaitEqueue");
    equeue *feq = NULL;
    CHECK(create_eq && wait_eq && create_eq(&feq, "flip") == SCE_OK);
    CHECK(add_flip(feq, h, NULL) == 0);

    CHECK(flip(h, 5, 1, 0) == (int32_t)0x8029000A); /* unregistered buffer */
    CHECK(flip(h, 0, 1, 42) == 0);
    CHECK(pending(h) == 1);
    for (int i = 0; i < 4 && pending(h) > 0; i++) wait_vblank(h);
    flip_status fs;
    CHECK(get_flip(h, &fs) == 0);
    CHECK(fs.count == 1 && fs.flip_arg == 42 && fs.current_buffer == 0 &&
          fs.flip_pending_num == 0);
    /* The flip event reached the real equeue with flip_arg in the data. */
    ps4_kevent fev;
    int32_t got = 0;
    uint32_t tmo = 100000;
    CHECK(wait_eq(feq, &fev, 1, &got, &tmo) == SCE_OK && got == 1);
    CHECK(fev.filter == EVFILT_VIDEO_OUT && event_id(&fev) == 0);
    int64_t farg = 0;
    CHECK(event_data(&fev, &farg) == 0 && farg == 42);

    /* The GnmDriver path (EOP flip). */
    CHECK(videoout_submit_flip(h, 1, 1, -7) == 0);
    CHECK(get_flip(h, &fs) == 0 && fs.gc_queue_num == 1 && fs.flip_pending_num == 1);
    for (int i = 0; i < 4 && pending(h) > 0; i++) wait_vblank(h);
    CHECK(get_flip(h, &fs) == 0 && fs.count == 2 && fs.flip_arg == -7 &&
          fs.current_buffer == 1 && fs.gc_queue_num == 0);

    /* Flip rate 1 (30 Hz): at most one flip every second vblank. */
    CHECK(set_rate(h, 1) == 0);
    CHECK(set_rate(h, 3) == (int32_t)0x80290001);
    vblank_status vs0, vs1;
    get_vblank(h, &vs0);
    for (int i = 0; i < 4; i++) CHECK(flip(h, i & 1, 1, i) == 0);
    for (int i = 0; i < 40 && pending(h) > 0; i++) wait_vblank(h);
    get_vblank(h, &vs1);
    CHECK(pending(h) == 0);
    CHECK(vs1.count - vs0.count >= 7);
    CHECK(set_rate(h, 0) == 0);

    /* Event accessors. */
    ps4_kevent ev = {.ident = 6, .filter = EVFILT_VIDEO_OUT, .data = 6 | ((uint64_t)-7 << 16)};
    int64_t d = 0;
    CHECK(event_id(&ev) == 0);
    CHECK(event_data(&ev, &d) == 0 && d == -7);
    ev.ident = 7;
    ev.data = 7 | (123ull << 16);
    CHECK(event_id(&ev) == 1);
    CHECK(event_data(&ev, &d) == 0 && d == 123);
    ev.filter = EVFILT_USER;
    CHECK(event_id(&ev) == (int32_t)0x8029000D);

    CHECK(close_(h) == 0);
    CHECK(get_flip(h, &fs) == (int32_t)0x8029000B);
    /* Re-open works after close. */
    int32_t h2 = open_(255, 0, 0, NULL);
    CHECK(h2 > 0);
    CHECK(get_flip(h2, &fs) == 0 && fs.count == 0 && fs.current_buffer == -1);
    CHECK(close_(h2) == 0);
}

int main(void) {
    log_init();
    hle_init();
    g_mem = plat_map(0x3000000000ull, MEM_SIZE, PLAT_PROT_R | PLAT_PROT_W, false);
    CHECK(g_mem != NULL && (uint64_t)(uintptr_t)g_mem < PLAT_GUEST_LIMIT);
    if (!g_mem) return 1;

    test_pm4();
    test_async();
    test_videoout();

    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("pm4/videoout tests passed\n");
    return 0;
}
