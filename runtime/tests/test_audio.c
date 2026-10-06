/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Null audio: AudioOut pacing, Audio3d queue, Ngs2 handles and silent render. */
#include <stdio.h>
#include <stdlib.h>
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

#define FN(lib, name, type) type name = (type)hle_lookup(lib, #name); CHECK(name != NULL)

typedef SYSV int32_t (*fn_v)(void);
typedef SYSV int32_t (*fn_i)(int32_t);
typedef SYSV int32_t (*fn_open)(int32_t, int32_t, int32_t, uint32_t, uint32_t, uint32_t);
typedef SYSV int32_t (*fn_ip)(int32_t, const void *);
typedef SYSV int32_t (*fn_pu)(const void *, uint32_t);
typedef SYSV int32_t (*fn_ii)(int32_t, int32_t);

static void test_audioout(void) {
    FN("libSceAudioOut", sceAudioOutInit, fn_v);
    FN("libSceAudioOut", sceAudioOutOpen, fn_open);
    FN("libSceAudioOut", sceAudioOutClose, fn_i);
    FN("libSceAudioOut", sceAudioOutOutput, fn_ip);
    FN("libSceAudioOut", sceAudioOutOutputs, fn_pu);
    FN("libSceAudioOut", sceAudioOutGetPortState, fn_ip);
    FN("libSceAudioOut", sceAudioOutSetMixLevelPadSpk, fn_ii);
    if (g_failures) return;

    CHECK(sceAudioOutOpen(1, 0, 0, 256, 48000, 1) == (int32_t)0x8026000F); /* not init */
    CHECK(sceAudioOutInit() == 0);
    CHECK(sceAudioOutInit() == (int32_t)0x8026000E);
    CHECK(sceAudioOutOpen(1, 0, 0, 100, 48000, 1) == (int32_t)0x80260006);   /* size */
    CHECK(sceAudioOutOpen(1, 0, 0, 256, 44100, 1) == (int32_t)0x80260008);   /* rate */
    CHECK(sceAudioOutOpen(1, 9, 0, 256, 48000, 1) == (int32_t)0x8026000A);   /* type */

    /* Main, 256 frames S16 stereo: 5.333 ms per buffer. */
    int32_t h = sceAudioOutOpen(1, 0, 0, 256, 48000, 1);
    CHECK(h == 0x20000000);
    int32_t bgm = sceAudioOutOpen(1, 1, 0, 1024, 48000, 4);
    CHECK(bgm == 0x20010008);
    CHECK(sceAudioOutOpen(1, 1, 0, 1024, 48000, 4) == (int32_t)0x80260005); /* BGM full */

    static int16_t buf[256 * 2];
    uint64_t t0 = plat_monotonic_ns();
    for (int i = 0; i < 31; i++) CHECK(sceAudioOutOutput(h, buf) == 512);
    uint64_t ms = (plat_monotonic_ns() - t0) / 1000000;
    /* The first buffer is accepted at once; 30 more take 30 * 5.333 = 160 ms.
     * Absolute deadlines keep the total close even with 1 ms+ sleep jitter. */
    printf("audioout: 31 outputs took %llu ms (expect ~160)\n", (unsigned long long)ms);
    CHECK(ms >= 150 && ms <= 260);

    /* NULL waits for the queued buffer without queueing another. */
    CHECK(sceAudioOutOutput(h, NULL) == 0);

    uint8_t state[32];
    memset(state, 0xAA, sizeof(state));
    CHECK(sceAudioOutGetPortState(h, state) == 0);
    CHECK(state[0] == 1 && state[2] == 2); /* primary, stereo */
    CHECK(sceAudioOutGetPortState(h, NULL) == (int32_t)0x80260004);
    CHECK(sceAudioOutGetPortState(0x1234, state) == (int32_t)0x80260003);

    CHECK(sceAudioOutSetMixLevelPadSpk(h, 1000) == (int32_t)0x8026000A);
    int32_t pad = sceAudioOutOpen(1, 4, 0, 256, 48000, 0);
    CHECK(pad == 0x20040011);
    CHECK(sceAudioOutSetMixLevelPadSpk(pad, 1000) == 0);

    /* Outputs needs matching sizes. */
    struct { int32_t handle; const void *ptr; } params[2] = {{h, buf}, {bgm, buf}};
    CHECK(sceAudioOutOutputs(params, 2) == (int32_t)0x80260006);
    params[1].handle = pad;
    CHECK(sceAudioOutOutputs(params, 2) == 256);
    params[1].handle = h;
    CHECK(sceAudioOutOutputs(params, 2) == (int32_t)0x80260003); /* duplicate */

    CHECK(sceAudioOutClose(h) == 0);
    CHECK(sceAudioOutClose(h) == (int32_t)0x80260001);
    CHECK(sceAudioOutOutput(h, buf) == (int32_t)0x80260001);
    CHECK(sceAudioOutClose(bgm) == 0);
    CHECK(sceAudioOutClose(pad) == 0);
}

