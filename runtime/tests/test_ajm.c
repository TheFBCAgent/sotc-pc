/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceAjm: contexts, instances, the batch chunk format, and ATRAC9 jobs
 * (LibAtrac9 decode of a synthetic stream: input consumed per frame and
 * superframe, PCM equal to LibAtrac9's own in S16/S32/F32, interleaved or
 * planar, gapless, codec errors, sideband sizes). Also the libkernel
 * functions the firmware Ngs2 imports. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle/hle.h"
#include "libatrac9.h"
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
#define RESULT_CODEC_ERROR 0x40000000u
#define RESULT_FATAL 0x80000000u

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

/* ATRAC9 config: 0xFE, then sample rate index 7 (48 kHz), channel config 1
 * (two mono blocks: stereo output), frame bytes 192, superframe index 2 (4
 * frames: 768 bytes). */
#define FRAME_BYTES 192u
#define SUPERFRAME (4 * FRAME_BYTES)
#define FRAME_SAMPLES 256u
#define CONFIG(chan) {0xFE, (7 << 4) | ((chan) << 1), (uint8_t)((FRAME_BYTES - 1) >> 3), \
                      (uint8_t)(((FRAME_BYTES - 1) & 7) << 5 | 2 << 3), 0, 0, 0, 0}
static const uint8_t k_config[8] = CONFIG(1);
static const uint8_t k_config_mono[8] = CONFIG(0);
static const uint8_t k_config_bad[8] = CONFIG(6);   /* channel config 6 doesn't exist */

typedef struct { int32_t result, internal; } sb_result;
typedef struct { int32_t consumed, written; uint64_t total; } sb_stream;
typedef struct { uint32_t channels, mask, rate, encoding, bitrate, reserved; } sb_format;
typedef struct { uint32_t total; uint16_t skip, skipped; } sb_gapless;
typedef struct { uint32_t superframe, frames, next, samples; } sb_info;

#define SUPERFRAMES 3
static uint8_t g_batch[4096] __attribute__((aligned(8)));
static uint8_t g_input[SUPERFRAMES * SUPERFRAME];
static uint8_t g_output[65536];
static uint32_t g_frame_size;   /* bytes per synthetic frame */

/* No real ATRAC9 can be committed (game data), so the stream is synthetic: a
 * hand-built mono block that LibAtrac9 accepts. Its header bits: first in
 * superframe (0 = yes), no band param reuse, band count 0 (+ minimum), no
 * band extension, gradient mode 0 with all-zero parameters, no extension
 * data, then scale factor mode 1 (fixed-length: 2 bits each over base value
 * 20). The 0x55 filler becomes the spectra, so the PCM is non-zero. A frame
 * is two such blocks (channel config 1); a superframe is 4 frames, then zero
 * padding. LibAtrac9 itself (the reference) measures the block length. */
static void make_block(uint8_t *p, uint32_t size, bool first) {
    memset(p, 0x55, size);
    p[0] = first ? 0x00 : 0x80;
    p[1] = p[2] = p[3] = 0;
    p[4] = 0x04;   /* bits 36-37: scale factor mode 1; bits 38-39: 2-bit values */
    p[5] = 0xA0;   /* base value 20 */
}

static bool build_stream(void) {
    uint8_t block[FRAME_BYTES];
    short pcm[FRAME_SAMPLES];
    int used = 0;
    void *mono = Atrac9GetHandle();
    make_block(block, sizeof(block), true);
    CHECK(Atrac9InitDecoder(mono, (uint8_t *)k_config_mono) == 0);
    CHECK(Atrac9Decode(mono, block, sizeof(block), pcm, &used, 0) == 0);
    Atrac9ReleaseHandle(mono);
    if (used <= 0 || 2 * 4 * (uint32_t)used > SUPERFRAME) return false;
    g_frame_size = 2 * (uint32_t)used;
    memset(g_input, 0, sizeof(g_input));
    for (uint32_t s = 0; s < SUPERFRAMES; s++)
        for (uint32_t f = 0; f < 4; f++)
            for (uint32_t b = 0; b < 2; b++) {
                uint8_t *p = g_input + s * SUPERFRAME + f * g_frame_size + b * (uint32_t)used;
                uint8_t tmp[FRAME_BYTES];
                make_block(tmp, sizeof(tmp), f == 0);
                memcpy(p, tmp, (uint32_t)used);
            }
    return true;
}

/* The reference: LibAtrac9 straight over `frames` frames of the stream, in
 * the given format (0 S16, 1 S32, 2 F32), PCM appended to `out`. */
