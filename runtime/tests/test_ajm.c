/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceAjm: contexts, instances, the batch chunk format, and ATRAC9 jobs
 * (silent decode: input consumed per frame/superframe, output and sideband
 * sizes exact). Also the libkernel functions the firmware Ngs2 imports. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle/hle.h"
#include "runtime.h"

static int g_failures;
#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                         \
        }                                                                         \
    } while (0)

#define FN(lib, name, type) type name = (type)hle_lookup(lib, #name); CHECK(name != NULL)

typedef struct { void *address; uint64_t size; } ajm_buffer;
typedef struct { int32_t code; const void *job; uint32_t offset; const void *ra; } ajm_batch_error;

typedef SYSV int32_t (*fn_init)(int64_t, uint32_t *);
typedef SYSV int32_t (*fn_u)(uint32_t);
typedef SYSV int32_t (*fn_reg)(uint32_t, uint32_t, int64_t);
typedef SYSV int32_t (*fn_uu)(uint32_t, uint32_t);
typedef SYSV int32_t (*fn_create)(uint32_t, uint32_t, uint64_t, uint32_t *);
typedef SYSV void *(*fn_control)(void *, uint32_t, uint64_t, void *, uint64_t, void *, uint64_t, void *);
typedef SYSV void *(*fn_run)(void *, uint32_t, uint64_t, void *, uint64_t, void *, uint64_t, void *,
                             uint64_t, void *);
typedef SYSV void *(*fn_split)(void *, uint32_t, uint64_t, const ajm_buffer *, uint64_t,
                               const ajm_buffer *, uint64_t, void *, uint64_t, void *);
typedef SYSV void *(*fn_inline)(void *, const void *, uint64_t, const void **);
typedef SYSV int32_t (*fn_start)(uint32_t, void *, uint32_t, int32_t, ajm_batch_error *, uint32_t *);
typedef SYSV int32_t (*fn_wait)(uint32_t, uint32_t, uint32_t, ajm_batch_error *);

#define ERR_INVALID_CONTEXT   ((int32_t)0x80930002)
#define ERR_INVALID_INSTANCE  ((int32_t)0x80930003)
#define ERR_INVALID_BATCH     ((int32_t)0x80930004)
#define ERR_INVALID_PARAMETER ((int32_t)0x80930005)
#define ERR_ALREADY_REGISTERED ((int32_t)0x80930009)
#define ERR_NOT_REGISTERED    ((int32_t)0x8093000A)
#define ERR_WRONG_REVISION    ((int32_t)0x8093000B)
#define ERR_MALFORMED_BATCH   ((int32_t)0x80930011)
#define RESULT_NOT_INITIALIZED 0x1
#define RESULT_PARTIAL_INPUT   0x8
#define RESULT_NOT_ENOUGH_ROOM 0x10

/* Job flags: version:3 codec:8 run:2 control:3 reserved:29 sideband:3. */
#define JOB(run, control, sideband) (1ull | (uint64_t)(run) << 11 | (uint64_t)(control) << 13 | \
                                     (uint64_t)(sideband) << 45)
#define RUN_INFO 1
#define RUN_MULTI 2
#define CTL_RESET 1
#define CTL_INIT 2
#define SB_GAPLESS 1
#define SB_FORMAT 2
#define SB_STREAM 4

/* ATRAC9 config: 0xFE, then sample rate index 7 (48 kHz), channel config 2
 * (stereo), frame bytes 192, superframe index 2 (4 frames: 768 bytes). */
#define FRAME_BYTES 192u
#define SUPERFRAME (4 * FRAME_BYTES)
static const uint8_t k_config[8] = {0xFE, (7 << 4) | (2 << 1), (uint8_t)((FRAME_BYTES - 1) >> 3),
                                    (uint8_t)(((FRAME_BYTES - 1) & 7) << 5 | 2 << 3), 0, 0, 0, 0};

typedef struct { int32_t result, internal; } sb_result;
typedef struct { int32_t consumed, written; uint64_t total; } sb_stream;
typedef struct { uint32_t channels, mask, rate, encoding, bitrate, reserved; } sb_format;
typedef struct { uint32_t total; uint16_t skip, skipped; } sb_gapless;
typedef struct { uint32_t superframe, frames, next, samples; } sb_info;

static uint8_t g_batch[4096] __attribute__((aligned(8)));
static uint8_t g_input[4 * SUPERFRAME];
static uint8_t g_output[16384];