typedef struct {
    uint64_t size_this;
    uint32_t granularity, rate, max_objects, queue_depth, buffer_mode, pad, num_beds;
} a3d_params;

typedef SYSV int32_t (*fn_l)(int64_t);
typedef SYSV int32_t (*fn_popen)(int32_t, const a3d_params *, uint32_t *);
typedef SYSV int32_t (*fn_u)(uint32_t);
typedef SYSV int32_t (*fn_up)(uint32_t, uint32_t *);
typedef SYSV int32_t (*fn_uu)(uint32_t, uint32_t);
typedef SYSV int32_t (*fn_attrs)(uint32_t, uint32_t, uint64_t, const void *);
typedef SYSV int32_t (*fn_pattr)(uint32_t, uint32_t, const void *, uint64_t);
typedef SYSV int32_t (*fn_level)(uint32_t, uint32_t *, uint32_t *);

static void test_audio3d(void) {
    FN("libSceAudio3d", sceAudio3dInitialize, fn_l);
    FN("libSceAudio3d", sceAudio3dTerminate, fn_v);
    FN("libSceAudio3d", sceAudio3dPortOpen, fn_popen);
    FN("libSceAudio3d", sceAudio3dPortClose, fn_u);
    FN("libSceAudio3d", sceAudio3dObjectReserve, fn_up);
    FN("libSceAudio3d", sceAudio3dObjectUnreserve, fn_uu);
    FN("libSceAudio3d", sceAudio3dObjectSetAttributes, fn_attrs);
    FN("libSceAudio3d", sceAudio3dPortSetAttribute, fn_pattr);
    FN("libSceAudio3d", sceAudio3dPortAdvance, fn_u);
    FN("libSceAudio3d", sceAudio3dPortPush, fn_uu);
    FN("libSceAudio3d", sceAudio3dPortGetQueueLevel, fn_level);
    if (g_failures) return;

    CHECK(sceAudio3dInitialize(0) == 0); /* AudioOut already initialized: fine */
    CHECK(sceAudio3dInitialize(0) == (int32_t)0x80EA0007);

    a3d_params p = {0x28, 256, 0, 4, 2, 2 /* ADVANCE_AND_PUSH */, 0, 2};
    uint32_t port = 99;
    CHECK(sceAudio3dPortOpen(0xFF, &p, &port) == 0);
    CHECK(port == 0);

    uint32_t obj[5];
    for (int i = 0; i < 4; i++) CHECK(sceAudio3dObjectReserve(port, &obj[i]) == 0);
    CHECK(obj[0] != obj[1]);
    CHECK(sceAudio3dObjectReserve(port, &obj[4]) == (int32_t)0x80EA0006);
    CHECK(obj[4] == 0xFFFFFFFFu);
    struct { uint32_t id, pad; const void *value; uint64_t size; } attr = {3, 0, &p, 4};
    CHECK(sceAudio3dObjectSetAttributes(port, obj[1], 1, &attr) == 0);
    CHECK(sceAudio3dObjectSetAttributes(port, obj[1], 0, &attr) == (int32_t)0x80EA0004);
    CHECK(sceAudio3dPortSetAttribute(port, 1, &p, 4) == 0);
    CHECK(sceAudio3dObjectUnreserve(port, obj[1]) == 0);
    CHECK(sceAudio3dObjectUnreserve(port, obj[1]) == (int32_t)0x80EA0003);
    CHECK(sceAudio3dObjectReserve(port, &obj[1]) == 0);

    uint32_t level = 9, avail = 9;
    CHECK(sceAudio3dPortGetQueueLevel(port, &level, &avail) == 0);
    CHECK(level == 0 && avail == 2);
    CHECK(sceAudio3dPortAdvance(port) == 0);
    CHECK(sceAudio3dPortPush(port, 0) == 0); /* not full yet: nothing sent */
    CHECK(sceAudio3dPortAdvance(port) == 0);
    CHECK(sceAudio3dPortAdvance(port) == (int32_t)0x80EA0007); /* full */
    CHECK(sceAudio3dPortGetQueueLevel(port, &level, &avail) == 0);
    CHECK(level == 2 && avail == 0);

    /* Steady state: advance + push, paced at 256 frames per push. */
    uint64_t t0 = plat_monotonic_ns();
    for (int i = 0; i < 20; i++) {
        CHECK(sceAudio3dPortPush(port, 1) == 0);
        CHECK(sceAudio3dPortAdvance(port) == 0);
    }
    uint64_t ms = (plat_monotonic_ns() - t0) / 1000000;
    printf("audio3d: 20 pushes took %llu ms (expect ~101)\n", (unsigned long long)ms);
    CHECK(ms >= 95 && ms <= 200);

    /* NO_ADVANCE port refuses Advance and Push. */
    a3d_params p2 = {0x10, 512, 0, 0, 0, 0, 0, 0};
    uint32_t port2;
    CHECK(sceAudio3dPortOpen(0xFF, &p2, &port2) == 0);
    CHECK(sceAudio3dPortAdvance(port2) == (int32_t)0x80EA0008);
    CHECK(sceAudio3dPortPush(port2, 0) == (int32_t)0x80EA0008);

    /* ADVANCE_NO_PUSH port drains on its own. */
    a3d_params p3 = {0x28, 256, 0, 8, 2, 1, 0, 2};
    uint32_t port3;
    CHECK(sceAudio3dPortOpen(0xFF, &p3, &port3) == 0);
    CHECK(sceAudio3dPortAdvance(port3) == 0);
    CHECK(sceAudio3dPortAdvance(port3) == 0);
    plat_sleep_us(30000);
    CHECK(sceAudio3dPortGetQueueLevel(port3, &level, NULL) == 0);
    CHECK(level == 0);

    CHECK(sceAudio3dPortClose(port) == 0);
    CHECK(sceAudio3dPortClose(port) == (int32_t)0x80EA0002);
    CHECK(sceAudio3dTerminate() == 0);
    CHECK(sceAudio3dTerminate() == (int32_t)0x80EA0007);
}