static uint32_t reference(int format, int planar, uint32_t frames, uint8_t *out) {
    void *h = Atrac9GetHandle();
    CHECK(Atrac9InitDecoder(h, (uint8_t *)k_config) == 0);
    uint32_t at = 0, sample_bytes = format == 0 ? 2 : 4, frame_out = FRAME_SAMPLES * 2 * sample_bytes;
    for (uint32_t i = 0; i < frames; i++) {
        const uint8_t *p = g_input + (i / 4) * SUPERFRAME + (i % 4) * g_frame_size;
        int used = 0, r;
        if (format == 0) r = Atrac9Decode(h, p, (int)g_frame_size, (short *)(out + at), &used, planar);
        else if (format == 1) r = Atrac9DecodeS32(h, p, (int)g_frame_size, (int *)(out + at), &used, planar);
        else r = Atrac9DecodeF32(h, p, (int)g_frame_size, (float *)(out + at), &used, planar);
        CHECK(r == 0 && (uint32_t)used == g_frame_size);
        at += frame_out;
    }
    Atrac9ReleaseHandle(h);
    return at;
}

static bool any_nonzero(const uint8_t *p, uint32_t size) {
    for (uint32_t i = 0; i < size; i++)
        if (p[i]) return true;
    return false;
}

static uint8_t g_ref[SUPERFRAMES * 4 * FRAME_SAMPLES * 2 * 4];

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

    if (!build_stream()) {
        CHECK(!"LibAtrac9 rejects the synthetic block");
        return;
    }
    CHECK(g_frame_size > 0 && 4 * g_frame_size < SUPERFRAME);   /* frames vary; padding follows */