static void test_ajm(void) {
    FN("libSceAjm", sceAjmInitialize, fn_init);
    FN("libSceAjm", sceAjmFinalize, fn_u);
    FN("libSceAjm", sceAjmModuleRegister, fn_reg);
    FN("libSceAjm", sceAjmModuleUnregister, fn_uu);
    FN("libSceAjm", sceAjmInstanceCreate, fn_create);
    FN("libSceAjm", sceAjmInstanceDestroy, fn_uu);
    FN("libSceAjm", sceAjmBatchJobControlBufferRa, fn_control);
    FN("libSceAjm", sceAjmBatchJobRunBufferRa, fn_run);
    FN("libSceAjm", sceAjmBatchJobRunSplitBufferRa, fn_split);
    FN("libSceAjm", sceAjmBatchJobInlineBuffer, fn_inline);
    FN("libSceAjm", sceAjmBatchStartBuffer, fn_start);
    FN("libSceAjm", sceAjmBatchWait, fn_wait);
    if (g_failures) return;

    uint32_t ctx = 0, inst = 0, batch = 0;
    CHECK(sceAjmInitialize(1, &ctx) == ERR_INVALID_PARAMETER);
    CHECK(sceAjmInitialize(0, &ctx) == 0 && ctx != 0);
    CHECK(sceAjmModuleRegister(ctx + 1, 1, 0) == ERR_INVALID_CONTEXT);
    CHECK(sceAjmInstanceCreate(ctx, 1, 1, &inst) == ERR_NOT_REGISTERED);
    CHECK(sceAjmModuleRegister(ctx, 1, 0) == 0);
    CHECK(sceAjmModuleRegister(ctx, 1, 0) == ERR_ALREADY_REGISTERED);
    CHECK(sceAjmInstanceCreate(ctx, 1, 0, &inst) == ERR_WRONG_REVISION);
    /* version 1, 2 channels, S16, no gapless loop, codec flags 0. */
    CHECK(sceAjmInstanceCreate(ctx, 1, 1 | 2 << 3, &inst) == 0);
    CHECK(inst == (1u | 1u << 14));

    /* Run before Initialize: NOT_INITIALIZED, nothing consumed. */
    struct { sb_result r; sb_stream s; } side0;
    memset(&side0, 0xCC, sizeof(side0));
    uint8_t *end = sceAjmBatchJobRunBufferRa(g_batch, inst, JOB(0, 0, SB_STREAM), g_input, SUPERFRAME,
                                             g_output, sizeof(g_output), &side0, sizeof(side0), NULL);
    CHECK(end - g_batch == 8 + 16 + 8 + 16 + 16);
    CHECK(sceAjmBatchStartBuffer(ctx, g_batch, (uint32_t)(end - g_batch), 0, NULL, &batch) == 0);
    CHECK(sceAjmBatchWait(ctx, batch, ~0u, NULL) == 0);
    CHECK(sceAjmBatchWait(ctx, batch, ~0u, NULL) == ERR_INVALID_BATCH);
    CHECK(side0.r.result == RESULT_NOT_INITIALIZED);
    CHECK(side0.s.consumed == 0 && side0.s.written == 0);

    /* Reset + Initialize with the config; the format sideband comes back. The
     * init parameters ride in an inline chunk, as Ngs2 may do. */
    const void *init_params = NULL;
    uint8_t *p = sceAjmBatchJobInlineBuffer(g_batch, k_config, 8, &init_params);
    CHECK(p - g_batch == 16 && init_params == g_batch + 8);
    struct { sb_result r; sb_format f; } side1;
    memset(&side1, 0xCC, sizeof(side1));
    end = sceAjmBatchJobControlBufferRa(p, inst, JOB(0, CTL_RESET | CTL_INIT, SB_FORMAT),
                                        (void *)init_params, 8, &side1, sizeof(side1), (void *)1);
    CHECK(sceAjmBatchStartBuffer(ctx, g_batch, (uint32_t)(end - g_batch), 0, NULL, &batch) == 0);
    CHECK(sceAjmBatchWait(ctx, batch, ~0u, NULL) == 0);
    CHECK(side1.r.result == 0);
    CHECK(side1.f.channels == 2 && side1.f.mask == 3 && side1.f.rate == 48000);
    CHECK(side1.f.encoding == 0);
    CHECK(side1.f.bitrate == 48000u * SUPERFRAME * 8 / (4 * 256));   /* 144 kbps */

    /* One frame from a full superframe: 192 bytes in, 256 stereo S16 out. */
    struct { sb_result r; sb_stream s; sb_info i; } side2;
    memset(g_output, 0x55, sizeof(g_output));
    end = sceAjmBatchJobRunBufferRa(g_batch, inst, JOB(RUN_INFO, 0, SB_STREAM), g_input, SUPERFRAME,
                                    g_output, sizeof(g_output), &side2, sizeof(side2), NULL);
    CHECK(sceAjmBatchStartBuffer(ctx, g_batch, (uint32_t)(end - g_batch), 0, NULL, &batch) == 0);
    CHECK(sceAjmBatchWait(ctx, batch, 0, NULL) == 0);
    CHECK(side2.r.result == 0);
    CHECK(side2.s.consumed == (int32_t)FRAME_BYTES && side2.s.written == 256 * 4);
    CHECK(side2.s.total == 256);
    CHECK(side2.i.superframe == SUPERFRAME && side2.i.frames == 4 && side2.i.samples == 256);
    CHECK(side2.i.next == SUPERFRAME - FRAME_BYTES);
    CHECK(g_output[0] == 0 && g_output[1023] == 0 && g_output[1024] == 0x55);

    /* The rest of the superframe (576 bytes) and 2 more, split over two input
     * and two output buffers, multiple frames: 3 + 8 frames, then input runs
     * out (PARTIAL_INPUT, as shadPS4 reports it). */
    ajm_buffer in[2] = {{g_input, 1000}, {g_input + 1000, SUPERFRAME - FRAME_BYTES + 2 * SUPERFRAME - 1000}};
    ajm_buffer out[2] = {{g_output, 5000}, {g_output + 5000, sizeof(g_output) - 5000}};
    struct { sb_result r; sb_stream s; uint32_t mframe[2]; } side3;
    memset(g_output, 0x55, sizeof(g_output));
    end = sceAjmBatchJobRunSplitBufferRa(g_batch, inst, JOB(RUN_MULTI, 0, SB_STREAM), in, 2, out, 2,
                                         &side3, sizeof(side3), NULL);
    CHECK(sceAjmBatchStartBuffer(ctx, g_batch, (uint32_t)(end - g_batch), 0, NULL, &batch) == 0);
    CHECK(sceAjmBatchWait(ctx, batch, 0, NULL) == 0);
    CHECK(side3.r.result == RESULT_PARTIAL_INPUT);
    CHECK(side3.mframe[0] == 11);
    CHECK(side3.s.consumed == (int32_t)(in[0].size + in[1].size));
    CHECK(side3.s.written == 11 * 1024);
    CHECK(side3.s.total == 12 * 256);
    CHECK(g_output[5000] == 0 && g_output[11 * 1024 - 1] == 0 && g_output[11 * 1024] == 0x55);

    /* Not enough room for a frame: nothing consumed. */
    struct { sb_result r; sb_stream s; } side4;
    end = sceAjmBatchJobRunBufferRa(g_batch, inst, JOB(0, 0, SB_STREAM), g_input, SUPERFRAME,
                                    g_output, 1000, &side4, sizeof(side4), NULL);
    CHECK(sceAjmBatchStartBuffer(ctx, g_batch, (uint32_t)(end - g_batch), 0, NULL, &batch) == 0);
    CHECK(sceAjmBatchWait(ctx, batch, 0, NULL) == 0);
    CHECK(side4.r.result == RESULT_NOT_ENOUGH_ROOM && side4.s.consumed == 0 && side4.s.written == 0);

    /* Partial superframe: nothing consumed. */
    end = sceAjmBatchJobRunBufferRa(g_batch, inst, JOB(0, 0, SB_STREAM), g_input, SUPERFRAME - 1,
                                    g_output, sizeof(g_output), &side4, sizeof(side4), NULL);
    CHECK(sceAjmBatchStartBuffer(ctx, g_batch, (uint32_t)(end - g_batch), 0, NULL, &batch) == 0);
    CHECK(sceAjmBatchWait(ctx, batch, 0, NULL) == 0);
    CHECK(side4.r.result == RESULT_PARTIAL_INPUT && side4.s.consumed == 0);

    /* Gapless: reset, 300 samples after skipping 100. Frame 1 gives 156,
     * frame 2 gives 144 and ends the stream, which skips the rest of the
     * superframe; then the input is gone. */
    sb_gapless gapless = {300, 100, 0};
    struct { sb_result r; sb_gapless g; } side5;
    end = sceAjmBatchJobControlBufferRa(g_batch, inst, JOB(0, CTL_RESET, SB_GAPLESS), &gapless,
                                        sizeof(gapless), &side5, sizeof(side5), NULL);
    CHECK(sceAjmBatchStartBuffer(ctx, g_batch, (uint32_t)(end - g_batch), 0, NULL, &batch) == 0);
    CHECK(sceAjmBatchWait(ctx, batch, 0, NULL) == 0);
    CHECK(side5.r.result == 0 && side5.g.total == 300 && side5.g.skip == 100);
    struct { sb_result r; sb_stream s; sb_gapless g; uint32_t mframe[2]; } side6;
    end = sceAjmBatchJobRunBufferRa(g_batch, inst, JOB(RUN_MULTI, 0, SB_STREAM | SB_GAPLESS), g_input,
                                    SUPERFRAME, g_output, sizeof(g_output), &side6, sizeof(side6), NULL);
    CHECK(sceAjmBatchStartBuffer(ctx, g_batch, (uint32_t)(end - g_batch), 0, NULL, &batch) == 0);
    CHECK(sceAjmBatchWait(ctx, batch, 0, NULL) == 0);
    CHECK(side6.r.result == RESULT_PARTIAL_INPUT);
    CHECK(side6.mframe[0] == 2);
    CHECK(side6.s.consumed == (int32_t)SUPERFRAME && side6.s.written == 300 * 4 && side6.s.total == 300);
    CHECK(side6.g.total == 0 && side6.g.skip == 0 && side6.g.skipped == 100 + 112);

    /* Malformed: an unknown chunk. Nothing runs; the error points at the job. */
    ajm_batch_error err = {0};
    end = sceAjmBatchJobRunBufferRa(g_batch, inst, JOB(0, 0, SB_STREAM), g_input, SUPERFRAME,
                                    g_output, sizeof(g_output), &side4, sizeof(side4), NULL);
    uint8_t *second = end;
    end = sceAjmBatchJobRunBufferRa(second, inst, JOB(0, 0, SB_STREAM), g_input, SUPERFRAME,
                                    g_output, sizeof(g_output), &side4, sizeof(side4), NULL);
    second[8] = 0x3F;   /* first chunk of the second job: ident 63 */
    memset(&side4, 0xCC, sizeof(side4));
    CHECK(sceAjmBatchStartBuffer(ctx, g_batch, (uint32_t)(end - g_batch), 0, &err, &batch) ==
          ERR_MALFORMED_BATCH);
    CHECK(err.code == ERR_MALFORMED_BATCH && err.offset == (uint32_t)(second - g_batch));
    CHECK(side4.r.result == (int32_t)0xCCCCCCCC);
    CHECK(sceAjmBatchStartBuffer(ctx, g_batch, 12, 0, &err, &batch) == ERR_MALFORMED_BATCH);

    /* Statistics instance: zeroed sideband. */
    uint32_t stats[8];
    memset(stats, 0xCC, sizeof(stats));
    end = sceAjmBatchJobControlBufferRa(g_batch, 0x80000, 0x10007, NULL, 0, stats, sizeof(stats), NULL);
    CHECK(sceAjmBatchStartBuffer(ctx, g_batch, (uint32_t)(end - g_batch), 0, NULL, &batch) == 0);
    CHECK(sceAjmBatchWait(ctx, batch, 0, NULL) == 0);
    CHECK(stats[0] == 0 && stats[7] == 0);

    /* Many batches without waits reuse ids instead of failing. */
    end = sceAjmBatchJobRunBufferRa(g_batch, inst, JOB(0, 0, 0), NULL, 0, NULL, 0, NULL, 0, NULL);
    for (int i = 0; i < 300; i++)
        CHECK(sceAjmBatchStartBuffer(ctx, g_batch, (uint32_t)(end - g_batch), 0, NULL, &batch) == 0);

    CHECK(sceAjmInstanceDestroy(ctx, inst) == 0);
    CHECK(sceAjmInstanceDestroy(ctx, inst) == ERR_INVALID_INSTANCE);
    CHECK(sceAjmModuleUnregister(ctx, 1) == 0);
    CHECK(sceAjmModuleUnregister(ctx, 1) == ERR_NOT_REGISTERED);
    CHECK(sceAjmFinalize(ctx) == 0);
    CHECK(sceAjmFinalize(ctx) == ERR_INVALID_CONTEXT);
}