/* ---- Ngs2 -------------------------------------------------------------- */

typedef struct {
    void *host_buffer;
    uint64_t host_buffer_size;
    uint64_t reserved[5];
    uint64_t user_data;
} buffer_info;

static int g_allocs, g_frees;
static SYSV int32_t test_alloc(buffer_info *b) {
    CHECK(b->user_data == 0x5150);
    CHECK(b->host_buffer_size > 0);
    b->host_buffer = malloc(b->host_buffer_size);
    g_allocs++;
    return 0;
}
static SYSV int32_t test_free(buffer_info *b) {
    CHECK(b->host_buffer != NULL);
    free(b->host_buffer);
    g_frees++;
    return 0;
}

typedef struct {
    SYSV int32_t (*alloc)(buffer_info *);
    SYSV int32_t (*free)(buffer_info *);
    uint64_t user_data;
} allocator;

typedef struct {
    uint64_t size;
    char name[16];
    uint32_t flags, max_grain, num_grain, rate, reserved[6];
} sys_option;

typedef struct {
    uint64_t size;
    char name[16];
    uint32_t flags, max_grain, max_voices, max_delay, max_matrices, max_ports, reserved[20];
} rack_option;

typedef struct {
    void *buffer;
    uint64_t size;
    uint32_t waveform_type, num_channels;
} render_info;

typedef SYSV int32_t (*fn_syscreate)(const sys_option *, const allocator *, uint64_t *);
typedef SYSV int32_t (*fn_destroy)(uint64_t, buffer_info *);
typedef SYSV int32_t (*fn_render)(uint64_t, const render_info *, uint32_t);
typedef SYSV int32_t (*fn_query)(uint32_t, const rack_option *, buffer_info *);
typedef SYSV int32_t (*fn_rackcreate)(uint64_t, uint32_t, const rack_option *, const buffer_info *,
                                      uint64_t *);
typedef SYSV int32_t (*fn_rackcreatea)(uint64_t, uint32_t, const rack_option *, const allocator *,
                                       uint64_t *);
typedef SYSV int32_t (*fn_getvoice)(uint64_t, uint32_t, uint64_t *);
typedef SYSV int32_t (*fn_control)(uint64_t, const void *);
typedef SYSV int32_t (*fn_state)(uint64_t, void *, uint64_t);
typedef SYSV int32_t (*fn_flags)(uint64_t, uint32_t *);

