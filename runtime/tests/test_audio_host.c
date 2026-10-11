/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host audio: stream formats and channel orders, peaks, and AudioOut paced by
 * the device queue (SDL's dummy driver consumes in real time). */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_host.h"
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

static void test_channel_order(void) {
    /* Non-STD 8ch (L R C LFE Ls Rs Lb Rb) comes out in SDL's order. */
    audio_host_stream *s = audio_host_stream_new_unbound(8);
    CHECK(s != NULL);
    if (!s) return;
    float in[8] = {0, 1, 2, 3, 4, 5, 6, 7}, out[8] = {0};
    CHECK(audio_host_stream_config(s, 8, true, false));
    audio_host_stream_put(s, in, sizeof(in));
    CHECK(audio_host_stream_queued(s) == 1);
    CHECK(audio_host_stream_get(s, out, sizeof(out)) == (int)sizeof(out));
    static const float want_nonstd[8] = {0, 1, 2, 3, 6, 7, 4, 5};
    CHECK(memcmp(out, want_nonstd, sizeof(out)) == 0);

    /* STD is SDL's order already. */
    s = audio_host_stream_new_unbound(8);
    CHECK(audio_host_stream_config(s, 8, true, true));
    audio_host_stream_put(s, in, sizeof(in));
    CHECK(audio_host_stream_get(s, out, sizeof(out)) == (int)sizeof(out));
    CHECK(memcmp(out, in, sizeof(out)) == 0);

    /* S16 stereo: 2 frames queued, converted to float. */
    s = audio_host_stream_new_unbound(2);
    CHECK(audio_host_stream_config(s, 2, false, false));
    int16_t pcm[4] = {16384, -16384, 0, 32767};
    audio_host_stream_put(s, pcm, sizeof(pcm));
    CHECK(audio_host_stream_queued(s) == 2);
    float f[4] = {0};
    CHECK(audio_host_stream_get(s, f, sizeof(f)) == (int)sizeof(f));
    CHECK(f[0] > 0.49f && f[0] < 0.51f && f[1] < -0.49f && f[1] > -0.51f && f[2] == 0.0f);

    /* Reconfiguring drops what was queued. */
    audio_host_stream_put(s, pcm, sizeof(pcm));
    CHECK(audio_host_stream_config(s, 1, true, false));
    CHECK(audio_host_stream_queued(s) == 0);
}

static void test_peak(void) {
    float f[4] = {0.1f, -0.75f, 0.5f, 0};
    CHECK(audio_host_peak(f, 4, true) == 0.75f);
    int16_t p[3] = {100, -32768, 5};
    CHECK(audio_host_peak(p, 3, false) == 1.0f);
    CHECK(audio_host_peak(f, 0, true) == 0.0f);
}

typedef SYSV int32_t (*fn_v)(void);
typedef SYSV int32_t (*fn_i)(int32_t);
typedef SYSV int32_t (*fn_open)(int32_t, int32_t, int32_t, uint32_t, uint32_t, uint32_t);
typedef SYSV int32_t (*fn_ip)(int32_t, const void *);
typedef SYSV int32_t (*fn_pu)(const void *, uint32_t);

typedef struct {
    int32_t handle;
    const void *ptr;
} output_param;

/* Main port, 256 frames of F32 8ch (the format SotC opens): Output returns at
 * once until the queue holds the target, then at the device's rate. */
static void test_paced_output(void) {
    FN("libSceAudioOut", sceAudioOutInit, fn_v);
    FN("libSceAudioOut", sceAudioOutOpen, fn_open);
    FN("libSceAudioOut", sceAudioOutClose, fn_i);
    FN("libSceAudioOut", sceAudioOutOutput, fn_ip);
    FN("libSceAudioOut", sceAudioOutOutputs, fn_pu);
    if (g_failures) return;
    CHECK(sceAudioOutInit() == 0);
    int32_t h = sceAudioOutOpen(255, 0, 0, 256, 48000, 5);
    int32_t bgm = sceAudioOutOpen(255, 1, 0, 256, 48000, 5);
    int32_t pad = sceAudioOutOpen(1, 4, 0, 256, 48000, 3);
    CHECK(h == 0x20000000 && bgm == 0x20010008 && pad == 0x20040011);

    static float buf[256 * 8];
    for (int i = 0; i < 256 * 8; i++) buf[i] = (i & 1) ? 0.25f : -0.25f;
    uint32_t target = audio_host_target_frames();
    printf("audio_host: target %u frames\n", target);

    /* Filling the queue up to the target does not block. */
    uint64_t t0 = plat_monotonic_ns();
    uint32_t fill = target / 256;
    for (uint32_t i = 0; i < fill; i++) CHECK(sceAudioOutOutput(h, buf) == 256 * 8);
    uint64_t fill_ms = (plat_monotonic_ns() - t0) / 1000000;
    printf("audio_host: %u buffers to fill the queue took %llu ms\n", fill,
           (unsigned long long)fill_ms);
    CHECK(fill_ms < 30);

    /* Then 75 buffers (400 ms of audio) take about 400 ms. */
    t0 = plat_monotonic_ns();
    for (int i = 0; i < 75; i++) CHECK(sceAudioOutOutput(h, buf) == 256 * 8);
    uint64_t ms = (plat_monotonic_ns() - t0) / 1000000;
    printf("audio_host: 75 paced outputs took %llu ms (expect ~400)\n", (unsigned long long)ms);
    CHECK(ms >= 300 && ms <= 600);

    /* Outputs over main + bgm, paced the same way. */
    output_param two[2] = {{h, buf}, {bgm, buf}};
    t0 = plat_monotonic_ns();
    for (int i = 0; i < 75; i++) CHECK(sceAudioOutOutputs(two, 2) == 256);
    ms = (plat_monotonic_ns() - t0) / 1000000;
    printf("audio_host: 75 paced Outputs(main, bgm) took %llu ms (expect ~400, bgm fills first)\n",
           (unsigned long long)ms);
    CHECK(ms >= 250 && ms <= 600);

    /* The pad speaker is not played: clock pacing, 5.33 ms per buffer. */
    static float mono[256];
    t0 = plat_monotonic_ns();
    for (int i = 0; i < 31; i++) CHECK(sceAudioOutOutput(pad, mono) == 256);
    ms = (plat_monotonic_ns() - t0) / 1000000;
    printf("audio_host: 31 pad speaker outputs took %llu ms (expect ~160)\n",
           (unsigned long long)ms);
    CHECK(ms >= 150 && ms <= 260);

    CHECK(sceAudioOutClose(h) == 0);
    CHECK(sceAudioOutClose(bgm) == 0);
    CHECK(sceAudioOutClose(pad) == 0);
    /* A reopened slot reuses its stream, emptied by Close. */
    h = sceAudioOutOpen(255, 0, 0, 256, 48000, 1);
    static int16_t s16[256 * 2];
    t0 = plat_monotonic_ns();
    CHECK(sceAudioOutOutput(h, s16) == 512);
    CHECK((plat_monotonic_ns() - t0) / 1000000 < 10);
    CHECK(sceAudioOutClose(h) == 0);
}

int main(void) {
    log_init();
    hle_init();
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    putenv("SOTC_AUDIO_LATENCY_MS=20");
    if (!SDL_Init(0)) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    test_peak();
    test_channel_order();
    CHECK(!audio_host_ready());
    CHECK(audio_host_init());
    CHECK(audio_host_ready());
    if (audio_host_ready()) test_paced_output();
    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("audio host tests passed\n");
    return 0;
}