#define SUBMIT(end_)                                                                              \
    do {                                                                                          \
        CHECK(sceAjmBatchStartBuffer(ctx, g_batch, (uint32_t)((uint8_t *)(end_) - g_batch), 0,    \
                                     NULL, &batch) == 0);                                         \
        CHECK(sceAjmBatchWait(ctx, batch, 0, NULL) == 0);                                         \
    } while (0)

    /* Reset + Initialize with the config; the format sideband comes back. The
     * init parameters ride in an inline chunk, as Ngs2 may do. */
    const void *init_params = NULL;
    uint8_t *p = sceAjmBatchJobInlineBuffer(g_batch, k_config, 8, &init_params);
    CHECK(p - g_batch == 16 && init_params == g_batch + 8);
    struct { sb_result r; sb_format f; } side1;
    memset(&side1, 0xCC, sizeof(side1));
    end = sceAjmBatchJobControlBufferRa(p, inst, JOB(0, CTL_RESET | CTL_INIT, SB_FORMAT),
                                        (void *)init_params, 8, &side1, sizeof(side1), (void *)1);
    SUBMIT(end);
    CHECK(side1.r.result == 0);
    CHECK(side1.f.channels == 2 && side1.f.mask == 3 && side1.f.rate == 48000);
    CHECK(side1.f.encoding == 0);
    CHECK(side1.f.bitrate == 48000u * SUPERFRAME * 8 / (4 * 256));   /* 288 kbps */

    /* One frame from a full superframe: its bytes in, 256 stereo S16 out, the
     * same PCM as LibAtrac9's own (non-zero). */
    reference(0, 0, 4 * SUPERFRAMES, g_ref);
    CHECK(any_nonzero(g_ref, 4 * SUPERFRAMES * 1024));
    struct { sb_result r; sb_stream s; sb_info i; } side2;
    memset(g_output, 0x55, sizeof(g_output));
    end = sceAjmBatchJobRunBufferRa(g_batch, inst, JOB(RUN_INFO, 0, SB_STREAM), g_input, SUPERFRAME,
                                    g_output, sizeof(g_output), &side2, sizeof(side2), NULL);
    SUBMIT(end);
    CHECK(side2.r.result == 0);
    CHECK(side2.s.consumed == (int32_t)g_frame_size && side2.s.written == 256 * 4);
    CHECK(side2.s.total == 256);
    CHECK(side2.i.superframe == SUPERFRAME && side2.i.frames == 4 && side2.i.samples == 256);
    CHECK(side2.i.next == SUPERFRAME - g_frame_size);
    CHECK(memcmp(g_output, g_ref, 1024) == 0 && g_output[1024] == 0x55);

    /* The rest of the superframe (3 frames, then the padding) and 2 more, split
     * over two input and two output buffers, multiple frames: 3 + 8 frames,
     * then input runs out (PARTIAL_INPUT, as shadPS4 reports it). */
    ajm_buffer in[2] = {{g_input + g_frame_size, 1000},
                        {g_input + g_frame_size + 1000, SUPERFRAMES * SUPERFRAME - g_frame_size - 1000}};
    ajm_buffer out[2] = {{g_output, 5000}, {g_output + 5000, sizeof(g_output) - 5000}};
    struct { sb_result r; sb_stream s; uint32_t mframe[2]; } side3;
    memset(g_output, 0x55, sizeof(g_output));
    end = sceAjmBatchJobRunSplitBufferRa(g_batch, inst, JOB(RUN_MULTI, 0, SB_STREAM), in, 2, out, 2,
                                         &side3, sizeof(side3), NULL);
    SUBMIT(end);
    CHECK(side3.r.result == RESULT_PARTIAL_INPUT);
    CHECK(side3.mframe[0] == 11);
    CHECK(side3.s.consumed == (int32_t)(in[0].size + in[1].size));
    CHECK(side3.s.written == 11 * 1024);
    CHECK(side3.s.total == 12 * 256);
    CHECK(memcmp(g_output, g_ref + 1024, 11 * 1024) == 0 && g_output[11 * 1024] == 0x55);

    /* Not enough room for a frame: nothing consumed. */
    struct { sb_result r; sb_stream s; } side4;
    end = sceAjmBatchJobRunBufferRa(g_batch, inst, JOB(0, 0, SB_STREAM), g_input, SUPERFRAME,
                                    g_output, 1000, &side4, sizeof(side4), NULL);
    SUBMIT(end);
    CHECK(side4.r.result == RESULT_NOT_ENOUGH_ROOM && side4.s.consumed == 0 && side4.s.written == 0);

    /* Partial superframe: nothing consumed. */
    end = sceAjmBatchJobRunBufferRa(g_batch, inst, JOB(0, 0, SB_STREAM), g_input, SUPERFRAME - 1,
                                    g_output, sizeof(g_output), &side4, sizeof(side4), NULL);
    SUBMIT(end);
    CHECK(side4.r.result == RESULT_PARTIAL_INPUT && side4.s.consumed == 0);

    /* Data LibAtrac9 rejects (all zeros: scale factors out of range): a fatal
     * codec error with its status, nothing consumed or written. */
    static uint8_t zeros[SUPERFRAME];
    memset(&side4, 0xCC, sizeof(side4));
    end = sceAjmBatchJobRunBufferRa(g_batch, inst, JOB(RUN_MULTI, 0, SB_STREAM), zeros, SUPERFRAME,
                                    g_output, sizeof(g_output), &side4, sizeof(side4), NULL);
    SUBMIT(end);
    CHECK((uint32_t)side4.r.result == (RESULT_CODEC_ERROR | RESULT_FATAL));
    CHECK(side4.r.internal != 0);
    CHECK(side4.s.consumed == 0 && side4.s.written == 0);

    /* Gapless: reset (a fresh decoder), 300 samples after skipping 100. Frame
     * 1 gives 156, frame 2 gives 144 and ends the stream; frames 3 and 4 are
     * decoded and dropped (shadPS4 counts them) and the padding skipped; then
     * the input is gone. */
    sb_gapless gapless = {300, 100, 0};
    struct { sb_result r; sb_gapless g; } side5;
    end = sceAjmBatchJobControlBufferRa(g_batch, inst, JOB(0, CTL_RESET, SB_GAPLESS), &gapless,
                                        sizeof(gapless), &side5, sizeof(side5), NULL);
    SUBMIT(end);
    CHECK(side5.r.result == 0 && side5.g.total == 300 && side5.g.skip == 100);
    struct { sb_result r; sb_stream s; sb_gapless g; uint32_t mframe[2]; } side6;
    memset(g_output, 0x55, sizeof(g_output));
    end = sceAjmBatchJobRunBufferRa(g_batch, inst, JOB(RUN_MULTI, 0, SB_STREAM | SB_GAPLESS), g_input,
                                    SUPERFRAME, g_output, sizeof(g_output), &side6, sizeof(side6), NULL);
    SUBMIT(end);
    CHECK(side6.r.result == RESULT_PARTIAL_INPUT);
    CHECK(side6.mframe[0] == 4);
    CHECK(side6.s.consumed == (int32_t)SUPERFRAME && side6.s.written == 300 * 4 && side6.s.total == 300);
    CHECK(side6.g.total == 0 && side6.g.skip == 0 && side6.g.skipped == 100 + 112);
    CHECK(memcmp(g_output, g_ref + 100 * 4, 156 * 4) == 0);
    CHECK(memcmp(g_output + 156 * 4, g_ref + 1024, 144 * 4) == 0 && g_output[300 * 4] == 0x55);

    /* Past the end the decoder stays in step (the drain kept its frame index):
     * the next superframe decodes without an error and writes nothing. */
    end = sceAjmBatchJobRunBufferRa(g_batch, inst, JOB(RUN_MULTI, 0, SB_STREAM | SB_GAPLESS),
                                    g_input + SUPERFRAME, SUPERFRAME, g_output, sizeof(g_output),
                                    &side6, sizeof(side6), NULL);
    SUBMIT(end);
    CHECK(side6.r.result == RESULT_PARTIAL_INPUT && side6.mframe[0] == 4);
    CHECK(side6.s.consumed == (int32_t)SUPERFRAME && side6.s.written == 0);

    /* After a reset that also clears the gapless settings, a whole superframe
     * decodes from the start again. */
    sb_gapless no_gapless = {0, 0, 0};
    end = sceAjmBatchJobControlBufferRa(g_batch, inst, JOB(0, CTL_RESET, SB_GAPLESS), &no_gapless,
                                        sizeof(no_gapless), &side5, sizeof(side5), NULL);
    SUBMIT(end);
    end = sceAjmBatchJobRunBufferRa(g_batch, inst, JOB(RUN_MULTI, 0, SB_STREAM), g_input, SUPERFRAME,
                                    g_output, sizeof(g_output), &side4, sizeof(side4), NULL);
    SUBMIT(end);
    CHECK(side4.r.result == RESULT_PARTIAL_INPUT && side4.s.written == 4 * 1024);
    CHECK(memcmp(g_output, g_ref, 4 * 1024) == 0);

    /* F32 output: 4-byte samples, LibAtrac9's floats (PCM / 32767). */
    uint32_t inst_f = 0, inst_s = 0;
    CHECK(sceAjmInstanceCreate(ctx, 1, 1 | 2 << 3 | 2 << 7, &inst_f) == 0);
    struct { sb_result r; sb_format f; } side7;
    end = sceAjmBatchJobControlBufferRa(g_batch, inst_f, JOB(0, CTL_INIT, SB_FORMAT), (void *)k_config,
                                        8, &side7, sizeof(side7), NULL);
    SUBMIT(end);
    CHECK(side7.r.result == 0 && side7.f.encoding == 2 && side7.f.channels == 2);
    uint32_t ref_size = reference(2, 0, 4 * SUPERFRAMES, g_ref);
    CHECK(ref_size == 4 * SUPERFRAMES * 2048);
    struct { sb_result r; sb_stream s; uint32_t mframe[2]; } side8;
    end = sceAjmBatchJobRunBufferRa(g_batch, inst_f, JOB(RUN_MULTI, 0, SB_STREAM), g_input,
                                    sizeof(g_input), g_output, sizeof(g_output), &side8, sizeof(side8), NULL);
    SUBMIT(end);
    CHECK(side8.r.result == RESULT_PARTIAL_INPUT && side8.mframe[0] == 4 * SUPERFRAMES);
    CHECK(side8.s.consumed == (int32_t)sizeof(g_input) && side8.s.written == (int32_t)ref_size);
    CHECK(memcmp(g_output, g_ref, ref_size) == 0);

    /* S32, planar (codec flag NonInterleavedOutput): one plane per channel. */
    CHECK(sceAjmInstanceCreate(ctx, 1, 1 | 2 << 3 | 1 << 7 | 0x100ull << 32, &inst_s) == 0);
    end = sceAjmBatchJobControlBufferRa(g_batch, inst_s, JOB(0, CTL_INIT, 0), (void *)k_config, 8,
                                        &side7, sizeof(side7), NULL);
    SUBMIT(end);
    reference(1, 1, 1, g_ref);
    end = sceAjmBatchJobRunBufferRa(g_batch, inst_s, JOB(0, 0, SB_STREAM), g_input, SUPERFRAME,
                                    g_output, sizeof(g_output), &side4, sizeof(side4), NULL);
    SUBMIT(end);
    CHECK(side4.r.result == 0 && side4.s.written == 2048);
    CHECK(memcmp(g_output, g_ref, 2048) == 0);

    /* A config LibAtrac9 can't take: the instance stays uninitialized. */
    end = sceAjmBatchJobControlBufferRa(g_batch, inst_s, JOB(0, CTL_RESET | CTL_INIT, 0),
                                        (void *)k_config_bad, 8, &side7, sizeof(side7), NULL);
    SUBMIT(end);
    end = sceAjmBatchJobRunBufferRa(g_batch, inst_s, JOB(0, 0, SB_STREAM), g_input, SUPERFRAME,
                                    g_output, sizeof(g_output), &side4, sizeof(side4), NULL);
    SUBMIT(end);
    CHECK(side4.r.result == RESULT_NOT_INITIALIZED && side4.s.consumed == 0);
    CHECK(sceAjmInstanceDestroy(ctx, inst_f) == 0);
    CHECK(sceAjmInstanceDestroy(ctx, inst_s) == 0);

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