static void test_ngs2(void) {
    FN("libSceNgs2", sceNgs2SystemCreateWithAllocator, fn_syscreate);
    FN("libSceNgs2", sceNgs2SystemDestroy, fn_destroy);
    FN("libSceNgs2", sceNgs2SystemRender, fn_render);
    FN("libSceNgs2", sceNgs2RackQueryBufferSize, fn_query);
    FN("libSceNgs2", sceNgs2RackCreate, fn_rackcreate);
    FN("libSceNgs2", sceNgs2RackCreateWithAllocator, fn_rackcreatea);
    FN("libSceNgs2", sceNgs2RackDestroy, fn_destroy);
    FN("libSceNgs2", sceNgs2RackGetVoiceHandle, fn_getvoice);
    FN("libSceNgs2", sceNgs2VoiceControl, fn_control);
    FN("libSceNgs2", sceNgs2VoiceGetState, fn_state);
    FN("libSceNgs2", sceNgs2VoiceGetStateFlags, fn_flags);
    if (g_failures) return;

    allocator a = {test_alloc, test_free, 0x5150};
    sys_option so = {sizeof(so), "sys", 0, 512, 256, 48000, {0}};
    uint64_t sys = 0;
    CHECK(sceNgs2SystemCreateWithAllocator(&so, NULL, &sys) == (int32_t)0x804A020A);
    so.rate = 12345;
    CHECK(sceNgs2SystemCreateWithAllocator(&so, &a, &sys) == (int32_t)0x804A0201);
    so.rate = 48000;
    CHECK(sceNgs2SystemCreateWithAllocator(&so, &a, &sys) == 0);
    CHECK(sys != 0 && g_allocs == 1);

    rack_option ro = {sizeof(ro), "sampler", 0, 512, 8, 0, 0, 0, {0}};
    buffer_info bi;
    CHECK(sceNgs2RackQueryBufferSize(0x1000, &ro, &bi) == 0);
    CHECK(bi.host_buffer_size > 0 && bi.host_buffer == NULL);
    bi.host_buffer = malloc(bi.host_buffer_size);
    uint64_t rack = 0, rack2 = 0;
    CHECK(sceNgs2RackCreate(sys, 0x1000, &ro, &bi, &rack) == 0);
    CHECK(sceNgs2RackCreate(sys + 8, 0x1000, &ro, &bi, &rack2) == (int32_t)0x804A0230);
    CHECK(sceNgs2RackCreateWithAllocator(sys, 0x3000, NULL, &a, &rack2) == 0);
    CHECK(g_allocs == 2);

    uint64_t v0 = 0, v7 = 0, vx;
    CHECK(sceNgs2RackGetVoiceHandle(rack, 0, &v0) == 0);
    CHECK(sceNgs2RackGetVoiceHandle(rack, 7, &v7) == 0);
    CHECK(v0 && v7 && v0 != v7);
    CHECK(sceNgs2RackGetVoiceHandle(rack, 8, &vx) == (int32_t)0x804A0302);
    CHECK(sceNgs2RackGetVoiceHandle(sys, 0, &vx) == (int32_t)0x804A0261);

    struct { uint16_t size; int16_t next; uint32_t id; uint32_t event; } ev = {12, 0, 5, 0};
    CHECK(sceNgs2VoiceControl(v0, &ev) == 0);
    CHECK(sceNgs2VoiceControl(rack, &ev) == (int32_t)0x804A0300);
    uint32_t flags = 0xFFFF;
    CHECK(sceNgs2VoiceGetStateFlags(v7, &flags) == 0 && flags == 0);
    uint8_t st[64];
    memset(st, 0xCC, sizeof(st));
    CHECK(sceNgs2VoiceGetState(v7, st, 48) == 0);
    CHECK(st[0] == 0 && st[47] == 0 && st[48] == 0xCC);

    float out[256 * 2];
    for (int i = 0; i < 512; i++) out[i] = 1.0f;
    render_info ri = {out, sizeof(out), 0, 2};
    CHECK(sceNgs2SystemRender(sys, &ri, 1) == 0);
    CHECK(out[0] == 0.0f && out[511] == 0.0f);
    CHECK(sceNgs2SystemRender(rack, &ri, 1) == (int32_t)0x804A0230);

    buffer_info back;
    CHECK(sceNgs2RackDestroy(rack, &back) == 0);
    CHECK(back.host_buffer == bi.host_buffer);
    CHECK(sceNgs2RackDestroy(rack, &back) == (int32_t)0x804A0261);
    CHECK(sceNgs2VoiceGetStateFlags(v0, &flags) == (int32_t)0x804A0300);
    free(bi.host_buffer);
    CHECK(sceNgs2RackDestroy(rack2, NULL) == 0);
    CHECK(g_frees == 1);
    CHECK(sceNgs2SystemDestroy(sys, NULL) == 0);
    CHECK(g_frees == 2);
    CHECK(sceNgs2SystemDestroy(sys, NULL) == (int32_t)0x804A0230);
}

int main(void) {
    log_init();
    hle_init();
    test_audioout();
    test_audio3d();
    test_ngs2();
    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("audio tests passed\n");
    return 0;
}