typedef SYSV int32_t (*fn_pp)(void *, void *);
typedef SYSV void *(*fn_self)(void);
typedef SYSV int32_t (*fn_sleep)(uint32_t);
typedef SYSV int32_t (*fn_ver)(int32_t *);
typedef SYSV void (*fn_sincosf)(float, float *, float *);

static void test_kernel(void) {
    FN("libkernel", pthread_equal, fn_pp);
    FN("libkernel", usleep, fn_sleep);
    FN("libkernel", sceKernelGetCompiledSdkVersion, fn_ver);
    FN("libSceLibcInternal", sincosf, fn_sincosf);
    if (g_failures) return;
    float s = 0, c = 0;
    sincosf(0.5f, &s, &c);
    CHECK(s > 0.4794f && s < 0.4795f && c > 0.8775f && c < 0.8776f);
    CHECK(pthread_equal((void *)0x10, (void *)0x10) != 0);
    CHECK(pthread_equal((void *)0x10, (void *)0x20) == 0);
    uint64_t t0 = plat_monotonic_ns();
    CHECK(usleep(2000) == 0);
    CHECK(plat_monotonic_ns() - t0 >= 1500000);
    g_rt.sdk_version = 0x04508101;
    int32_t v = 0;
    CHECK(sceKernelGetCompiledSdkVersion(&v) == 0 && v == 0x04508101);
}

int main(void) {
    log_init();
    hle_init();
    test_ajm();
    test_kernel();
    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_ajm: OK\n");
    return 0;
}
